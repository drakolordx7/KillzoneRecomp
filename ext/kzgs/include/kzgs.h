// kzgs - PCSX2's hardware GS renderer as a library.
// SPDX-License-Identifier: GPL-3.0+   (links PCSX2 code; see README.md)
//
// Threading model
// ---------------
// kzgs owns a dedicated GS thread. The D3D11/D3D12/Vulkan device is created, used and destroyed on that thread only.
// Every kzgs* function below is a *producer* call: it copies its arguments into a ring buffer and returns
// (asynchronous), except kzgsOpen / kzgsClose / kzgsReadback / kzgsSync, which wait for the GS thread.
// Producer calls are serialized by an internal mutex, but GIF data from different threads would interleave at packet
// granularity, so in practice drive kzgs from ONE thread (the game/EE thread that owns the GIF arbiter).
//
// Privileged registers
// --------------------
// The caller owns the 0x2000-byte privileged register block (PCSX2 GSPrivRegSet layout: PMODE at +0x0000, SMODE1
// +0x0010, SMODE2 +0x0020, ..., DISPFB1 +0x0070, DISPLAY1 +0x0080, DISPFB2 +0x0090, DISPLAY2 +0x00A0, EXTBUF +0x00B0,
// EXTDATA +0x00C0, EXTWRITE +0x00D0, BGCOLOR +0x00E0, CSR +0x1000, IMR +0x1010, BUSDIR +0x1040, SIGLBLID +0x1080).
// Like PCSX2's MTGS, the GS thread works on its own copy: kzgsVsync() snapshots 0x0000-0x00EF, CSR, IMR and SIGLBLID
// from the caller's block into the ring, so the caller may keep writing its block at any time.
// SIGNAL / FINISH / LABEL, CSR interrupt bits and IMR masking are NOT handled by kzgs (PCSX2's GS ignores those
// A+D writes too; the EE-side GIF unit handles them). The caller's GIF arbiter must process them before or while
// forwarding packets, and raise INTC_GS itself.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class KzgsRenderer : int
{
	D3D11 = 0,
	D3D12 = 1,
	Vulkan = 2,
	Software = 3, // PCSX2's software rasterizer (reference/debugging; presents through D3D11)
};

enum class KzgsAspect : int
{
	Ratio4_3 = 0,
	Ratio16_9 = 1,
	Stretch = 2,
};

// Maps to PCSX2 BiFiltering.
enum class KzgsTextureFilter : int
{
	Nearest = 0,         // never bilinear
	Forced = 1,          // always bilinear
	PS2 = 2,             // bilinear only where the game asked for it (PCSX2 default)
	ForcedExceptSprites = 3,
};

struct KzgsConfig
{
	KzgsRenderer renderer = KzgsRenderer::D3D11;
	int upscale = 1;                                   // internal resolution multiplier, 1..8
	KzgsTextureFilter textureFiltering = KzgsTextureFilter::PS2;
	int anisotropy = 0;                                // 0 (off), 2, 4, 8, 16
	bool fxaa = false;                                 // post-process FXAA
	bool smaa = false;                                 // post-process SMAA 1x (Direct3D 11 only; others ignore it)
	// PCSX2 texture replacement: files in <exe dir>/textures/SCUS-97402/replacements (named by PCSX2's texture hash) replace
	// the game's textures; dumpTextures writes every texture the game uses to .../dumps (named the same way).
	bool loadTextureReplacements = false;
	bool dumpTextures = false;
	bool edgeAA = false;                               // GS AA1 (PS2 edge antialiasing emulation, "HWAA1")
	KzgsAspect aspect = KzgsAspect::Ratio4_3;
	bool vsync = true;
	bool bilinearPresent = true;                       // smooth scaling of the final image to the window
	bool sharpPresent = false;                         // with bilinearPresent: PCSX2 "Bilinear (Sharp)" (crisp pixel edges)
	int casSharpness = 0;                              // Contrast Adaptive Sharpening, 0 = off, 1..100
	// Presentation crop in native (1x) output pixels: left, top, right, bottom. Applies to the displayed image and to
	// kzgsReadback only, not to GS rendering. Hides the edge pixels PCSX2 shows that a CRT's overscan would hide.
	int crop[4] = {0, 0, 0, 0};
	// PCSX2 builds sprites and lines in the vertex shader (DisableVertexShaderExpand=false). On NVIDIA under D3D11 this
	// intermittently corrupts frames (measured in docs/findings.md); D3D12 and Vulkan do not show it.
	bool vertexShaderExpand = true;
	bool pcrtcAntiBlur = true;                         // PCSX2 "Anti-Blur" (PCRTCAntiBlur): merges near-identical display circuits
	int interlaceMode = 0;                             // PCSX2 GSInterlaceMode: 0 Automatic, 1 Off, 2/3 Weave TFF/BFF, 4/5 Bob, 6/7 Blend, 8/9 Adaptive

	// Killzone GameIndex gsHWFixes (SCUS-97402 / SCES-52004 / SCES-52893).
	int halfPixelOffset = 4;                           // GSHalfPixelOffset: 0 Off .. 4 Native, 5 Native+TexOffset
	int nativeScaling = 2;                             // GSNativeScaling: 0 Off, 1 Normal, 2 Aggressive, ...

	std::string adapter;                               // DXGI/Vulkan adapter name; empty = default GPU
	bool useWarp = false;                              // D3D11/D3D12: force the WARP software adapter

