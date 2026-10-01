// kzgs - public API and the GS thread.
// SPDX-License-Identifier: GPL-3.0+

#include "kzgs.h"
#include "kzgs_internal.h"

#include "GS/GS.h"
#include "GS/GSRegs.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "Config.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/RedtapeWindows.h"
#include "common/StringUtil.h"

#include <atomic>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace
{
	// ---------------------------------------------------------------------------------------------------------------
	// Ring buffer: single consumer (GS thread), producers serialized by s_produce_lock.
	// Positions are monotonically increasing byte counters; the slot is pos % RING_SIZE.
	// ---------------------------------------------------------------------------------------------------------------
	enum class Cmd : u32
	{
		Wrap,
		Transfer,
		Vsync,
		WriteCSR,
		SoftReset,
		Reset,
		Resize,
		UpdateConfig,
		Sync,
		Readback,
		Call,
		Quit,
	};

	struct alignas(16) CmdHeader
	{
		Cmd cmd;
		u32 size; // payload bytes following the header, multiple of 16
		u32 a;
		u32 b;
	};
	static_assert(sizeof(CmdHeader) == 16);

	struct VsyncPayload
	{
		u8 regset1[0xF0];
		u32 csr;
		u32 imr;
		u64 siglblid;
	};
	static_assert(sizeof(VsyncPayload) == 0x100);

	struct SyncPoint
	{
		std::atomic<u32> done{0};
		void Signal()
		{
			done.store(1, std::memory_order_release);
			done.notify_all();
		}
		void Wait()
		{
			while (done.load(std::memory_order_acquire) == 0)
				done.wait(0, std::memory_order_acquire);
		}
	};

	struct ReadbackRequest
	{
		SyncPoint sync;
		std::vector<uint8_t>* rgba;
		int* width;
		int* height;
		int present_w = 0, present_h = 0; // >0: the image as presented in a window of this size (aspect-corrected, scaled)
		bool ok = false;
	};

	struct CallRequest
	{
		SyncPoint sync;
		std::function<void()> fn;
	};

	static constexpr size_t RING_SIZE = 32u * 1024u * 1024u;
	static constexpr u32 TRANSFER_END_OF_PACKET = 0x100;
	static constexpr u32 MAX_TRANSFER_CHUNK = 1024u * 1024u; // bytes per Transfer command

	alignas(64) static u8* s_ring = nullptr;
	alignas(64) static std::atomic<u64> s_write_pos{0};
	alignas(64) static std::atomic<u64> s_read_pos{0};
	alignas(64) static std::atomic<s32> s_queued_frames{0};
	// Wake-up hand-shake (patch 0022): atomic notify is a WakeByAddress call even when nobody sleeps, and the producer
	// (VIF1 worker) did it once per GIF packet, the consumer once per command. The sleeper announces itself first, the
	// waker checks the flag after its (seq_cst) store, so a wake-up is never lost (a sleeper that sees the new value
	// does not wait, and wait() returns at once when the value already changed). KZGS_NOTIFY_ALWAYS=1 = the old behaviour.
	alignas(64) static std::atomic<u32> s_consumer_sleeping{0};
	alignas(64) static std::atomic<u32> s_producer_waiting{0};
	static const bool s_notify_always = []() {
		const char* v = std::getenv("KZGS_NOTIFY_ALWAYS");
		return v && *v && *v != '0';
	}();
	static std::mutex s_produce_lock;
	static u64 s_local_write = 0; // producer-side copy, guarded by s_produce_lock

	static std::thread s_gs_thread;
	static std::atomic<bool> s_open{false};
	static std::atomic<bool> s_thread_failed{false};
	static uint8_t* s_caller_regs = nullptr;
	alignas(16) static u8 s_gs_regs[0x2000]; // the GS thread's copy of the privileged registers
	static int s_max_queued_frames = 2;
	static bool s_self_contained_packets = true;
	static KzgsLogFn s_log_fn = nullptr;
	static KzgsTraceFn s_trace_fn = nullptr;
	static s64 s_trace_idle_ns = 0; // GS thread only
	static inline void Trace(int id, u32 a = 0)
	{
		if (KzgsTraceFn fn = s_trace_fn)
			fn(id, a);
	}

	static constexpr u32 Align16(u32 v) { return (v + 15u) & ~15u; }

	// Reserves header+payload space, waiting for the consumer if needed. Returns a pointer to the header slot.
	// Must hold s_produce_lock.
	static u8* Reserve(u32 payload_bytes)
	{
		const u64 need = sizeof(CmdHeader) + Align16(payload_bytes);
		for (;;)
		{
			const u64 pos = s_local_write % RING_SIZE;
			const u64 to_end = RING_SIZE - pos;
			const u64 required = (need <= to_end) ? need : (to_end + need);
			u64 r = s_read_pos.load(std::memory_order_acquire);
			bool ring_waited = false;
			while ((RING_SIZE - (s_local_write - r)) < required)
			{
				if (!ring_waited)
				{
					ring_waited = true;
					Trace(7);
				}
				s_producer_waiting.store(1, std::memory_order_seq_cst);
				r = s_read_pos.load(std::memory_order_seq_cst);
				if ((RING_SIZE - (s_local_write - r)) >= required)
					break;
				s_read_pos.wait(r, std::memory_order_acquire);
				r = s_read_pos.load(std::memory_order_acquire);
			}
			s_producer_waiting.store(0, std::memory_order_relaxed);
			if (ring_waited)
				Trace(8);

			if (need > to_end)
			{
				// Not enough contiguous space: emit a wrap marker and restart at 0.
				CmdHeader* wrap = reinterpret_cast<CmdHeader*>(s_ring + pos);
				wrap->cmd = Cmd::Wrap;
				wrap->size = static_cast<u32>(to_end - sizeof(CmdHeader));
				wrap->a = wrap->b = 0;
				s_local_write += to_end;
				continue;
			}
			return s_ring + pos;
		}
	}

	static void Publish(u8* slot, Cmd cmd, u32 payload_bytes, u32 a, u32 b)
	{
		CmdHeader* hdr = reinterpret_cast<CmdHeader*>(slot);
		hdr->cmd = cmd;
		hdr->size = Align16(payload_bytes);
		hdr->a = a;
		hdr->b = b;
		s_local_write += sizeof(CmdHeader) + hdr->size;
		s_write_pos.store(s_local_write, std::memory_order_seq_cst);
		if (s_notify_always || s_consumer_sleeping.load(std::memory_order_seq_cst))
			s_write_pos.notify_one();
	}

	static void Push(Cmd cmd, u32 a = 0, u32 b = 0, const void* payload = nullptr, u32 payload_bytes = 0)
	{
		std::lock_guard lock(s_produce_lock);
		u8* slot = Reserve(payload_bytes);
		if (payload_bytes)
			std::memcpy(slot + sizeof(CmdHeader), payload, payload_bytes);
		Publish(slot, cmd, payload_bytes, a, b);
	}

	template <typename T>
	static void PushPtr(Cmd cmd, T* ptr)
	{
		const u64 p = reinterpret_cast<u64>(ptr);
		Push(cmd, 0, 0, &p, sizeof(p));
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Config translation
	// ---------------------------------------------------------------------------------------------------------------
	static std::string ExeDir()
	{
		wchar_t buf[MAX_PATH * 2];
		const DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
		std::string path = StringUtil::WideStringToUTF8String(std::wstring_view(buf, len));
		return std::string(Path::GetDirectory(path));
	}

	static GSRendererType ToRendererType(KzgsRenderer r)
	{
		switch (r)
		{
			case KzgsRenderer::D3D12: return GSRendererType::DX12;
			case KzgsRenderer::Vulkan: return GSRendererType::VK;
			case KzgsRenderer::Software: return GSRendererType::SW;
			case KzgsRenderer::D3D11:
			default: return GSRendererType::DX11;
		}
	}

	static AspectRatioType ToAspect(KzgsAspect a)
	{
		switch (a)
		{
			case KzgsAspect::Ratio16_9: return AspectRatioType::R16_9;
			case KzgsAspect::Stretch: return AspectRatioType::Stretch;
			case KzgsAspect::Ratio4_3:
			default: return AspectRatioType::R4_3;
		}
	}

	static Pcsx2Config::GSOptions MakeOptions(const KzgsConfig& cfg)
	{
		Pcsx2Config::GSOptions o;
		o.Renderer = ToRendererType(cfg.renderer);
		o.UpscaleMultiplier = static_cast<float>(std::clamp(cfg.upscale, 1, 8));
		o.TextureFiltering = static_cast<BiFiltering>(std::clamp(static_cast<int>(cfg.textureFiltering), 0, 3));
		o.MaxAnisotropy = static_cast<u8>(std::clamp(cfg.anisotropy, 0, 16));
		o.FXAA = cfg.fxaa || cfg.smaa; // SMAA rides on the FXAA slot of the post chain (kz_smaa11.cpp)
		o.HWAA1 = cfg.edgeAA;
		o.AspectRatio = ToAspect(cfg.aspect);
		o.VsyncEnable = cfg.vsync;
		o.LinearPresent = !cfg.bilinearPresent ? GSPostBilinearMode::Off
		                : cfg.sharpPresent ? GSPostBilinearMode::BilinearSharp : GSPostBilinearMode::BilinearSmooth;
		o.CASMode = cfg.casSharpness > 0 ? GSCASMode::SharpenOnly : GSCASMode::Disabled;
		o.CAS_Sharpness = static_cast<u8>(std::clamp(cfg.casSharpness, 0, 100));
		o.UserHacks_HalfPixelOffset = static_cast<GSHalfPixelOffset>(
			std::clamp(cfg.halfPixelOffset, 0, static_cast<int>(GSHalfPixelOffset::MaxCount) - 1));
		o.UserHacks_NativeScaling = static_cast<GSNativeScaling>(
			std::clamp(cfg.nativeScaling, 0, static_cast<int>(GSNativeScaling::MaxCount) - 1));
		o.DisableVertexShaderExpand = !cfg.vertexShaderExpand;
		o.PCRTCAntiBlur = cfg.pcrtcAntiBlur;
		o.InterlaceMode = static_cast<GSInterlaceMode>(std::clamp(cfg.interlaceMode, 0, static_cast<int>(GSInterlaceMode::Count) - 1));
		for (int i = 0; i < 4; i++)
			o.Crop[i] = std::max(cfg.crop[i], 0);
		o.Adapter = cfg.useWarp ? std::string("Microsoft Basic Render Driver") : cfg.adapter;
		o.DisableShaderCache = cfg.disableShaderCache;
		o.UseDebugDevice = cfg.debugDevice;

		// Debug aid: KZGS_GS_OPTS="Name=0|1,..." flips PCSX2 GSOptions flags without a rebuild (used to bisect renderer bugs).
		if (const char* dbg = std::getenv("KZGS_GS_OPTS"))
		{
			const std::string list(dbg);
			size_t pos = 0;
			while (pos < list.size())
			{
				size_t comma = list.find(',', pos);
				if (comma == std::string::npos)
					comma = list.size();
				const std::string item = list.substr(pos, comma - pos);
				pos = comma + 1;
				const size_t eq = item.find('=');
				if (eq == std::string::npos)
					continue;
				const std::string name = item.substr(0, eq);
				const bool v = std::atoi(item.c_str() + eq + 1) != 0;
#define KZGS_OPT(n) if (name == #n) o.n = v;
				KZGS_OPT(DisableVertexShaderExpand) KZGS_OPT(DisableFramebufferFetch) KZGS_OPT(UserHacks_DisablePartialInvalidation)
				KZGS_OPT(UserHacks_DisableSafeFeatures) KZGS_OPT(UserHacks_DisableRenderFixes) KZGS_OPT(UserHacks_DisableDepthSupport)
				KZGS_OPT(UserHacks_NativePaletteDraw) KZGS_OPT(UserHacks_EstimateTextureRegion) KZGS_OPT(UserHacks_AlignSpriteX)
				KZGS_OPT(UserHacks_MergePPSprite) KZGS_OPT(UserHacks_DrawBuffering) KZGS_OPT(UserHacks_CPUFBConversion) KZGS_OPT(PCRTCAntiBlur)
				KZGS_OPT(DisableInterlaceOffset) KZGS_OPT(SkipDuplicateFrames) KZGS_OPT(UserHacks_ReadTCOnClose)
#undef KZGS_OPT
			}
		}

		// No OSD: kzgs does not render PCSX2's overlays.
		o.OsdMessagesPos = OsdOverlayPos::None;
		o.OsdPerformancePos = OsdOverlayPos::None;
		return o;
	}

	static void ApplyGlobals(const KzgsConfig& cfg, const Pcsx2Config::GSOptions& opts)
	{
		EmuConfig.GS = opts;
		EmuConfig.CurrentAspectRatio = opts.AspectRatio;
		s_max_queued_frames = std::max(1, cfg.maxQueuedFrames);
		s_self_contained_packets = cfg.selfContainedGifPackets;
		kzgs::SmaaSetEnabled(cfg.smaa);
	}

	static void SetupFolders(const KzgsConfig& cfg)
	{
		const std::string exe_dir = ExeDir();
		EmuFolders::AppRoot = exe_dir;
		EmuFolders::DataRoot = exe_dir;
		EmuFolders::Resources = cfg.resourcesDir.empty() ? Path::Combine(exe_dir, "resources") : cfg.resourcesDir;
		EmuFolders::UserResources = EmuFolders::Resources;
		EmuFolders::Cache = cfg.cacheDir.empty() ? Path::Combine(exe_dir, "cache") : cfg.cacheDir;
		EmuFolders::Logs = EmuFolders::Cache;
		EmuFolders::Snapshots = Path::Combine(exe_dir, "snaps");
		EmuFolders::Videos = Path::Combine(exe_dir, "videos");
		EmuFolders::Textures = Path::Combine(exe_dir, "textures");
		if (!cfg.disableShaderCache)
			FileSystem::CreateDirectoryPath(EmuFolders::Cache.c_str(), false);
	}

	static void LogCallback(LOGLEVEL level, ConsoleColors color, std::string_view message)
	{
		const int lvl = (level <= LOGLEVEL_ERROR) ? 0 : (level == LOGLEVEL_WARNING) ? 1 : 2;
		std::string msg(message);
		if (s_log_fn)
		{
			s_log_fn(lvl, msg.c_str());
		}
		else
		{
			msg += '\n';
			OutputDebugStringA(msg.c_str());
		}
	}

	// ---------------------------------------------------------------------------------------------------------------
	// GS thread
	// ---------------------------------------------------------------------------------------------------------------
	static void ApplyVsyncRegs(const VsyncPayload& p)
	{
		std::memcpy(s_gs_regs, p.regset1, sizeof(p.regset1));
		std::memcpy(s_gs_regs + 0x1000, &p.csr, sizeof(p.csr));
		std::memcpy(s_gs_regs + 0x1010, &p.imr, sizeof(p.imr));
		std::memcpy(s_gs_regs + 0x1080, &p.siglblid, sizeof(p.siglblid));
	}



	static void DoTransfer(u32 path_and_flags, const u8* data, u32 qwords)
	{
		const u32 path = path_and_flags & 0xFF;
		const u32 index = (path == 1) ? 0 : (path == 2) ? 1 : 2;
		switch (index)
		{
			case 0: g_gs_renderer->Transfer<0>(data, qwords); break;
			case 1: g_gs_renderer->Transfer<1>(data, qwords); break;
			default: g_gs_renderer->Transfer<2>(data, qwords); break;
		}

		// Self-contained packet mode: every kzgsGifTransfer() call starts with a GIFtag. A tag left open at the end of
		// a packet (e.g. the IMAGE tag that ends a VIF DIRECT whose data comes in a later chunk, which PS2Recomp's
		// VIF1 re-wraps in a synthesized IMAGE tag) is dropped instead of swallowing the next packet's tag as data.
		// The TRXDIR image transfer itself (m_tr) keeps its progress, so the re-wrapped data continues it.
		if (s_self_contained_packets && (path_and_flags & TRANSFER_END_OF_PACKET))
		{
			GIFPath& gp = g_gs_renderer->m_path[index];
			if (gp.nloop != 0)
				gp.nloop = 0;
		}
	}

	// Copies the current output texture 1:1 through a CPU download texture. Unlike GSSaveSnapshotToMemory it neither
	// creates nor recycles a render target, so taking a capture does not change the texture pool (and with it what later
	// frames render when a pass reuses a pooled texture that was not fully overwritten).
	static bool ReadbackCurrent(std::vector<u32>& pixels, u32& w, u32& h)
	{
		GSTexture* const current = g_gs_device ? g_gs_device->GetCurrent() : nullptr;
		if (!current || current->GetFormat() != GSTexture::Format::Color)
			return false;
		w = static_cast<u32>(current->GetWidth());
		h = static_cast<u32>(current->GetHeight());
		std::unique_ptr<GSDownloadTexture> dl(g_gs_device->CreateDownloadTexture(w, h, GSTexture::Format::Color));
		if (!dl)
			return false;
		const GSVector4i rc(0, 0, static_cast<int>(w), static_cast<int>(h));
		dl->CopyFromTexture(rc, current, rc, 0);
		dl->Flush();
		if (!dl->Map(rc))
			return false;
		pixels.resize(static_cast<size_t>(w) * h);
		StringUtil::StrideMemCpy(pixels.data(), w * sizeof(u32), dl->GetMapPointer(), dl->GetMapPitch(), w * sizeof(u32), h);
		dl->Unmap();
		return true;
	}

	static void DoReadback(ReadbackRequest* req)
	{
		u32 w = 0, h = 0;
		std::vector<u32> pixels;
		if (req->present_w > 0 && req->present_h > 0)
			req->ok = GSSaveSnapshotToMemory(static_cast<u32>(req->present_w), static_cast<u32>(req->present_h), true, false, &w, &h, &pixels) && w > 0 && h > 0;
		else
			req->ok = ReadbackCurrent(pixels, w, h) && w > 0 && h > 0;
		if (req->ok)
		{
			req->rgba->resize(static_cast<size_t>(w) * h * 4);
			std::memcpy(req->rgba->data(), pixels.data(), req->rgba->size());
			*req->width = static_cast<int>(w);
			*req->height = static_cast<int>(h);
		}
		else
		{
			req->rgba->clear();
			*req->width = *req->height = 0;
		}
	}

	static void GSThreadLoop()
	{
		u64 r = s_read_pos.load(std::memory_order_relaxed);
		for (;;)
		{
			u64 w = s_write_pos.load(std::memory_order_acquire);
			const bool was_idle = (w == r);
			const auto idle_t0 = was_idle && s_trace_fn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
			while (w == r)
			{
				s_consumer_sleeping.store(1, std::memory_order_seq_cst);
				w = s_write_pos.load(std::memory_order_seq_cst);
				if (w != r)
					break;
				s_write_pos.wait(w, std::memory_order_acquire);
				w = s_write_pos.load(std::memory_order_acquire);
			}
			s_consumer_sleeping.store(0, std::memory_order_relaxed);
			if (was_idle && s_trace_fn)
				s_trace_idle_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - idle_t0).count();

			const CmdHeader hdr = *reinterpret_cast<const CmdHeader*>(s_ring + (r % RING_SIZE));
			const u8* payload = s_ring + (r % RING_SIZE) + sizeof(CmdHeader);
			bool quit = false;

			switch (hdr.cmd)
			{
				case Cmd::Wrap:
					break;

				case Cmd::Transfer:
					DoTransfer(hdr.a, payload, hdr.b);
					break;

				case Cmd::Vsync:
				{
					ApplyVsyncRegs(*reinterpret_cast<const VsyncPayload*>(payload));
					Trace(2, static_cast<u32>(s_trace_idle_ns / 1000)); // idle time (us) since the previous frame
					s_trace_idle_ns = 0;
					Trace(3);
					GSvsync(hdr.a, hdr.b != 0);
					Trace(4);
					s_queued_frames.fetch_sub(1, std::memory_order_acq_rel);
					s_queued_frames.notify_all();
				}
				break;

				case Cmd::WriteCSR:
					GSwriteCSR(hdr.a);
					break;

				case Cmd::SoftReset:
					GSgifSoftReset(hdr.a);
					break;

				case Cmd::Reset:
					GSreset(true);
					break;

				case Cmd::Resize:
					if (GSHasDisplayWindow())
						GSResizeDisplayWindow(hdr.a, hdr.b, g_gs_device->GetWindowScale());
					break;

				case Cmd::UpdateConfig:
				{
					u64 p;
					std::memcpy(&p, payload, sizeof(p));
					std::unique_ptr<KzgsConfig> cfg(reinterpret_cast<KzgsConfig*>(p));
					const Pcsx2Config::GSOptions opts = MakeOptions(*cfg);
					const bool vsync_changed = (opts.VsyncEnable != GSConfig.VsyncEnable);
					ApplyGlobals(*cfg, opts);
					GSUpdateConfig(opts);
					kzgs::SmaaInstall();
					if (vsync_changed)
						GSSetVSyncMode(opts.VsyncEnable ? GSVSyncMode::FIFO : GSVSyncMode::Disabled, false);
				}
				break;

				case Cmd::Sync:
				{
					u64 p;
					std::memcpy(&p, payload, sizeof(p));
					reinterpret_cast<SyncPoint*>(p)->Signal();
				}
				break;

				case Cmd::Readback:
				{
					u64 p;
					std::memcpy(&p, payload, sizeof(p));
					ReadbackRequest* req = reinterpret_cast<ReadbackRequest*>(p);
					DoReadback(req);
					req->sync.Signal();
				}
				break;

				case Cmd::Call:
				{
					u64 p;
					std::memcpy(&p, payload, sizeof(p));
					CallRequest* req = reinterpret_cast<CallRequest*>(p);
					req->fn();
					req->sync.Signal();
				}
				break;

				case Cmd::Quit:
					quit = true;
					break;
			}

			r += sizeof(CmdHeader) + hdr.size;
			s_read_pos.store(r, std::memory_order_seq_cst);
			if (s_notify_always || s_producer_waiting.load(std::memory_order_seq_cst))
				s_read_pos.notify_all();
			if (quit)
				break;
		}
	}

	struct OpenResult
	{
		SyncPoint sync;
		bool ok = false;
		std::string error;
	};

	static void GSThreadMain(KzgsConfig cfg, OpenResult* result)
	{
		SetThreadDescription(GetCurrentThread(), L"kzgs GS thread");

		SetupFolders(cfg);
		const Pcsx2Config::GSOptions opts = MakeOptions(cfg);
		ApplyGlobals(cfg, opts);

		const std::string shader_probe = Path::Combine(EmuFolders::Resources, "shaders/dx11/tfx.fx");
		if (!FileSystem::FileExists(shader_probe.c_str()))
		{
			result->error = "GS shaders not found (expected " + shader_probe +
							"). Call kzgs_stage_resources() on the exe target or set KzgsConfig::resourcesDir.";
			result->sync.Signal();
			return;
		}

		kzgs::ClearLastError();
		const bool ok = GSopen(opts, opts.Renderer, s_gs_regs, opts.VsyncEnable ? GSVSyncMode::FIFO : GSVSyncMode::Disabled, false);
		if (!ok)
		{
			result->error = kzgs::GetLastError();
			if (result->error.empty())
				result->error = "GSopen failed";
			result->sync.Signal();
			return;
		}

		kzgs::SmaaInstall();
		result->ok = true;
		result->sync.Signal();
		result = nullptr; // owned by the opener, which has returned

		GSThreadLoop();

		GSclose();
	}
} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------------------------------

