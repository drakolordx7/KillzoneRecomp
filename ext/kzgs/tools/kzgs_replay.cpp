// kzgs_replay: debug replay of a game GS trace (KZ_GS_TRACE format) through kzgs, with access to PCSX2 GS internals.
//
// Trace records: [u32 type][u32 arg][u32 size][size bytes]
//   type 1 = GIF packet (arg = PATH 1..3), type 2 = vsync (arg = field, payload = 0x2000-byte GSPrivRegSet block)
//
// Usage: kzgs_replay <trace> <outdir> [--every N] [--upscale N] [--renderer d3d11|d3d12|vulkan|sw]
//                    [--vram FBP:FBW[,FBP:FBW...]] [--pcrtc] [--last N]
//   --vram   at every sampled frame, also decode these CT32 buffers from GS local memory (after a texture-cache
//            readback on HW renderers) to <outdir>/vram_<fbp>_<frame>.png
//   --pcrtc  print the PCRTC (display circuit) state PCSX2 computed at every sampled frame
//   --last N stop after N vsyncs
// SPDX-License-Identifier: GPL-3.0+

#include "kzgs.h"
#include "kzgs_internal.h"

#include "GS/GS.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/Renderers/HW/GSTextureCache.h"

#include "common/RedtapeWindows.h"
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

static uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0)
{
	crc = ~crc;
	for (size_t i = 0; i < n; i++)
	{
		crc ^= p[i];
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

static void PutBE32(std::vector<uint8_t>& v, uint32_t x)
{
	v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
}

static void Chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data)
{
	PutBE32(out, uint32_t(data.size()));
	std::vector<uint8_t> td(type, type + 4);
	td.insert(td.end(), data.begin(), data.end());
	out.insert(out.end(), td.begin(), td.end());
	PutBE32(out, Crc32(td.data(), td.size()));
}

static bool WritePNG(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h)
{
	std::vector<uint8_t> raw;
	raw.reserve(size_t(h) * (size_t(w) * 3 + 1));
	for (int y = 0; y < h; y++)
	{
		raw.push_back(0);
		const uint8_t* row = rgba.data() + size_t(y) * w * 4;
		for (int x = 0; x < w; x++)
		{
			raw.push_back(row[x * 4 + 0]); raw.push_back(row[x * 4 + 1]); raw.push_back(row[x * 4 + 2]);
		}
	}
	std::vector<uint8_t> z = {0x78, 0x01};
	uint32_t a = 1, b = 0;
	for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
	for (size_t off = 0; off < raw.size(); off += 65535)
	{
		const size_t n = std::min<size_t>(65535, raw.size() - off);
		z.push_back(off + n == raw.size() ? 1 : 0);
		z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
		z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
		z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
	}
	PutBE32(z, (b << 16) | a);
	std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	std::vector<uint8_t> ihdr;
	PutBE32(ihdr, uint32_t(w)); PutBE32(ihdr, uint32_t(h));
	ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
	Chunk(png, "IHDR", ihdr);
	Chunk(png, "IDAT", z);
	Chunk(png, "IEND", {});
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f)
		return false;
	std::fwrite(png.data(), 1, png.size(), f);
	std::fclose(f);
	return true;
}

static double NonBlack(const std::vector<uint8_t>& px)
{
	size_t nb = 0;
	for (size_t i = 0; i + 3 < px.size(); i += 4)
		nb += (px[i] + px[i + 1] + px[i + 2]) > 30;
	return px.size() ? 100.0 * nb / (px.size() / 4) : 0.0;
}

struct VramReq { uint32_t fbp, fbw; };

struct TexReq { uint32_t tbp, tbw, psm, w, h, cbp; };

