// kzipu_test: decodes real Killzone FMV I-frames through kzipu (PCSX2's IPU) the way a PS2 player drives the hardware,
// and compares them with FFmpeg's decode of the same frames.
//
//   1. Reads the disc image (ISO9660), finds FILES*.DAT, and locates PSS videos inside them (an MPEG-2 pack header
//      immediately followed by a system header marks the start of each video).
//   2. Demuxes the video elementary stream (PES stream 0xE0) from the first few MB of a video.
//   3. Places [default intra matrix][flat non-intra matrix][video ES] in emulated EE memory, starts the IPU_TO DMA
//      (ch4, chain mode, REF/REFE tags) and parses the stream *through the IPU*: FDEC to read start codes and header
//      fields, SETIQ to load quantizer matrices straight from the bitstream, VDEC to decode each slice's first
//      macroblock address increment, and IDEC (RGB32 output, CSC included) per slice with IPU_CTRL set from the
//      picture coding extension. Decoded macroblocks leave through the IPU_FROM DMA (ch3, normal mode, re-armed on
//      each ch3 DMA-end interrupt). P/B pictures are skipped over with FDEC.
//   4. Decodes the same ES with FFmpeg (libavcodec mpeg2video), converts its I-frames to RGB with the IPU's documented
//      integer BT.601 CSC, and computes PSNR per I-frame. Pass: every I-frame >= 30 dB.
//   5. Video 0 again with host-driven FIFOs (hostDrainsOutput, kzipuWriteInFifo / kzipuReadOutFifo, no DMA), and again
//      advancing only with kzipuRun(256): both must be bit-identical to step 3.
//   6. Video 0 with BDEC per macroblock (macroblock address/type/quant parsed with VDEC/FDEC, RAW16 YCbCr out): Y/Cb/Cr
//      PSNR against FFmpeg's planes >= 30 dB. Each BDEC picture is then sent through the CSC command (RAW8 in via a
//      normal-mode IPU_TO DMA) and must reproduce the IDEC picture.
//   7. Writes PNGs to test/out/: IPU | FFmpeg | |difference| x8, for the most detailed I-frame of each video.
//
// Usage: kzipu_test [--iso <path>] [--out <dir>] [--videos N] [--frames N] [--es-mb N]
// Exit code 0 = pass.
// SPDX-License-Identifier: GPL-3.0+

#include "kzipu.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_failures = 0;

static void Fail(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	std::printf("FAIL: ");
	std::vprintf(fmt, ap);
	std::printf("\n");
	va_end(ap);
	g_failures++;
}

// ---------------------------------------------------------------------------------------------------------------------
// Disc image: ISO9660 root directory -> FILES*.DAT extents
// ---------------------------------------------------------------------------------------------------------------------
struct IsoFile
{
	std::string name;
	uint64_t offset;
	uint64_t size;
};

static bool ReadAt(FILE* f, uint64_t off, void* dst, size_t n)
{
	if (_fseeki64(f, static_cast<__int64>(off), SEEK_SET) != 0)
		return false;
	return std::fread(dst, 1, n, f) == n;
}

static std::vector<IsoFile> ListRoot(FILE* f)
{
	std::vector<IsoFile> out;
	uint8_t pvd[2048];
	if (!ReadAt(f, 16 * 2048ull, pvd, sizeof(pvd)) || std::memcmp(pvd + 1, "CD001", 5) != 0)
		return out;
	const uint8_t* root = pvd + 156;
	uint32_t lba, size;
	std::memcpy(&lba, root + 2, 4);
	std::memcpy(&size, root + 10, 4);
	std::vector<uint8_t> dir((size + 2047) / 2048 * 2048);
	if (!ReadAt(f, lba * 2048ull, dir.data(), dir.size()))
		return out;
	for (size_t i = 0; i < size;)
	{
		const uint8_t len = dir[i];
		if (len == 0)
		{
			i = (i / 2048 + 1) * 2048;
			continue;
		}
		uint32_t elba, esize;
		std::memcpy(&elba, &dir[i + 2], 4);
		std::memcpy(&esize, &dir[i + 10], 4);
		const uint8_t nl = dir[i + 32];
		std::string name(reinterpret_cast<const char*>(&dir[i + 33]), nl);
		if (auto semi = name.find(';'); semi != std::string::npos)
			name.resize(semi);
		if (!(dir[i + 25] & 2))
			out.push_back({name, elba * 2048ull, esize});
		i += len;
	}
	return out;
}

// A PSS video starts with an MPEG-2 pack header (00 00 01 BA, '01' marker bits) followed directly by a system header
// (00 00 01 BB). Later packs of the same video carry no system header.
static std::vector<uint64_t> FindVideos(FILE* f, const IsoFile& file, size_t maxCount)
{
	std::vector<uint64_t> found;
	constexpr size_t kChunk = 16u << 20;
	std::vector<uint8_t> buf(kChunk + 64);
	for (uint64_t off = 0; off < file.size && found.size() < maxCount; off += kChunk)
	{
		const size_t n = static_cast<size_t>(std::min<uint64_t>(kChunk + 64, file.size - off));
		if (!ReadAt(f, file.offset + off, buf.data(), n))
			break;
		for (size_t i = 0; i + 32 <= n && i < kChunk; i++)
		{
			if (buf[i] != 0 || buf[i + 1] != 0 || buf[i + 2] != 1 || buf[i + 3] != 0xBA)
				continue;
			if ((buf[i + 4] & 0xC4) != 0x44)
				continue;
			const size_t sys = i + 14 + (buf[i + 13] & 7);
			if (sys + 4 <= n && buf[sys] == 0 && buf[sys + 1] == 0 && buf[sys + 2] == 1 && buf[sys + 3] == 0xBB)
			{
				found.push_back(file.offset + off + i);
				if (found.size() >= maxCount)
					break;
				i += 2047; // PSS packs are 2048 bytes; the next video cannot start inside this pack
			}
		}
	}
	return found;
}