	// Folders. Empty resourcesDir = "<exe dir>/resources" (needs resources/shaders, see kzgs_stage_resources()).
	// Empty cacheDir = "<exe dir>/cache" (compiled shader cache). disableShaderCache skips the cache entirely.
	std::string resourcesDir;
	std::string cacheDir;
	bool disableShaderCache = false;

	int maxQueuedFrames = 2;                           // kzgsVsync blocks when this many frames are still queued

	// true: every kzgsGifTransfer() call is a self-contained GIF packet sequence that starts with a GIFtag, as
	// PS2Recomp's GIF arbiter produces (it re-wraps IMAGE data continued across VIF DIRECT/DMA chunks in a synthesized
	// IMAGE tag). A tag still open at the end of a call is dropped. false: a raw, hardware-exact GIF byte stream per
	// path, where a packet may continue in the next call (PCSX2 MTGS semantics).
	bool selfContainedGifPackets = true;
	bool debugDevice = false;                          // D3D/Vulkan validation layer
};

// Creates the GS thread and the render device on window `hwnd` (a Win32 HWND; the window must outlive kzgsClose).
// `privRegs` is the caller's 0x2000-byte privileged register block (see above); it is read at open and at every
// kzgsVsync. Returns false with a message in *err on failure. Only one kzgs instance can be open at a time.
bool kzgsOpen(void* hwnd, const KzgsConfig& cfg, uint8_t* privRegs, std::string* err = nullptr);

// Destroys the device and joins the GS thread. Safe to call when not open.
void kzgsClose();

bool kzgsIsOpen();

// Queues a GIF packet (sequence of GIFtags + data) for PATH 1, 2 or 3. `qwords` is the size in 16-byte units.
// The data is copied; the caller may reuse its buffer immediately. PATH1 transfers stop at the first EOP, as on
// hardware XGKICK.
void kzgsGifTransfer(int path, const uint8_t* data, uint32_t qwords);

// End of frame: snapshots the privileged registers and makes the GS read out the display circuits and present to the
// window. `field` is the interlace field being displayed (0 = even, 1 = odd; PCSX2 derives it from !(CSR & 0x2000)).
// `regsWritten` = whether the game wrote any privileged register since the last vsync (PCSX2 uses it to decide whether
// an idle frame still needs presenting; pass true if unsure).
void kzgsVsync(int field, bool regsWritten = true);

// Write to GS CSR (0x12001000). Only the RESET bit (bit 9) has an effect on the GS itself.
void kzgsWriteCSR(uint32_t csr);

// GIF soft reset of the path state machines (mask bit0 = PATH1, bit1 = PATH2, bit2 = PATH3).
void kzgsSoftReset(uint32_t mask = 7);

// Full GS reset (clears VRAM/texture cache).
void kzgsReset();

// Window client area changed.
void kzgsResize(int width, int height);

// Applies new settings (renderer switches recreate the device on the same window).
void kzgsUpdateConfig(const KzgsConfig& cfg);

// Blocks until the GS thread has processed everything queued so far.
void kzgsSync();

// Reads back the last output frame (display circuits merged, before scaling to the window), at internal resolution:
// e.g. 640x448 at upscale 1, 1280x896 at upscale 2. RGBA8, top-down, w*h*4 bytes. Blocks (drains the queue first).
// With presentWidth/presentHeight > 0 the image is instead scaled and aspect-corrected the way it is presented in a
// window of that size (letterboxed with black to exactly presentWidth x presentHeight).
bool kzgsReadback(std::vector<uint8_t>& rgba, int& width, int& height, int presentWidth = 0, int presentHeight = 0);

// Optional sink for GS log lines (device info, warnings, errors). Called from the GS thread. nullptr = OutputDebugString.
using KzgsLogFn = void (*)(int level /*0 error, 1 warning, 2 info*/, const char* msg);
void kzgsSetLogCallback(KzgsLogFn fn);

// Optional frame-pipeline trace sink (timeline recorder, patch 0025). Called from the GS thread and from the producer
// thread, must be cheap and lock-free. id: 2 GS thread idle time in us since the previous frame (reported when a frame starts), 3/4 GS thread renders+presents a frame
// begin/end, 5/6 producer waits for the GS thread (maxQueuedFrames) begin/end, 7/8 producer waits for ring space
// begin/end, 9 frames queued (a) after a vsync was pushed. nullptr = off.
using KzgsTraceFn = void (*)(int id, uint32_t a);
void kzgsSetTraceHook(KzgsTraceFn fn);

// Measurement aid: after the next `count` kzgsVsync() frames that reach the GS thread, the GS thread copies the output
// (internal resolution, presentWidth/presentHeight 0; or as presented in such a window) into memory right after the
// frame is merged, without making the caller wait. kzgsBurstTake() returns them once all are captured.
struct KzgsBurstFrame
{
	std::vector<uint8_t> rgba;
	int width = 0, height = 0;
	int field = 0;
	int gameDeint = 0; // PCSX2 'game_deinterlacing' (selects the MAD/bob path in Merge)
	uint64_t seq = 0; // count of vsync commands the GS thread has processed
};
void kzgsBurstArm(int count, int presentWidth = 0, int presentHeight = 0);
bool kzgsBurstTake(std::vector<KzgsBurstFrame>& out); // true once the armed burst is complete (and moves it out)