// Decodes a PSMT8/PSMT4 (CSM1, CT32 CLUT) or PSMCT32 texture from GS local memory into color and alpha PNGs.
static void DumpTexture(const GSLocalMemory& mem, const TexReq& t, const std::filesystem::path& dir, uint64_t frame)
{
	std::vector<uint8_t> col(size_t(t.w) * t.h * 4), alp(size_t(t.w) * t.h * 4);
	auto clut = [&](uint32_t i) -> uint32_t {
		if (t.psm == 0x13)
			i = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1); // CSM1 T8 CLUT column swizzle
		return mem.ReadPixel32(int(i & 15), int(i >> 4), t.cbp, 1);
	};
	for (uint32_t y = 0; y < t.h; y++)
		for (uint32_t x = 0; x < t.w; x++)
		{
			uint32_t c;
			if (t.psm == 0x13)
				c = clut(mem.ReadPixel8(int(x), int(y), t.tbp, t.tbw));
			else if (t.psm == 0x14)
				c = clut(mem.ReadPixel4(int(x), int(y), t.tbp, t.tbw));
			else
				c = mem.ReadPixel32(int(x), int(y), t.tbp, t.tbw);
			const size_t o = (size_t(y) * t.w + x) * 4;
			std::memcpy(&col[o], &c, 4);
			const uint8_t a = uint8_t(std::min<uint32_t>(255, (c >> 24) * 2));
			alp[o] = alp[o + 1] = alp[o + 2] = a;
			alp[o + 3] = 255;
		}
	char n[96];
	std::snprintf(n, sizeof(n), "tex_%04x_%02x_%04llu_rgb.png", t.tbp, t.psm, (unsigned long long)frame);
	WritePNG((dir / n).string(), col, int(t.w), int(t.h));
	std::snprintf(n, sizeof(n), "tex_%04x_%02x_%04llu_alpha.png", t.tbp, t.psm, (unsigned long long)frame);
	WritePNG((dir / n).string(), alp, int(t.w), int(t.h));
}


static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
	HANDLE proc = GetCurrentProcess();
	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
	SymInitialize(proc, nullptr, TRUE);
	auto describe = [&](DWORD64 addr) {
		static char text[800];
		alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
		auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 255;
		DWORD64 disp = 0;
		IMAGEHLP_LINE64 line = {sizeof(line)};
		DWORD ldisp = 0;
		const bool has_sym = SymFromAddr(proc, addr, &disp, sym);
		const bool has_line = SymGetLineFromAddr64(proc, addr, &ldisp, &line);
		std::snprintf(text, sizeof(text), "%p %s+0x%llx %s:%lu", (void*)addr, has_sym ? sym->Name : "?",
			(unsigned long long)disp, has_line ? line.FileName : "", has_line ? line.LineNumber : 0);
		return text;
	};
	std::fprintf(stderr, "CRASH code=%08lx addr=%s\n", ep->ExceptionRecord->ExceptionCode,
		describe(reinterpret_cast<DWORD64>(ep->ExceptionRecord->ExceptionAddress)));
	if (ep->ExceptionRecord->NumberParameters >= 2)
		std::fprintf(stderr, "  access %s %p\n", ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
			(void*)ep->ExceptionRecord->ExceptionInformation[1]);
	CONTEXT ctx = *ep->ContextRecord;
	STACKFRAME64 frame{};
	frame.AddrPC.Offset = ctx.Rip; frame.AddrPC.Mode = AddrModeFlat;
	frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
	frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
	for (int i = 0; i < 20 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &frame, &ctx, nullptr,
								  SymFunctionTableAccess64, SymGetModuleBase64, nullptr) && frame.AddrPC.Offset; ++i)
		std::fprintf(stderr, "  #%02d %s\n", i, describe(frame.AddrPC.Offset));
	std::fflush(stderr);
	return EXCEPTION_EXECUTE_HANDLER;
}


// Debug: the GS texture cache's render targets (protected member, reached through a derived class).
struct TcAccess : GSTextureCache
{
	using GSTextureCache::m_dst;
};

static bool DownloadColor(GSTexture* tex, std::vector<uint8_t>& rgba, int& w, int& h)
{
	if (!tex || tex->GetFormat() != GSTexture::Format::Color)
		return false;
	w = tex->GetWidth();
	h = tex->GetHeight();
	std::unique_ptr<GSDownloadTexture> dl(g_gs_device->CreateDownloadTexture(w, h, GSTexture::Format::Color));
	if (!dl)
		return false;
	const GSVector4i rc(0, 0, w, h);
	dl->CopyFromTexture(rc, tex, rc, 0);
	dl->Flush();
	if (!dl->Map(rc))
		return false;
	rgba.resize(size_t(w) * h * 4);
	for (int y = 0; y < h; y++)
		std::memcpy(rgba.data() + size_t(y) * w * 4, dl->GetMapPointer() + size_t(y) * dl->GetMapPitch(), size_t(w) * 4);
	dl->Unmap();
	return true;
}