// Demuxes the MPEG-2 program stream from `start` and returns the video (0xE0) elementary stream, cut at the last
// picture start code so it ends on a complete picture, followed by a sequence_end_code.
static std::vector<uint8_t> DemuxVideo(FILE* f, uint64_t start, size_t maxEs)
{
	std::vector<uint8_t> es;
	std::vector<uint8_t> ps(maxEs + maxEs / 4 + (1 << 20));
	if (!ReadAt(f, start, ps.data(), ps.size()))
	{
		// Near the end of the image: read what is there.
		_fseeki64(f, static_cast<__int64>(start), SEEK_SET);
		ps.resize(std::fread(ps.data(), 1, ps.size(), f));
	}
	size_t p = 0;
	while (p + 6 <= ps.size() && es.size() < maxEs)
	{
		if (!(ps[p] == 0 && ps[p + 1] == 0 && ps[p + 2] == 1))
		{
			p++; // resync
			continue;
		}
		const uint8_t sid = ps[p + 3];
		if (sid == 0xB9)
			break;
		if (sid == 0xBA)
		{
			p += ((ps[p + 4] & 0xC0) == 0x40) ? 14 + (ps[p + 13] & 7) : 12;
			continue;
		}
		if (sid < 0xBB)
		{
			p++;
			continue;
		}
		const size_t len = (static_cast<size_t>(ps[p + 4]) << 8) | ps[p + 5];
		if (p + 6 + len > ps.size())
			break;
		if (sid == 0xE0 && (ps[p + 6] & 0xC0) == 0x80)
		{
			const size_t hdr = 9 + ps[p + 8];
			es.insert(es.end(), ps.begin() + p + hdr, ps.begin() + p + 6 + len);
		}
		p += 6 + len;
	}
	for (size_t i = es.size() >= 4 ? es.size() - 4 : 0; i > 0; i--)
	{
		if (es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 1 && es[i + 3] == 0x00)
		{
			es.resize(i);
			break;
		}
	}
	const uint8_t seq_end[4] = {0, 0, 1, 0xB7};
	es.insert(es.end(), seq_end, seq_end + 4);
	return es;
}

// ---------------------------------------------------------------------------------------------------------------------
// Pictures
// ---------------------------------------------------------------------------------------------------------------------
struct Rgba
{
	int w = 0, h = 0;
	std::vector<uint8_t> px; // RGBA8
};

// The IPU's CSC (yuv2rgb_reference in PCSX2's IPU/yuv2rgb.cpp: BT.601, integer, documented in the EE manual).
static void IpuCsc(int y, int cb, int cr, uint8_t* out)
{
	const int lum = (0x95 * std::max(0, y - 16)) >> 6;
	const int rcr = (0xcc * (cr - 128)) >> 6;
	const int gcr = (-0x68 * (cr - 128)) >> 6;
	const int gcb = (-0x32 * (cb - 128)) >> 6;
	const int bcb = (0x102 * (cb - 128)) >> 6;
	out[0] = static_cast<uint8_t>(std::clamp((lum + rcr + 1) >> 1, 0, 255));
	out[1] = static_cast<uint8_t>(std::clamp((lum + gcr + gcb + 1) >> 1, 0, 255));
	out[2] = static_cast<uint8_t>(std::clamp((lum + bcb + 1) >> 1, 0, 255));
	out[3] = 0x80;
}

static double LumaStdDev(const Rgba& img)
{
	double s = 0, s2 = 0;
	const size_t n = static_cast<size_t>(img.w) * img.h;
	for (size_t i = 0; i < n; i++)
	{
		const double l = 0.299 * img.px[4 * i] + 0.587 * img.px[4 * i + 1] + 0.114 * img.px[4 * i + 2];
		s += l;
		s2 += l * l;
	}
	const double m = s / n;
	return std::sqrt(std::max(0.0, s2 / n - m * m));
}

struct Compare
{
	double psnr;
	int maxDiff;
	double pctOver2;
};

static Compare CompareRgb(const Rgba& a, const Rgba& b)
{
	double se = 0;
	int maxd = 0;
	size_t over = 0;
	const size_t n = static_cast<size_t>(a.w) * a.h;
	for (size_t i = 0; i < n; i++)
	{
		int pixmax = 0;
		for (int c = 0; c < 3; c++)
		{
			const int d = std::abs(int(a.px[4 * i + c]) - int(b.px[4 * i + c]));
			se += double(d) * d;
			pixmax = std::max(pixmax, d);
		}
		maxd = std::max(maxd, pixmax);
		if (pixmax > 2)
			over++;
	}
	const double mse = se / (3.0 * n);
	return {mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse), maxd, 100.0 * over / n};
}