bool kzgsOpen(void* hwnd, const KzgsConfig& cfg, uint8_t* privRegs, std::string* err)
{
	if (s_open.load())
	{
		if (err)
			*err = "kzgs is already open";
		return false;
	}
	if (!privRegs)
	{
		if (err)
			*err = "privRegs must point to a 0x2000-byte register block";
		return false;
	}

	Log::SetHostOutputLevel(LOGLEVEL_INFO, LogCallback);

	if (!s_ring)
		s_ring = static_cast<u8*>(_aligned_malloc(RING_SIZE, 64));
	s_write_pos.store(0);
	s_read_pos.store(0);
	s_local_write = 0;
	s_queued_frames.store(0);

	s_caller_regs = privRegs;
	std::memcpy(s_gs_regs, privRegs, sizeof(s_gs_regs));
	kzgs::SetRenderWindow(hwnd);

	OpenResult result;
	s_gs_thread = std::thread(GSThreadMain, cfg, &result);
	result.sync.Wait();
	if (!result.ok)
	{
		s_gs_thread.join();
		if (err)
			*err = result.error;
		return false;
	}

	s_open.store(true);
	return true;
}

void kzgsClose()
{
	if (!s_open.load())
		return;
	Push(Cmd::Quit);
	s_gs_thread.join();
	s_open.store(false);
	s_caller_regs = nullptr;
}