static void DumpTargets(const std::filesystem::path& dir, uint64_t frame)
{
	TcAccess& tc = static_cast<TcAccess&>(*g_texture_cache);
	int idx = 0;
	for (GSTextureCache::Target* t : tc.m_dst[GSTextureCache::RenderTarget])
	{
		std::printf("  target #%d: TBP0 %05x TBW %u PSM %02x tex %dx%d scale %.1f age %d valid (%d,%d,%d,%d) drawn (%d,%d,%d,%d) used %d frame %d lastdraw %llu\n", idx,
			t->m_TEX0.TBP0, t->m_TEX0.TBW, t->m_TEX0.PSM, t->m_texture ? t->m_texture->GetWidth() : 0, t->m_texture ? t->m_texture->GetHeight() : 0,
			t->m_scale, t->m_age, t->m_valid.x, t->m_valid.y, t->m_valid.z, t->m_valid.w, t->m_drawn_since_read.x, t->m_drawn_since_read.y,
			t->m_drawn_since_read.z, t->m_drawn_since_read.w, t->m_used, t->m_is_frame, (unsigned long long)t->m_last_draw);
		std::vector<uint8_t> px;
		int w = 0, h = 0;
		if (DownloadColor(t->m_texture, px, w, h))
		{
			char n[96];
			std::snprintf(n, sizeof(n), "target_%04llu_%02d_bp%05x.png", (unsigned long long)frame, idx, t->m_TEX0.TBP0);
			WritePNG((dir / n).string(), px, w, h);
		}
		idx++;
	}
}

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	SetUnhandledExceptionFilter(CrashFilter);
	if (argc < 3)
	{
		std::fprintf(stderr, "usage: kzgs_replay <trace> <outdir> [--every N] [--upscale N] [--renderer d3d11|d3d12|vulkan|sw] [--vram FBP:FBW,...] [--pcrtc] [--last N]\n");
		return 2;
	}
	const std::string trace = argv[1];
	const std::filesystem::path out = argv[2];
	int every = 100, from = 0, last = 0, sleep_ms = 0, sleep_from = 0, dump_frame = -1, dump_count = 0; bool dump_rt = false; int dump_skip = 0;
	bool pcrtc = false, targets = false, use_window = false, backbuffer = false, skip_idle = false, single_circuit = false;
	uint64_t packets_since_vsync = 0;
	uint8_t last_regs[0xF0] = {};
	FILE* thumbs = nullptr;
	bool nopng = false;
	int present_w = 0, present_h = 0;
	std::vector<VramReq> vram;
	std::vector<TexReq> texs;
	std::string defrost, save_vram;
	KzgsConfig cfg;
	cfg.upscale = 1;
	cfg.vsync = false;
	for (int i = 3; i < argc; i++)
	{
		const std::string a = argv[i];
		if (a == "--every" && i + 1 < argc) every = std::atoi(argv[++i]);
		else if (a == "--upscale" && i + 1 < argc) cfg.upscale = std::atoi(argv[++i]);
		else if (a == "--sleep-from" && i + 1 < argc) sleep_from = std::atoi(argv[++i]);
		else if (a == "--sleep" && i + 1 < argc) sleep_ms = std::atoi(argv[++i]);
		else if (a == "--from" && i + 1 < argc) from = std::atoi(argv[++i]);
		else if (a == "--adapter" && i + 1 < argc) cfg.adapter = argv[++i];
		else if (a == "--hpo" && i + 1 < argc) cfg.halfPixelOffset = std::atoi(argv[++i]);
		else if (a == "--native" && i + 1 < argc) cfg.nativeScaling = std::atoi(argv[++i]);
		else if (a == "--antiblur" && i + 1 < argc) cfg.pcrtcAntiBlur = std::atoi(argv[++i]) != 0;
		else if (a == "--interlace" && i + 1 < argc) cfg.interlaceMode = std::atoi(argv[++i]);
		else if (a == "--fxaa") cfg.fxaa = true;
		else if (a == "--skip-idle") skip_idle = true;
		else if (a == "--single-circuit") single_circuit = true;
		else if (a == "--window") use_window = true;
		else if (a == "--backbuffer") backbuffer = true; // with --window: bb_<frame>.png = the swap chain back buffer at Present (exact final pixels, CAS included)
		else if (a == "--sharp" && i + 1 < argc) cfg.sharpPresent = std::atoi(argv[++i]) != 0;
		else if (a == "--bilinear" && i + 1 < argc) cfg.bilinearPresent = std::atoi(argv[++i]) != 0;
		else if (a == "--cas" && i + 1 < argc) cfg.casSharpness = std::atoi(argv[++i]);
		else if (a == "--filter" && i + 1 < argc) cfg.textureFiltering = static_cast<KzgsTextureFilter>(std::atoi(argv[++i]));
		else if (a == "--smaa") cfg.smaa = true;
		else if (a == "--dump-textures") cfg.dumpTextures = true;
		else if (a == "--replace-textures") cfg.loadTextureReplacements = true;
		else if (a == "--crop" && i + 1 < argc) std::sscanf(argv[++i], "%d,%d,%d,%d", &cfg.crop[0], &cfg.crop[1], &cfg.crop[2], &cfg.crop[3]);
		else if (a == "--last" && i + 1 < argc) last = std::atoi(argv[++i]);
		else if (a == "--pcrtc") pcrtc = true;
		else if (a == "--targets") targets = true;
		else if (a == "--nopng") nopng = true;
		else if (a == "--aspect" && i + 1 < argc) { const std::string v = argv[++i]; cfg.aspect = v == "16:9" ? KzgsAspect::Ratio16_9 : v == "stretch" ? KzgsAspect::Stretch : KzgsAspect::Ratio4_3; }
		else if (a == "--present" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &present_w, &present_h);
		else if (a == "--thumbs" && i + 1 < argc) thumbs = std::fopen(argv[++i], "wb");
		else if (a == "--dump-rt") dump_rt = true;
		else if (a == "--dump-skip" && i + 1 < argc) dump_skip = std::atoi(argv[++i]); // skip this many draws after the dump frame starts
		else if (a == "--dump-draws" && i + 1 < argc) std::sscanf(argv[++i], "%d:%d", &dump_frame, &dump_count);
		else if (a == "--renderer" && i + 1 < argc)
		{
			const std::string r = argv[++i];
			cfg.renderer = r == "vulkan" ? KzgsRenderer::Vulkan : r == "d3d12" ? KzgsRenderer::D3D12 :
						   r == "sw" ? KzgsRenderer::Software : KzgsRenderer::D3D11;
		}
		else if (a == "--defrost" && i + 1 < argc) defrost = argv[++i];
		else if (a == "--save-vram" && i + 1 < argc) save_vram = argv[++i];
		else if (a == "--tex" && i + 1 < argc)
		{
			TexReq t{};
			if (std::sscanf(argv[++i], "%x:%u:%x:%u:%u:%x", &t.tbp, &t.tbw, &t.psm, &t.w, &t.h, &t.cbp) >= 5)
				texs.push_back(t);
		}
		else if (a == "--vram" && i + 1 < argc)
		{
			std::string list = argv[++i];
			size_t pos = 0;
			while (pos < list.size())
			{
				size_t comma = list.find(',', pos);
				if (comma == std::string::npos) comma = list.size();
				const std::string item = list.substr(pos, comma - pos);
				unsigned fbp = 0, fbw = 0;
				if (std::sscanf(item.c_str(), "%x:%u", &fbp, &fbw) == 2)
					vram.push_back({fbp, fbw});
				pos = comma + 1;
			}
		}
	}
	std::filesystem::create_directories(out);

	FILE* f = std::fopen(trace.c_str(), "rb");
	if (!f)
	{
		std::fprintf(stderr, "cannot open %s\n", trace.c_str());
		return 1;
	}

	alignas(16) static uint8_t regs[0x2000] = {};
	kzgsSetLogCallback([](int level, const char* msg) {
		if (level <= 1)
			std::fprintf(stderr, "[gs] %s\n", msg);
	});
	std::string err;
	HWND hidden_window = nullptr; // --window: a hidden window of the --present size, so the swap chain and the final-scale path (Bilinear Sharp pre-scale) match the game
	if (use_window && present_w > 0 && present_h > 0)
		hidden_window = CreateWindowExW(0, L"STATIC", L"kzgs_replay", WS_POPUP, 0, 0, present_w, present_h, nullptr, nullptr, nullptr, nullptr);
	if (!kzgsOpen(hidden_window, cfg, regs, &err))
	{
		std::fprintf(stderr, "kzgsOpen failed: %s\n", err.c_str());
		return 1;
	}

	auto saveVram = [&](const std::string& path) {
		kzgs::RunOnGSThread([&]() {
			g_gs_renderer->ReadbackTextureCache();
			if (FILE* vf = std::fopen(path.c_str(), "wb"))
			{
				std::fwrite(g_gs_renderer->m_mem.vm8(), 1, 4u << 20, vf);
				std::fclose(vf);
			}
		});
	};

	// --defrost: load a PCSX2 savestate's GS.bin (GS freeze) instead of replaying a trace, then dump.
	if (!defrost.empty())
	{
		FILE* df = std::fopen(defrost.c_str(), "rb");
		if (!df)
			return 1;
		std::vector<u8> blob;
		u8 tmp[65536];
		size_t n;
		while ((n = std::fread(tmp, 1, sizeof(tmp), df)) > 0)
			blob.insert(blob.end(), tmp, tmp + n);
		std::fclose(df);
		int rc = -1;
		kzgs::RunOnGSThread([&]() {
			freezeData fd = {static_cast<int>(blob.size()), blob.data()};
			rc = GSfreeze(FreezeAction::Load, &fd);
			for (const TexReq& t : texs)
				DumpTexture(g_gs_renderer->m_mem, t, out, 0);
		});
		std::printf("defrost %s: rc=%d (%zu bytes)\n", defrost.c_str(), rc, blob.size());
		if (!save_vram.empty())
			saveVram(save_vram);
		kzgsClose();
		return rc == 0 ? 0 : 1;
	}

	uint32_t hdr[3];
	std::vector<uint8_t> buf;
	uint64_t frame = 0;
	while (std::fread(hdr, sizeof(hdr), 1, f) == 1)
	{
		if (hdr[0] < 1 || hdr[0] > 2 || hdr[2] > (64u << 20))
			break;
		buf.resize(hdr[2]);
		if (hdr[2] && std::fread(buf.data(), hdr[2], 1, f) != 1)
			break;
		if (hdr[0] == 1)
		{
			++packets_since_vsync;
			kzgsGifTransfer(int(hdr[1]), buf.data(), hdr[2] / 16);
			continue;
		}
		std::memcpy(regs, buf.data(), std::min<size_t>(sizeof(regs), buf.size()));
		if (single_circuit) // same register rewrite as src/kz_gs.cpp snapshotPrivRegs: read circuit 2 := read circuit 1 when it is Killzone's 1-line-offset flicker filter
		{
			uint64_t pmode, dispfb1, display1, dispfb2, display2;
			std::memcpy(&pmode, regs + 0x00, 8); std::memcpy(&dispfb1, regs + 0x70, 8); std::memcpy(&display1, regs + 0x80, 8);
			std::memcpy(&dispfb2, regs + 0x90, 8); std::memcpy(&display2, regs + 0xA0, 8);
			const uint64_t fbLow = 0xFFFFFFFFull, dbxMask = 0x7FFull << 32, dbyMask = 0x7FFull << 43, dhMask = 0x7FFull << 44;
			const uint64_t dby1 = (dispfb1 & dbyMask) >> 43, dby2 = (dispfb2 & dbyMask) >> 43, dh1 = (display1 & dhMask) >> 44, dh2 = (display2 & dhMask) >> 44;
			if ((pmode & 3u) == 3u && (dispfb1 & (fbLow | dbxMask)) == (dispfb2 & (fbLow | dbxMask)) && dby2 >= dby1 && dby2 - dby1 <= 1 &&
			    (display1 & ~dhMask) == (display2 & ~dhMask) && dh1 >= dh2 && dh1 - dh2 <= 1)
			{
				std::memcpy(regs + 0x90, &dispfb1, 8);
				std::memcpy(regs + 0xA0, &display1, 8);
			}
		}
		const bool capture_bb = backbuffer && every > 0 && (frame + 1) % uint64_t(every) == 0 && frame + 1 >= uint64_t(from);
		if (capture_bb)
			kzgsBurstArm(1, -1, -1);
		// --skip-idle: like the game (src/kz_gs.cpp runVsync), do not present a vblank that has no new GIF packets and unchanged display registers
		bool present_now = true;
		if (skip_idle)
		{
			present_now = packets_since_vsync != 0 || std::memcmp(last_regs, regs, sizeof(last_regs)) != 0 || frame == 0;
			std::memcpy(last_regs, regs, sizeof(last_regs));
		}
		packets_since_vsync = 0;
		if (present_now)
			kzgsVsync(int(hdr[1]), true);
		if (capture_bb)
		{
			kzgsSync();
			std::vector<KzgsBurstFrame> got;
			if (kzgsBurstTake(got) && !got.empty())
			{
				char bn[64];
				std::snprintf(bn, sizeof(bn), "bb_%04llu.png", (unsigned long long)(frame + 1));
				WritePNG((out / bn).string(), got[0].rgba, got[0].width, got[0].height);
			}
		}
		frame++;
		if (sleep_ms > 0 && frame >= uint64_t(sleep_from))
			Sleep(sleep_ms);
		if (dump_frame >= 0 && frame == uint64_t(dump_frame))
		{
			// PCSX2's own draw dumping (context registers, vertices, transfers per draw) for the next dump_count draws.
			const std::string dir = std::filesystem::absolute(out / "draws").string();
			std::filesystem::create_directories(dir);
			kzgs::RunOnGSThread([&]() {
				GSConfig.HWDumpDirectory = dir;
				GSConfig.SWDumpDirectory = dir;
				GSConfig.DumpGSData = true;
				GSConfig.SaveInfo = true;
				GSConfig.SaveRT = dump_rt; // render target after every dumped draw (.bmp)
				GSConfig.SaveTexture = dump_rt; // input texture of every dumped draw (.dds)
				GSConfig.SaveDrawStart = static_cast<int>(GSState::s_n) + dump_skip;
				GSConfig.SaveDrawCount = dump_count;
				GSConfig.SaveDrawBy = 1;
				GSConfig.SaveFrameStart = 0;
				GSConfig.SaveFrameCount = -1;
				GSConfig.SaveFrameBy = 1;
				std::printf("  dumping %d draws from s_n=%llu to %s\n", dump_count, (unsigned long long)GSState::s_n, dir.c_str());
			});
		}
		if (every > 0 && frame % every == 0 && frame >= uint64_t(from))
		{
			std::vector<uint8_t> px;
			int w = 0, h = 0;
			const bool ok = kzgsReadback(px, w, h);
			if (present_w > 0 && present_h > 0)
			{
				std::vector<uint8_t> pp;
				int pw = 0, ph = 0;
				if (kzgsReadback(pp, pw, ph, present_w, present_h))
				{
					char pn[64];
					std::snprintf(pn, sizeof(pn), "present_%04llu.png", (unsigned long long)frame);
					WritePNG((out / pn).string(), pp, pw, ph);
				}
			}
			if (thumbs && ok)
			{
				// frame number + 64x56 box-filtered RGB thumbnail (binary), for frame-by-frame comparison of runs
				const uint32_t fr = uint32_t(frame);
				std::fwrite(&fr, 4, 1, thumbs);
				for (int ty = 0; ty < 56; ty++)
					for (int tx = 0; tx < 64; tx++)
					{
						uint32_t acc[3] = {};
						const int bw = w / 64, bh = h / 56;
						for (int yy = 0; yy < bh; yy++)
							for (int xx = 0; xx < bw; xx++)
							{
								const uint8_t* q = &px[(size_t(ty * bh + yy) * w + tx * bw + xx) * 4];
								acc[0] += q[0]; acc[1] += q[1]; acc[2] += q[2];
							}
						for (int c = 0; c < 3; c++)
							std::fputc(int(acc[c] / (bw * bh)), thumbs);
					}
			}
			char name[64];
			std::snprintf(name, sizeof(name), "out_%04llu.png", (unsigned long long)frame);
			if (ok && !nopng)
				WritePNG((out / name).string(), px, w, h);
			std::printf("frame %llu: readback %s %dx%d nonblack %.1f%%\n", (unsigned long long)frame, ok ? "ok" : "FAILED", w,
				h, NonBlack(px));

			if (pcrtc || targets || !vram.empty() || !texs.empty())
			{
				kzgs::RunOnGSThread([&]() {
					GSRenderer* r = g_gs_renderer.get();
					if (pcrtc)
					{
						auto& pc = r->PCRTCDisplays;
						std::printf("  pcrtc: videomode %d interlaced %d FFMD %d samesrc %d res %dx%d\n", pc.videomode,
							pc.interlaced, pc.FFMD, pc.PCRTCSameSrc, pc.GetResolution().x, pc.GetResolution().y);
						for (int d = 0; d < 2; d++)
						{
							const auto& c = pc.PCRTCDisplays[d];
							std::printf("  disp%d en %d FBP %x FBW %d PSM %x DBX %d DBY %d displayRect (%d,%d,%d,%d) fbRect (%d,%d,%d,%d) "
										"dispOff (%d,%d) fbOff (%d,%d) mag (%d,%d)\n",
								d, c.enabled, c.FBP, c.FBW, c.PSM, c.DBX, c.DBY, c.displayRect.x, c.displayRect.y,
								c.displayRect.z, c.displayRect.w, c.framebufferRect.x, c.framebufferRect.y,
								c.framebufferRect.z, c.framebufferRect.w, c.displayOffset.x, c.displayOffset.y,
								c.framebufferOffsets.x, c.framebufferOffsets.y, c.magnification.x, c.magnification.y);
						}
						GSTexture* cur = g_gs_device->GetCurrent();
						std::printf("  current output texture: %s %dx%d\n", cur ? "yes" : "none", cur ? cur->GetWidth() : 0,
							cur ? cur->GetHeight() : 0);
					}
					if (targets)
						DumpTargets(out, frame);
					if (!texs.empty())
					{
						r->ReadbackTextureCache();
						for (const TexReq& t : texs)
							DumpTexture(r->m_mem, t, out, frame);
					}
					if (!vram.empty())
					{
						r->ReadbackTextureCache();
						// Nonzero bytes per 32 pages (256 KB) of the 4 MB local memory.
						std::printf("  vram nonzero KB per 256KB:");
						for (int chunk = 0; chunk < 16; chunk++)
						{
							size_t nz = 0;
							for (size_t i = size_t(chunk) << 18; i < (size_t(chunk + 1) << 18); i++)
								nz += r->m_mem.vm8()[i] != 0;
							std::printf(" %zu", nz >> 10);
						}
						std::printf("\n");
						for (const VramReq& v : vram)
						{
							const int vw = int(v.fbw) * 64, vh = 512;
							std::vector<uint8_t> img(size_t(vw) * vh * 4);
							for (int y = 0; y < vh; y++)
								for (int x = 0; x < vw; x++)
								{
									const u32 c = r->m_mem.ReadPixel32(x, y, v.fbp << 5, v.fbw);
									std::memcpy(&img[(size_t(y) * vw + x) * 4], &c, 4);
								}
							char vn[64];
							std::snprintf(vn, sizeof(vn), "vram_%03x_%04llu.png", v.fbp, (unsigned long long)frame);
							WritePNG((out / vn).string(), img, vw, vh);
							std::printf("  vram FBP %x FBW %u: nonblack %.1f%%\n", v.fbp, v.fbw, NonBlack(img));
						}
					}
				});
			}
		}
		if (last > 0 && frame >= uint64_t(last))
			break;
	}
	std::fclose(f);
	std::printf("replayed %llu frames\n", (unsigned long long)frame);
	if (!save_vram.empty())
		saveVram(save_vram);
	kzgsClose();
	return 0;
}
