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

#include "common/RedtapeWindows.h"
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
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
	int every = 100, last = 0, dump_frame = -1, dump_count = 0;
	bool pcrtc = false;
	std::vector<VramReq> vram;
	KzgsConfig cfg;
	cfg.upscale = 1;
	cfg.vsync = false;
	for (int i = 3; i < argc; i++)
	{
		const std::string a = argv[i];
		if (a == "--every" && i + 1 < argc) every = std::atoi(argv[++i]);
		else if (a == "--upscale" && i + 1 < argc) cfg.upscale = std::atoi(argv[++i]);
		else if (a == "--last" && i + 1 < argc) last = std::atoi(argv[++i]);
		else if (a == "--pcrtc") pcrtc = true;
		else if (a == "--dump-draws" && i + 1 < argc) std::sscanf(argv[++i], "%d:%d", &dump_frame, &dump_count);
		else if (a == "--renderer" && i + 1 < argc)
		{
			const std::string r = argv[++i];
			cfg.renderer = r == "vulkan" ? KzgsRenderer::Vulkan : r == "d3d12" ? KzgsRenderer::D3D12 :
						   r == "sw" ? KzgsRenderer::Software : KzgsRenderer::D3D11;
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
	if (!kzgsOpen(nullptr, cfg, regs, &err))
	{
		std::fprintf(stderr, "kzgsOpen failed: %s\n", err.c_str());
		return 1;
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
			kzgsGifTransfer(int(hdr[1]), buf.data(), hdr[2] / 16);
			continue;
		}
		std::memcpy(regs, buf.data(), std::min<size_t>(sizeof(regs), buf.size()));
		kzgsVsync(int(hdr[1]), true);
		frame++;
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
				GSConfig.SaveDrawStart = static_cast<int>(GSState::s_n);
				GSConfig.SaveDrawCount = dump_count;
				GSConfig.SaveDrawBy = 1;
				GSConfig.SaveFrameStart = 0;
				GSConfig.SaveFrameCount = -1;
				GSConfig.SaveFrameBy = 1;
				std::printf("  dumping %d draws from s_n=%llu to %s\n", dump_count, (unsigned long long)GSState::s_n, dir.c_str());
			});
		}
		if (every > 0 && frame % every == 0)
		{
			std::vector<uint8_t> px;
			int w = 0, h = 0;
			const bool ok = kzgsReadback(px, w, h);
			char name[64];
			std::snprintf(name, sizeof(name), "out_%04llu.png", (unsigned long long)frame);
			if (ok)
				WritePNG((out / name).string(), px, w, h);
			std::printf("frame %llu: readback %s %dx%d nonblack %.1f%%\n", (unsigned long long)frame, ok ? "ok" : "FAILED", w,
				h, NonBlack(px));

			if (pcrtc || !vram.empty())
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
	kzgsClose();
	return 0;
}