bool kzgsIsOpen()
{
	return s_open.load();
}

void kzgsGifTransfer(int path, const uint8_t* data, uint32_t qwords)
{
	if (!s_open.load(std::memory_order_relaxed) || !data || qwords == 0)
		return;
	const u32 p = static_cast<u32>(std::clamp(path, 1, 3));
	const u32 chunk_qw = MAX_TRANSFER_CHUNK / 16;
	while (qwords > 0)
	{
		const u32 n = std::min(qwords, chunk_qw);
		Push(Cmd::Transfer, p | ((n == qwords) ? TRANSFER_END_OF_PACKET : 0u), n, data, n * 16);
		data += static_cast<size_t>(n) * 16;
		qwords -= n;
	}
}

void kzgsVsync(int field, bool regsWritten)
{
	if (!s_open.load(std::memory_order_relaxed))
		return;

	VsyncPayload p;
	std::memcpy(p.regset1, s_caller_regs, sizeof(p.regset1));
	std::memcpy(&p.csr, s_caller_regs + 0x1000, sizeof(p.csr));
	std::memcpy(&p.imr, s_caller_regs + 0x1010, sizeof(p.imr));
	std::memcpy(&p.siglblid, s_caller_regs + 0x1080, sizeof(p.siglblid));

	// Frame pacing: don't let the game run more than N frames ahead of the GPU.
	s32 queued = s_queued_frames.fetch_add(1, std::memory_order_acq_rel) + 1;
	Push(Cmd::Vsync, static_cast<u32>(field & 1), regsWritten ? 1u : 0u, &p, sizeof(p));
	Trace(9, static_cast<u32>(queued));
	const bool throttled = queued > s_max_queued_frames;
	if (throttled)
		Trace(5);
	while (queued > s_max_queued_frames)
	{
		s_queued_frames.wait(queued, std::memory_order_acquire);
		queued = s_queued_frames.load(std::memory_order_acquire);
	}
	if (throttled)
		Trace(6);
}