// ---------------------------------------------------------------------------------------------------------------------
// PNG writer (stored deflate blocks; no compression library needed)
// ---------------------------------------------------------------------------------------------------------------------
static uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0)
{
	static uint32_t table[256];
	static bool init = false;
	if (!init)
	{
		for (uint32_t i = 0; i < 256; i++)
		{
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		init = true;
	}
	crc = ~crc;
	for (size_t i = 0; i < n; i++)
		crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

static bool WritePng(const fs::path& path, int w, int h, const std::vector<uint8_t>& rgb)
{
	std::vector<uint8_t> raw;
	raw.reserve(static_cast<size_t>(h) * (1 + 3 * w));
	for (int y = 0; y < h; y++)
	{
		raw.push_back(0);
		raw.insert(raw.end(), rgb.begin() + static_cast<size_t>(y) * w * 3, rgb.begin() + static_cast<size_t>(y + 1) * w * 3);
	}
	std::vector<uint8_t> z = {0x78, 0x01};
	uint32_t a = 1, b = 0;
	for (uint8_t v : raw)
	{
		a = (a + v) % 65521;
		b = (b + a) % 65521;
	}
	for (size_t off = 0; off < raw.size(); off += 65535)
	{
		const size_t len = std::min<size_t>(65535, raw.size() - off);
		z.push_back(off + len == raw.size() ? 1 : 0);
		z.push_back(len & 0xFF);
		z.push_back(len >> 8);
		z.push_back(~len & 0xFF);
		z.push_back((~len >> 8) & 0xFF);
		z.insert(z.end(), raw.begin() + off, raw.begin() + off + len);
	}
	const uint32_t adler = (b << 16) | a;
	for (int s = 24; s >= 0; s -= 8)
		z.push_back((adler >> s) & 0xFF);

	FILE* f = std::fopen(path.string().c_str(), "wb");
	if (!f)
		return false;
	auto be32 = [&](uint32_t v) {
		const uint8_t d[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
		std::fwrite(d, 1, 4, f);
	};
	auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
		be32(static_cast<uint32_t>(data.size()));
		std::vector<uint8_t> td(type, type + 4);
		td.insert(td.end(), data.begin(), data.end());
		std::fwrite(td.data(), 1, td.size(), f);
		be32(Crc32(td.data(), td.size()));
	};
	const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
	std::fwrite(sig, 1, 8, f);
	std::vector<uint8_t> ihdr = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
		uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0};
	chunk("IHDR", ihdr);
	chunk("IDAT", z);
	chunk("IEND", {});
	std::fclose(f);
	return true;
}

static bool WriteSideBySide(const fs::path& path, const Rgba& ipu, const Rgba& ref)
{
	const int w = ipu.w, h = ipu.h, W = 3 * w + 16;
	std::vector<uint8_t> rgb(static_cast<size_t>(W) * h * 3, 40);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
		{
			const size_t s = (static_cast<size_t>(y) * w + x) * 4;
			for (int c = 0; c < 3; c++)
			{
				rgb[(static_cast<size_t>(y) * W + x) * 3 + c] = ipu.px[s + c];
				rgb[(static_cast<size_t>(y) * W + w + 8 + x) * 3 + c] = ref.px[s + c];
				const int d = std::abs(int(ipu.px[s + c]) - int(ref.px[s + c])) * 8;
				rgb[(static_cast<size_t>(y) * W + 2 * w + 16 + x) * 3 + c] = static_cast<uint8_t>(std::min(255, d));
			}
		}
	return WritePng(path, W, h, rgb);
}

// ---------------------------------------------------------------------------------------------------------------------
// FFmpeg reference decode: every I-frame, in display order (4:2:0 planes)
// ---------------------------------------------------------------------------------------------------------------------
struct Yuv
{
	int w = 0, h = 0;
	std::vector<uint8_t> y, cb, cr; // w*h, (w/2)*(h/2), (w/2)*(h/2)
};

// Converts with the IPU's CSC, so an IPU RGB32 picture and a reference picture differ only by decoding differences.
static Rgba YuvToRgb(const Yuv& f)
{
	Rgba img;
	img.w = f.w;
	img.h = f.h;
	img.px.resize(static_cast<size_t>(f.w) * f.h * 4);
	const int cw = f.w / 2;
	for (int y = 0; y < f.h; y++)
		for (int x = 0; x < f.w; x++)
			IpuCsc(f.y[static_cast<size_t>(y) * f.w + x], f.cb[(y >> 1) * cw + (x >> 1)], f.cr[(y >> 1) * cw + (x >> 1)],
				&img.px[(static_cast<size_t>(y) * f.w + x) * 4]);
	return img;
}

static double PlanePsnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
	double se = 0;
	for (size_t i = 0; i < a.size(); i++)
	{
		const double d = double(a[i]) - double(b[i]);
		se += d * d;
	}
	const double mse = se / a.size();
	return mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

static std::vector<Yuv> FfmpegIFrames(const std::vector<uint8_t>& es)
{
	std::vector<Yuv> out;
	const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
	AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
	AVCodecContext* ctx = avcodec_alloc_context3(codec);
	if (!codec || !parser || !ctx || avcodec_open2(ctx, codec, nullptr) < 0)
	{
		Fail("FFmpeg mpeg2video decoder unavailable");
		return out;
	}
	AVPacket* pkt = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();

	auto drain = [&]() {
		while (avcodec_receive_frame(ctx, frame) == 0)
		{
			if (frame->pict_type == AV_PICTURE_TYPE_I && frame->format == AV_PIX_FMT_YUV420P)
			{
				Yuv f;
				f.w = frame->width;
				f.h = frame->height;
				const int cw = f.w / 2, ch = f.h / 2;
				f.y.resize(static_cast<size_t>(f.w) * f.h);
				f.cb.resize(static_cast<size_t>(cw) * ch);
				f.cr.resize(static_cast<size_t>(cw) * ch);
				for (int y = 0; y < f.h; y++)
					std::memcpy(&f.y[static_cast<size_t>(y) * f.w], frame->data[0] + y * frame->linesize[0], f.w);
				for (int y = 0; y < ch; y++)
				{
					std::memcpy(&f.cb[static_cast<size_t>(y) * cw], frame->data[1] + y * frame->linesize[1], cw);
					std::memcpy(&f.cr[static_cast<size_t>(y) * cw], frame->data[2] + y * frame->linesize[2], cw);
				}
				out.push_back(std::move(f));
			}
			av_frame_unref(frame);
		}
	};

	std::vector<uint8_t> data(es);
	data.resize(es.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
	const uint8_t* p = data.data();
	int left = static_cast<int>(es.size());
	while (left > 0)
	{
		const int used = av_parser_parse2(parser, ctx, &pkt->data, &pkt->size, p, left, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
		if (used < 0)
			break;
		p += used;
		left -= used;
		if (pkt->size && avcodec_send_packet(ctx, pkt) == 0)
			drain();
	}
	av_parser_parse2(parser, ctx, &pkt->data, &pkt->size, nullptr, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
	if (pkt->size && avcodec_send_packet(ctx, pkt) == 0)
		drain();
	avcodec_send_packet(ctx, nullptr);
	drain();

	av_frame_free(&frame);
	av_packet_free(&pkt);
	av_parser_close(parser);
	avcodec_free_context(&ctx);
	return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Emulated EE side: 32 MB of RDRAM, DMA address translation, interrupt counters
// ---------------------------------------------------------------------------------------------------------------------
static constexpr uint32_t IPU_CMD = 0x10002000, IPU_CTRL = 0x10002010, IPU_BP = 0x10002020, IPU_TOP = 0x10002030;
static constexpr uint32_t D3_CHCR = 0x1000B000, D3_MADR = 0x1000B010, D3_QWC = 0x1000B020;
static constexpr uint32_t D4_CHCR = 0x1000B400, D4_QWC = 0x1000B420, D4_TADR = 0x1000B430;

static constexpr uint32_t kRamSize = 32u << 20;
static constexpr uint32_t kTagAddr = 0x00100000; // IPU_TO DMA chain (REF tags)
static constexpr uint32_t kInAddr = 0x00200000;  // matrices + video ES
static constexpr uint32_t kOutAddr = 0x01800000; // IPU_FROM destination: one picture of RGB32 macroblocks

static std::vector<uint8_t> g_ram(kRamSize);
static int g_intc = 0, g_dmaEnd3 = 0, g_dmaEnd4 = 0, g_busErr = 0;

static uint8_t* HostDmaPtr(void*, uint32_t addr, uint32_t bytes, bool)
{
	if (addr & 0x80000000u) // no scratchpad in this test
		return nullptr;
	addr &= 0x1FFFFFF0u;
	if (static_cast<uint64_t>(addr) + bytes > kRamSize)
		return nullptr;
	return &g_ram[addr];
}

// MPEG-2 default intra quantizer matrix (ISO/IEC 13818-2 6.3.11), raster order; the IPU wants bitstream (zigzag) order.
static const uint8_t kDefaultIntraRaster[64] = {
	8, 16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37,
	19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
	22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
	26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};
static const uint8_t kZigzag[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// ---------------------------------------------------------------------------------------------------------------------
// The "player": parses the stream through the IPU and decodes I-pictures with IDEC (one command per slice, RGB32 out)
// or BDEC (one command per macroblock, RAW16 YCbCr out; macroblock address/type/quant parsed by the player with VDEC and
// FDEC, as a software MPEG decoder on the EE does).
// ---------------------------------------------------------------------------------------------------------------------
class Player
{
public:
	enum class Mode { Dma, HostFifo };
	enum class Decoder { Idec, Bdec };

	Player(Mode mode, Decoder dec, const std::vector<uint8_t>& es, size_t maxFrames)
		: m_mode(mode), m_dec(dec), m_maxFrames(maxFrames)
	{
		// Input = default intra matrix (zigzag) + flat non-intra matrix + ES, padded to whole qwords.
		m_input.resize(128);
		for (int i = 0; i < 64; i++)
			m_input[i] = kDefaultIntraRaster[kZigzag[i]];
		std::fill(m_input.begin() + 64, m_input.begin() + 128, 16);
		m_input.insert(m_input.end(), es.begin(), es.end());
		m_input.resize((m_input.size() + 15 + 64) & ~size_t(15), 0);
	}

	std::vector<Rgba> frames;                   // Idec: RGB32 pictures
	std::vector<Yuv> yuvFrames;                 // Bdec: YCbCr pictures
	std::vector<std::vector<uint8_t>> raw8;     // Bdec: the same pictures as RAW8 macroblocks (CSC input)
	std::string error;
	uint64_t commands = 0, decodes = 0, dmaRearms = 0;
	uint32_t timedSlice = 0;
	bool pcsx2IdecQsc = false;                  // KzipuConfig::mapIdecQsc = false (PCSX2's unmapped IDEC QSC)                    // != 0: advance kzipu with kzipuRun(timedSlice) instead of kzipuStep()

	void Init()
	{
		KzipuConfig cfg;
		cfg.intc = [](void*) { g_intc++; };
		cfg.dmacIrq = [](void*, int ch) {
			if (ch == 3) g_dmaEnd3++;
			else if (ch == 4) g_dmaEnd4++;
			else g_busErr++;
		};
		cfg.dmaPtr = HostDmaPtr;
		cfg.log = [](void*, int level, const char* msg) { std::printf("  [kzipu %s] %s\n", level == 2 ? "error" : level == 1 ? "warn" : "info", msg); };
		cfg.hostDrainsOutput = (m_mode == Mode::HostFifo);
		cfg.mapIdecQsc = !pcsx2IdecQsc;
		kzipuInit(cfg);

		kzipuWriteReg32(IPU_CTRL, 0x40000000); // RST
		kzipuWriteReg32(IPU_CMD, 0x00000000);  // BCLR, BP=0
	}

	bool Run()
	{
		Init();
		if (m_mode == Mode::Dma)
		{
			if (kInAddr + m_input.size() > kOutAddr)
				return Err("input too large for the emulated RAM layout");
			std::memcpy(&g_ram[kInAddr], m_input.data(), m_input.size());
			// IPU_TO chain: REF tags of up to 0x8000 qwords, REFE for the last one.
			uint32_t qwLeft = static_cast<uint32_t>(m_input.size() / 16), addr = kInAddr, t = kTagAddr;
			while (qwLeft)
			{
				const uint32_t qwc = std::min<uint32_t>(qwLeft, 0x8000);
				qwLeft -= qwc;
				const uint64_t id = qwLeft ? 3 /*REF*/ : 0 /*REFE*/;
				const uint64_t tag = qwc | (id << 28) | (static_cast<uint64_t>(addr) << 32);
				std::memcpy(&g_ram[t], &tag, 8);
				std::memset(&g_ram[t + 8], 0, 8);
				addr += qwc * 16;
				t += 16;
			}
			kzipuWriteReg32(D4_QWC, 0);
			kzipuWriteReg32(D4_TADR, kTagAddr);
			kzipuWriteReg32(D4_CHCR, 0x105); // STR | chain | from memory
		}

		// Default matrices (the stream only carries the ones that differ from MPEG-2's defaults).
		if (!Cmd(0x50000000) || !Cmd(0x58000000))
			return false;
		if (!Fdec(0))
			return false;
		return Parse();
	}

	// CSC command: RAW8 macroblocks in through a normal-mode IPU_TO DMA, RGB32 out through IPU_FROM.
	bool RunCsc(const std::vector<uint8_t>& mbs, int w, int h, Rgba& img)
	{
		Init();
		m_w = w;
		m_h = h;
		m_mbw = (w + 15) / 16;
		m_mbh = (h + 15) / 16;
		m_qwPerMb = 64;
		m_inI = true;
		m_outQwArmed = 0;
		const uint32_t mbc = static_cast<uint32_t>(m_mbw * m_mbh);
		if (mbs.size() != mbc * 384u || mbc > 2047)
			return Err("bad CSC input");
		std::memcpy(&g_ram[kInAddr], mbs.data(), mbs.size());
		kzipuWriteReg32(0x1000B410, kInAddr);                          // D4_MADR
		kzipuWriteReg32(D4_QWC, static_cast<uint32_t>(mbs.size() / 16));
		kzipuWriteReg32(D4_CHCR, 0x101);                               // STR | normal | from memory
		if (!Cmd(0x70000000u | mbc))                                   // CSC, RGB32, no dither
			return false;
		if (!DrainOutput())
			return false;
		if (MbReceived() != mbc)
			return Err("CSC produced " + std::to_string(MbReceived()) + " of " + std::to_string(mbc) + " macroblocks");
		img = MbsToRgba(std::vector<uint8_t>(g_ram.begin() + kOutAddr, g_ram.begin() + kOutAddr + mbc * 1024u));
		m_inI = false;
		return error.empty();
	}

private:
	Mode m_mode;
	Decoder m_dec;
	size_t m_maxFrames;
	std::vector<uint8_t> m_input;
	size_t m_inPos = 0;           // HostFifo: bytes pushed so far
	uint32_t m_cur = 0;           // next 32 bits of the stream, as IPU_CMD.DATA/IPU_TOP report them

	// picture state
	int m_w = 0, m_h = 0, m_mbw = 0, m_mbh = 0;
	bool m_mpeg1 = true, m_customIntra = false;
	int m_pictType = 0, m_idp = 0, m_ps = 3, m_fpfd = 1, m_qst = 0, m_ivf = 0, m_as = 0;
	bool m_inI = false;
	uint32_t m_mbDone = 0;        // macroblocks of the current I-picture whose output has been received
	uint32_t m_outQwArmed = 0;    // Dma: qwords handed to ch3 for this picture so far
	uint32_t m_qwPerMb = 64;      // output per macroblock: 64 (RGB32) or 48 (BDEC RAW16)
	std::vector<uint8_t> m_out;   // output macroblocks of the current picture, in decode order

	bool Err(const std::string& e)
	{
		if (error.empty())
		{
			const KzipuStatus s = kzipuGetStatus();
			char buf[256];
			std::snprintf(buf, sizeof(buf), " [busy=%d cmd=%u IFC=%u OFC=%u BP=%u waitIn=%d waitOut=%d ch3=%d ch4=%d ev=0x%x]",
				s.busy, s.command, s.inFifoQwc, s.outFifoQwc, s.bitPointer, s.waitingForInput, s.waitingForOutput,
				s.fromIpuActive, s.toIpuActive, s.pendingEvents);
			error = e + buf;
		}
		return false;
	}

	uint32_t ExpectedOutQw() const { return static_cast<uint32_t>(m_mbw * m_mbh) * m_qwPerMb; }

	// Moves data and runs IPU events once. Returns true if anything happened.
	bool Pump()
	{
		bool moved = false;
		if (m_mode == Mode::HostFifo)
		{
			if (m_inPos < m_input.size())
			{
				const uint32_t qw = static_cast<uint32_t>(std::min<size_t>(8, (m_input.size() - m_inPos) / 16));
				const uint32_t n = kzipuWriteInFifo(&m_input[m_inPos], qw);
				m_inPos += n * 16;
				moved |= n != 0;
			}
			alignas(16) uint8_t q[8 * 16];
			const uint32_t n = kzipuReadOutFifo(q, 8);
			if (n)
			{
				if (!m_inI)
				{
					Err("output data outside an I-picture");
					return true;
				}
				m_out.insert(m_out.end(), q, q + 16 * n);
				moved = true;
			}
		}
		else if (m_inI && !(kzipuReadReg32(D3_CHCR) & 0x100) && m_outQwArmed < ExpectedOutQw())
		{
			// IPU_FROM: (re)arm ch3 for the next part of the picture, 4 macroblock rows at a time.
			const uint32_t qwc = std::min<uint32_t>(ExpectedOutQw() - m_outQwArmed, m_qwPerMb * m_mbw * 4);
			kzipuWriteReg32(D3_MADR, kOutAddr + m_outQwArmed * 16);
			kzipuWriteReg32(D3_QWC, qwc);
			kzipuWriteReg32(D3_CHCR, 0x100); // STR, normal mode, to memory
			m_outQwArmed += qwc;
			dmaRearms++;
			moved = true;
		}
		if (timedSlice)
			moved |= kzipuRun(timedSlice) != 0 || kzipuGetStatus().pendingEvents != 0; // time passing is progress
		else
			moved |= kzipuStep() != 0;
		return moved;
	}

	bool WaitIdle()
	{
		int idle = 0;
		while (kzipuReadReg32(IPU_CTRL) & 0x80000000u)
		{
			if (!error.empty())
				return false;
			if (Pump())
				idle = 0;
			else if (++idle > 16)
				return Err("IPU command stalled");
		}
		return true;
	}

	bool Cmd(uint32_t v)
	{
		commands++;
		kzipuWriteReg32(IPU_CMD, v);
		return WaitIdle();
	}

	bool Fdec(uint32_t skipBits)
	{
		if (!Cmd(0x40000000u | skipBits))
			return false;
		const uint64_t r = kzipuReadReg64(IPU_CMD);
		if (r >> 63)
			return Err("IPU_CMD still busy after FDEC");
		m_cur = static_cast<uint32_t>(r);
		return true;
	}

	uint32_t Show(int n) const { return m_cur >> (32 - n); }
	bool Skip(int n) { return Fdec(static_cast<uint32_t>(n)); }
	bool Get(int n, uint32_t& v)
	{
		v = Show(n);
		return Skip(n);
	}

	bool NextStartCode(uint32_t& code)
	{
		const uint32_t bp = kzipuReadReg32(IPU_BP) & 0x7F;
		if (bp & 7)
			if (!Skip(8 - (bp & 7)))
				return false;
		while ((m_cur >> 8) != 1)
			if (!Skip(8))
				return false;
		code = m_cur & 0xFF;
		return true;
	}

	// SETIQ straight from the bitstream: FB=1 skips the load_*_quantiser_matrix flag, then 64 bytes are read.
	bool LoadMatrixIfFlagged(bool intra)
	{
		if (Show(1))
		{
			if (!Cmd(0x50000001u | (intra ? 0 : 0x08000000u)))
				return false;
			if (intra)
				m_customIntra = true;
			return Fdec(0);
		}
		if (intra && m_customIntra)
			return Err("stream reverts to the default intra matrix; not handled by this test");
		return Skip(1);
	}

	bool Parse()
	{
		uint32_t v, code;
		while (frames.size() + yuvFrames.size() < m_maxFrames)
		{
			if (!NextStartCode(code))
				return false;
			if (code == 0xB7)
				break;
			if (!Skip(32))
				return false;

			if (code == 0xB3) // sequence header
			{
				uint32_t w, h;
				if (!Get(12, w) || !Get(12, h) || !Get(8, v) || !Get(18, v) || !Get(1, v) || !Get(10, v) || !Get(1, v))
					return false;
				m_w = static_cast<int>(w);
				m_h = static_cast<int>(h);
				m_mbw = (m_w + 15) / 16;
				m_mbh = (m_h + 15) / 16;
				if (!LoadMatrixIfFlagged(true) || !LoadMatrixIfFlagged(false))
					return false;
				m_mpeg1 = true; // until a sequence extension says otherwise
			}
			else if (code == 0xB5) // extension
			{
				uint32_t id;
				if (!Get(4, id))
					return false;
				if (id == 1)
					m_mpeg1 = false;
				else if (id == 3) // quant matrix extension
				{
					if (!LoadMatrixIfFlagged(true) || !LoadMatrixIfFlagged(false))
						return false;
				}
				else if (id == 8) // picture coding extension
				{
					uint32_t idp, ps, tff, fpfd, cmv, qst, ivf, as;
					if (!Get(16, v) || !Get(2, idp) || !Get(2, ps) || !Get(1, tff) || !Get(1, fpfd) || !Get(1, cmv) ||
						!Get(1, qst) || !Get(1, ivf) || !Get(1, as))
						return false;
					m_idp = idp; m_ps = ps; m_fpfd = fpfd; m_qst = qst; m_ivf = ivf; m_as = as;
				}
			}
			else if (code == 0x00) // picture header
			{
				if (!Get(10, v) || !Get(3, v))
					return false;
				if (m_inI)
					return Err("picture header before the previous I-picture was complete");
				m_pictType = static_cast<int>(v);
				if (m_mpeg1)
				{
					m_idp = 0; m_ps = 3; m_fpfd = 1; m_qst = 0; m_ivf = 0; m_as = 0;
				}
			}
			else if (code >= 0x01 && code <= 0xAF && m_pictType == 1) // slice of an I-picture
			{
				if (!DecodeSlice(code))
					return false;
			}
		}
		return error.empty();
	}

	bool DecodeSlice(uint32_t code)
	{
		uint32_t qsc;
		if (!m_inI)
		{
			if (m_ps != 3)
				return Err("field pictures are not handled by this test");
			// IPU_CTRL from the picture coding extension: IDP, AS, IVF, QST, MP1, PCT=I.
			const uint32_t ctrl = (m_idp << 16) | (m_as << 20) | (m_ivf << 21) | (m_qst << 22) | ((m_mpeg1 ? 1u : 0u) << 23) | (1u << 24);
			kzipuWriteReg32(IPU_CTRL, ctrl);
			m_inI = true;
			m_qwPerMb = (m_dec == Decoder::Idec) ? 64 : 48;
			m_outQwArmed = 0;
			m_out.clear();
		}

		// slice header: quantiser_scale_code, optional intra_slice/extra_information, extra_bit_slice
		if (!Get(5, qsc))
			return false;
		if (Show(1))
		{
			if (!Skip(9))
				return false;
			while (Show(1))
				if (!Skip(9))
					return false;
		}
		if (!Skip(1))
			return false;

		// First macroblock_address_increment of the slice, decoded by the IPU (VDEC, table 0 = MBAI).
		uint32_t mbai;
		if (!Vdec(0, mbai))
			return false;
		mbai &= 0xFFFF;
		const uint32_t mbAddr = (code - 1) * m_mbw + mbai - 1;
		const uint32_t mbBefore = MbReceived();
		if (mbai == 0 || mbAddr != mbBefore)
			return Err("slice starts at macroblock " + std::to_string(mbAddr) + ", expected " + std::to_string(mbBefore));

		if (m_dec == Decoder::Idec)
		{
			// IDEC: RGB32 output (OFM=0), no dither, no sign offset; DTD when dct_type is present in the macroblocks.
			decodes++;
			if (!Cmd(0x10000000u | ((m_fpfd ? 0u : 1u) << 24) | (qsc << 16)) || !Drain())
				return false;
		}
		else
		{
			for (bool first = true;; first = false)
			{
				if (!first)
				{
					if (!Vdec(0, mbai))
						return false;
					if ((mbai & 0xFFFF) != 1)
						return Err("skipped macroblocks in an I-picture");
				}
				// macroblock_type (VDEC table 1), quantiser_scale_code, dct_type
				uint32_t modes, dt = 0;
				if (!Vdec(1, modes))
					return false;
				if (!(modes & 1))
					return Err("non-intra macroblock in an I-picture");
				if ((modes & 16) && !Get(5, qsc))
					return false;
				if (!m_fpfd && !Get(1, dt))
					return false;
				// BDEC: intra (MBI), DC predictor reset at the slice's first macroblock (DCR), RAW16 output
				decodes++;
				if (!Cmd(0x28000000u | (first ? 1u << 26 : 0u) | (dt << 25) | (qsc << 16)) || !DrainOutput())
					return false;
				const bool sliceEnd = (kzipuReadReg32(IPU_CTRL) >> 15) & 1; // SCD: a start code follows
				if (!Fdec(0))
					return false;
				if (sliceEnd)
					break;
			}
		}

		const uint32_t got = MbReceived();
		if (got == static_cast<uint32_t>(m_mbw * m_mbh))
			return FinishPicture();
		if (got > static_cast<uint32_t>(m_mbw * m_mbh))
			return Err("more macroblocks than the picture holds");
		return true;
	}

	bool Vdec(uint32_t table, uint32_t& data)
	{
		if (!Cmd(0x30000000u | (table << 26)))
			return false;
		data = static_cast<uint32_t>(kzipuReadReg64(IPU_CMD));
		return Fdec(0);
	}

	// Lets the IPU_FROM DMA / host drain deliver everything the last command produced.
	bool DrainOutput()
	{
		for (int idle = 0; idle < 16;)
		{
			if (!error.empty())
				return false;
			idle = Pump() ? 0 : idle + 1;
		}
		if (kzipuReadReg32(IPU_CTRL) & 0xF0)
			return Err("output FIFO not drained");
		return true;
	}

	// ... and refreshes the bit reader (IPU_CMD.DATA) afterwards.
	bool Drain() { return DrainOutput() && Fdec(0); }

	uint32_t MbReceived() const
	{
		if (!m_inI)
			return 0;
		if (m_mode == Mode::HostFifo)
			return static_cast<uint32_t>(m_out.size() / (16 * m_qwPerMb));
		const uint32_t qwLeft = (kzipuReadReg32(D3_CHCR) & 0x100) ? kzipuReadReg32(D3_QWC) : 0;
		return (m_outQwArmed - qwLeft) / m_qwPerMb;
	}

	Rgba MbsToRgba(const std::vector<uint8_t>& out) const
	{
		Rgba img;
		img.w = m_w;
		img.h = m_h;
		img.px.resize(static_cast<size_t>(m_w) * m_h * 4);
		for (int mb = 0; mb < m_mbw * m_mbh; mb++)
		{
			const int mx = mb % m_mbw, my = mb / m_mbw;
			for (int y = 0; y < 16; y++)
				for (int x = 0; x < 16; x++)
				{
					const int px = mx * 16 + x, py = my * 16 + y;
					if (px < m_w && py < m_h)
						std::memcpy(&img.px[(static_cast<size_t>(py) * m_w + px) * 4], &out[static_cast<size_t>(mb) * 1024 + (y * 16 + x) * 4], 4);
				}
		}
		return img;
	}

	bool FinishPicture()
	{
		const size_t bytes = static_cast<size_t>(m_mbw) * m_mbh * m_qwPerMb * 16;
		if (m_mode == Mode::Dma)
			m_out.assign(g_ram.begin() + kOutAddr, g_ram.begin() + kOutAddr + bytes);
		if (m_out.size() != bytes)
			return Err("picture output size mismatch");
		if (m_dec == Decoder::Idec)
			frames.push_back(MbsToRgba(m_out));
		else
		{
			// RAW16 macroblock: Y[16][16], Cb[8][8], Cr[8][8] as signed 16-bit (intra: 0..255).
			Yuv f;
			f.w = m_w;
			f.h = m_h;
			const int cw = m_w / 2;
			f.y.resize(static_cast<size_t>(m_w) * m_h);
			f.cb.resize(static_cast<size_t>(cw) * (m_h / 2));
			f.cr.resize(f.cb.size());
			std::vector<uint8_t> r8(static_cast<size_t>(m_mbw) * m_mbh * 384);
			bool range = true;
			for (int mb = 0; mb < m_mbw * m_mbh; mb++)
			{
				const int16_t* m16 = reinterpret_cast<const int16_t*>(&m_out[static_cast<size_t>(mb) * 768]);
				for (int i = 0; i < 384; i++)
				{
					range &= m16[i] >= 0 && m16[i] <= 255;
					r8[static_cast<size_t>(mb) * 384 + i] = static_cast<uint8_t>(std::clamp<int>(m16[i], 0, 255));
				}
				const int mx = mb % m_mbw, my = mb / m_mbw;
				for (int y = 0; y < 16; y++)
					for (int x = 0; x < 16; x++)
						if (mx * 16 + x < m_w && my * 16 + y < m_h)
							f.y[static_cast<size_t>(my * 16 + y) * m_w + mx * 16 + x] = static_cast<uint8_t>(m16[y * 16 + x]);
				for (int y = 0; y < 8; y++)
					for (int x = 0; x < 8; x++)
						if (mx * 8 + x < cw && my * 8 + y < m_h / 2)
						{
							f.cb[static_cast<size_t>(my * 8 + y) * cw + mx * 8 + x] = static_cast<uint8_t>(m16[256 + y * 8 + x]);
							f.cr[static_cast<size_t>(my * 8 + y) * cw + mx * 8 + x] = static_cast<uint8_t>(m16[320 + y * 8 + x]);
						}
			}
			if (!range)
				return Err("BDEC intra output outside 0..255");
			yuvFrames.push_back(std::move(f));
			raw8.push_back(std::move(r8));
		}
		m_inI = false;
		if (m_mode == Mode::Dma)
			std::memset(&g_ram[kOutAddr], 0xCD, bytes); // poison, so a short next picture cannot pass on stale data
		return true;
	}
};

// ---------------------------------------------------------------------------------------------------------------------
int main(int argc, char** argv)
{
	std::string iso = "C:\\Users\\drakolord\\Downloads\\Killzone (USA)\\Killzone (USA).iso";
	fs::path outDir = fs::path(__FILE__).parent_path() / "out";
	size_t videos = 2, maxFrames = 20, esMb = 10;
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		if (a == "--iso" && i + 1 < argc) iso = argv[++i];
		else if (a == "--out" && i + 1 < argc) outDir = argv[++i];
		else if (a == "--videos" && i + 1 < argc) videos = std::stoul(argv[++i]);
		else if (a == "--frames" && i + 1 < argc) maxFrames = std::stoul(argv[++i]);
		else if (a == "--es-mb" && i + 1 < argc) esMb = std::stoul(argv[++i]);
	}
	constexpr double kMinPsnr = 30.0;
	std::error_code ec;
	fs::create_directories(outDir, ec);

	FILE* f = std::fopen(iso.c_str(), "rb");
	if (!f)
	{
		std::printf("FAIL: cannot open disc image %s\n", iso.c_str());
		return 1;
	}
	std::vector<std::pair<std::string, uint64_t>> vids;
	for (const IsoFile& file : ListRoot(f))
	{
		if (file.name.rfind("FILES", 0) != 0 || vids.size() >= videos)
			continue;
		// The first videos of FILES.DAT alternate between NTSC (512x448) and PAL (512x512) versions.
		for (uint64_t off : FindVideos(f, file, videos - vids.size()))
			vids.emplace_back(file.name, off);
	}
	if (vids.empty())
	{
		std::printf("FAIL: no PSS video found in FILES*.DAT\n");
		return 1;
	}

	for (size_t vi = 0; vi < vids.size(); vi++)
	{
		const auto& [fileName, off] = vids[vi];
		const std::vector<uint8_t> es = DemuxVideo(f, off, esMb << 20);
		std::printf("video %zu: %s @ disc offset 0x%llx, %zu bytes of video ES\n", vi, fileName.c_str(),
			static_cast<unsigned long long>(off), es.size());

		const std::vector<Yuv> refYuv = FfmpegIFrames(es);
		std::vector<Rgba> ref;
		for (const Yuv& y : refYuv)
			ref.push_back(YuvToRgb(y));
		std::printf("  FFmpeg: %zu I-frames\n", ref.size());

		// ---- IDEC through DMA ch4 (chain) / ch3 (normal), RGB32 ------------------------------------------------------
		const auto t0 = std::chrono::steady_clock::now();
		Player player(Player::Mode::Dma, Player::Decoder::Idec, es, maxFrames);
		g_intc = g_dmaEnd3 = g_dmaEnd4 = g_busErr = 0;
		const bool ok = player.Run();
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		std::printf("  [IDEC, DMA] %zu I-frames, %llu IPU commands (%llu IDEC), %.0f ms; INTC_IPU x%d, ch3 end x%d (armed x%llu), ch4 end x%d\n",
			player.frames.size(), static_cast<unsigned long long>(player.commands), static_cast<unsigned long long>(player.decodes),
			ms, g_intc, g_dmaEnd3, static_cast<unsigned long long>(player.dmaRearms), g_dmaEnd4);
		if (!ok)
			Fail("video %zu: IDEC decode: %s", vi, player.error.c_str());
		if (g_busErr)
			Fail("video %zu: %d DMA bus errors", vi, g_busErr);
		if (g_dmaEnd3 != static_cast<int>(player.dmaRearms))
			Fail("video %zu: %d ch3 DMA-end interrupts for %llu ch3 transfers", vi, g_dmaEnd3, static_cast<unsigned long long>(player.dmaRearms));

		const size_t n = std::min(ref.size(), player.frames.size());
		if (n == 0)
			Fail("video %zu: no I-frame to compare", vi);
		if (player.frames.size() > ref.size())
			Fail("video %zu: IPU decoded more I-frames (%zu) than FFmpeg (%zu)", vi, player.frames.size(), ref.size());

		double minPsnr = 1e9, bestStd = -1;
		size_t best = 0;
		for (size_t i = 0; i < n; i++)
		{
			const Rgba& a = player.frames[i];
			const Rgba& b = ref[i];
			if (a.w != b.w || a.h != b.h)
			{
				Fail("video %zu I-frame %zu: size %dx%d vs FFmpeg %dx%d", vi, i, a.w, a.h, b.w, b.h);
				continue;
			}
			const Compare c = CompareRgb(a, b);
			const double sd = LumaStdDev(b);
			std::printf("    I-frame %2zu  %dx%d  RGB PSNR %6.2f dB  max|d| %3d  px>2: %6.3f%%  (luma stddev %.1f)\n",
				i, a.w, a.h, c.psnr, c.maxDiff, c.pctOver2, sd);
			minPsnr = std::min(minPsnr, c.psnr);
			if (c.psnr < kMinPsnr)
				Fail("video %zu I-frame %zu: PSNR %.2f dB < %.0f dB", vi, i, c.psnr, kMinPsnr);
			if (sd > bestStd)
			{
				bestStd = sd;
				best = i;
			}
		}
		if (n)
		{
			if (bestStd < 10.0)
				Fail("video %zu: every compared I-frame is nearly flat (max luma stddev %.1f); use more --es-mb", vi, bestStd);
			const fs::path png = outDir / ("video" + std::to_string(vi) + "_iframe" + std::to_string(best) + "_ipu_ffmpeg_diffx8.png");
			if (WriteSideBySide(png, player.frames[best], ref[best]))
				std::printf("    wrote %s (IPU | FFmpeg | |diff| x8)\n", png.string().c_str());
			std::printf("    min RGB PSNR %.2f dB over %zu I-frames\n", minPsnr, n);
		}
		if (vi != 0)
			continue;

		// ---- IDEC again with host-driven FIFOs instead of DMA: must be bit-identical -------------------------------------
		Player hp(Player::Mode::HostFifo, Player::Decoder::Idec, es, maxFrames);
		if (!hp.Run())
			Fail("video 0 host-FIFO mode: %s", hp.error.c_str());
		size_t same = 0;
		for (size_t i = 0; i < std::min(hp.frames.size(), player.frames.size()); i++)
			same += hp.frames[i].px == player.frames[i].px;
		std::printf("  [IDEC, host FIFO] %zu I-frames, %zu bit-identical to the DMA run\n", hp.frames.size(), same);
		if (hp.frames.size() != player.frames.size() || same != player.frames.size())
			Fail("host-FIFO mode output differs from DMA mode");

		// ---- IDEC with PCSX2's unmapped IDEC QSC (informational) --------------------------------------------------------
		{
			Player qp(Player::Mode::Dma, Player::Decoder::Idec, es, maxFrames);
			qp.pcsx2IdecQsc = true;
			if (!qp.Run())
				Fail("video 0 mapIdecQsc=false: %s", qp.error.c_str());
			size_t differ = 0;
			double qmin = 1e9;
			int qmax = 0;
			for (size_t i = 0; i < std::min(qp.frames.size(), ref.size()); i++)
			{
				differ += qp.frames[i].px != player.frames[i].px;
				const Compare c = CompareRgb(qp.frames[i], ref[i]);
				qmin = std::min(qmin, c.psnr);
				qmax = std::max(qmax, c.maxDiff);
			}
			std::printf("  [IDEC, mapIdecQsc=false (PCSX2 as is)] %zu of %zu I-frames differ from the default; min RGB PSNR vs FFmpeg %.2f dB, max|d| %d\n",
				differ, qp.frames.size(), qmin, qmax);
		}

		// ---- IDEC again, advancing kzipu's clock with kzipuRun(256) (PCSX2 event timing) instead of kzipuStep() --------
		Player tp(Player::Mode::Dma, Player::Decoder::Idec, es, maxFrames);
		tp.timedSlice = 256;
		if (!tp.Run())
			Fail("video 0 timed mode: %s", tp.error.c_str());
		same = 0;
		for (size_t i = 0; i < std::min(tp.frames.size(), player.frames.size()); i++)
			same += tp.frames[i].px == player.frames[i].px;
		std::printf("  [IDEC, DMA, kzipuRun(256)] %zu I-frames, %zu bit-identical to the kzipuStep run; %.1f M virtual EE cycles\n",
			tp.frames.size(), same, kzipuGetStatus().cycle / 1e6);
		if (tp.frames.size() != player.frames.size() || same != player.frames.size())
			Fail("timed-mode output differs from kzipuStep mode");

		// ---- BDEC per macroblock (RAW16 YCbCr) vs FFmpeg's YCbCr, then CSC of that output vs IDEC's RGB32 ----------------
		Player bp(Player::Mode::Dma, Player::Decoder::Bdec, es, maxFrames);
		if (!bp.Run())
			Fail("video 0 BDEC: %s", bp.error.c_str());
		std::printf("  [BDEC, DMA] %zu I-frames, %llu IPU commands (%llu BDEC)\n", bp.yuvFrames.size(),
			static_cast<unsigned long long>(bp.commands), static_cast<unsigned long long>(bp.decodes));
		if (bp.yuvFrames.size() != player.frames.size())
			Fail("BDEC decoded %zu I-frames, IDEC %zu", bp.yuvFrames.size(), player.frames.size());
		double minY = 1e9;
		size_t cscSame = 0, cscDone = 0;
		for (size_t i = 0; i < std::min(bp.yuvFrames.size(), refYuv.size()); i++)
		{
			const Yuv& a = bp.yuvFrames[i];
			const Yuv& b = refYuv[i];
			if (a.w != b.w || a.h != b.h)
			{
				Fail("BDEC I-frame %zu size mismatch", i);
				continue;
			}
			const double py = PlanePsnr(a.y, b.y), pcb = PlanePsnr(a.cb, b.cb), pcr = PlanePsnr(a.cr, b.cr);
			minY = std::min(minY, std::min(py, std::min(pcb, pcr)));
			Rgba csc;
			Player cp(Player::Mode::Dma, Player::Decoder::Idec, {}, 0);
			const int end4 = g_dmaEnd4;
			if (!cp.RunCsc(bp.raw8[i], a.w, a.h, csc))
				Fail("CSC I-frame %zu: %s", i, cp.error.c_str());
			else
			{
				cscDone++;
				if (g_dmaEnd4 != end4 + 1)
					Fail("CSC I-frame %zu: expected one ch4 (normal mode) DMA-end interrupt, got %d", i, g_dmaEnd4 - end4);
			}
			const bool eq = i < player.frames.size() && csc.px == player.frames[i].px;
			cscSame += eq;
			const Compare c = (i < player.frames.size() && csc.px.size() == player.frames[i].px.size()) ? CompareRgb(csc, player.frames[i]) : Compare{0, 255, 100};
			const Compare cf = csc.px.size() == ref[i].px.size() ? CompareRgb(csc, ref[i]) : Compare{0, 255, 100};
			std::printf("    I-frame %2zu  YCbCr PSNR Y %6.2f  Cb %6.2f  Cr %6.2f dB | CSC(BDEC) vs FFmpeg %6.2f dB max|d| %3d | vs IDEC: %s (%.2f dB)\n",
				i, py, pcb, pcr, cf.psnr, cf.maxDiff, eq ? "identical" : "differs", c.psnr);
			if (cf.psnr < kMinPsnr)
				Fail("CSC(BDEC) I-frame %zu: RGB PSNR %.2f dB below %.0f dB", i, cf.psnr, kMinPsnr);
			if (py < kMinPsnr || pcb < kMinPsnr || pcr < kMinPsnr)
				Fail("BDEC I-frame %zu: YCbCr PSNR below %.0f dB", i, kMinPsnr);
		}
		std::printf("    min YCbCr PSNR %.2f dB; CSC(BDEC) bit-identical to IDEC in %zu of %zu I-frames\n", minY, cscSame, cscDone);
		if (cscSame != cscDone || cscDone != bp.yuvFrames.size())
			Fail("CSC of the BDEC output does not reproduce the IDEC pictures (see README, IDEC quantiser scale)");
	}
	std::fclose(f);
	kzipuShutdown();

	if (g_failures)
	{
		std::printf("RESULT: FAIL (%d)\n", g_failures);
		return 1;
	}
	std::printf("RESULT: PASS\n");
	return 0;
}