void kzgsWriteCSR(uint32_t csr)
{
	if (s_open.load(std::memory_order_relaxed))
		Push(Cmd::WriteCSR, csr);
}

void kzgsSoftReset(uint32_t mask)
{
	if (s_open.load(std::memory_order_relaxed))
		Push(Cmd::SoftReset, mask);
}

void kzgsReset()
{
	if (s_open.load(std::memory_order_relaxed))
		Push(Cmd::Reset);
}

void kzgsResize(int width, int height)
{
	if (s_open.load(std::memory_order_relaxed) && width > 0 && height > 0)
		Push(Cmd::Resize, static_cast<u32>(width), static_cast<u32>(height));
}

void kzgsUpdateConfig(const KzgsConfig& cfg)
{
	if (s_open.load(std::memory_order_relaxed))
		PushPtr(Cmd::UpdateConfig, new KzgsConfig(cfg));
}

void kzgsSync()
{
	if (!s_open.load(std::memory_order_relaxed))
		return;
	SyncPoint sp;
	PushPtr(Cmd::Sync, &sp);
	sp.Wait();
}

bool kzgsReadback(std::vector<uint8_t>& rgba, int& width, int& height, int presentWidth, int presentHeight)
{
	if (!s_open.load(std::memory_order_relaxed))
		return false;
	ReadbackRequest req;
	req.rgba = &rgba;
	req.width = &width;
	req.height = &height;
	req.present_w = presentWidth;
	req.present_h = presentHeight;
	PushPtr(Cmd::Readback, &req);
	req.sync.Wait();
	return req.ok;
}

void kzgs::RunOnGSThread(std::function<void()> fn)
{
	if (!s_open.load(std::memory_order_relaxed))
		return;
	CallRequest req;
	req.fn = std::move(fn);
	PushPtr(Cmd::Call, &req);
	req.sync.Wait();
}

void kzgsSetLogCallback(KzgsLogFn fn)
{
	s_log_fn = fn;
}

void kzgsSetTraceHook(KzgsTraceFn fn)
{
	s_trace_fn = fn;
}
