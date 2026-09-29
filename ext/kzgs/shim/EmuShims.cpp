// kzgs shim: emulator subsystems PCSX2's GS (and Pcsx2Config.cpp) reference but kzgs does not have.
// Everything here is a no-op or returns a neutral value; nothing in this file affects rendering.
// SPDX-License-Identifier: GPL-3.0+

#include "Counters.h"
#include "DebugTools/Debug.h"
#include "GS.h"
#include "GS/GS.h"
#include "GSDumpReplayer.h"
#include "Host/AudioStream.h"
#include "MTGS.h"
#include "Memory.h"
#include "MemoryTypes.h"
#include "PerformanceMetrics.h"
#include "R5900.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "SIO/Pad/Pad.h"
#include "SPU2/spu2.h"
#include "USB/USB.h"
#include "VMManager.h"

#include "common/CrashHandler.h"
#include "BuildVersion.h"

// ---- EE globals that PCSX2 headers bind static references to (Dmac.h, R5900.h) ---------------------------------------
alignas(__pagealignsize) u8 eeHw[Ps2MemSize::Hardware];
alignas(16) cpuRegistersPack _cpuRegistersPack;

u8* SysMemory::GetCodePtr(size_t offset)
{
	return nullptr;
}

void CrashHandler::WriteDumpForCaller() {}

// ---- Counters / video timing ------------------------------------------------------------------------------------------
double GetVerticalFrequency()
{
	switch (GSgetDisplayMode())
	{
		case GSVideoMode::PAL: return 50.0;
		case GSVideoMode::HDTV_1080I: return 60.0;
		default: return 59.94;
	}
}

const char* ReportVideoMode()
{
	switch (GSgetDisplayMode())
	{
		case GSVideoMode::PAL: return "PAL";
		case GSVideoMode::VESA: return "VESA";
		case GSVideoMode::SDTV_480P: return "SDTV 480p";
		case GSVideoMode::HDTV_720P: return "HDTV 720p";
		case GSVideoMode::HDTV_1080I: return "HDTV 1080i";
		case GSVideoMode::NTSC: return "NTSC";
		default: return "Unknown";
	}
}

const char* ReportInterlaceMode()
{
	return "Interlaced";
}

// ---- VMManager ------------------------------------------------------------------------------------------------------
bool VMManager::HasValidVM() { return true; }
std::string VMManager::GetDiscSerial() { return "SCUS-97402"; }
std::string VMManager::GetTitle(bool prefer_en) { return "Killzone"; }
u32 VMManager::GetDiscCRC() { return 0; }

const std::vector<u32>& VMManager::Internal::GetSoftwareRendererProcessorList()
{
	static const std::vector<u32> empty;
	return empty;
}

// ---- PerformanceMetrics ---------------------------------------------------------------------------------------------
void PerformanceMetrics::Update(bool gs_register_write, bool fb_blit, bool is_skipping_present) {}
void PerformanceMetrics::OnGPUPresent(float gpu_time, u64 vs_invocations, u64 ps_invocations) {}
void PerformanceMetrics::SetGSSWThreadCount(u32 count) {}
void PerformanceMetrics::SetGSSWThread(u32 index, Threading::ThreadHandle thread) {}
PerformanceMetrics::InternalFPSMethod PerformanceMetrics::GetInternalFPSMethod() { return InternalFPSMethod::None; }
void PerformanceMetrics::StartSavingMetrics(u32 seconds) {}
bool PerformanceMetrics::IsSavingMetrics() { return false; }
void PerformanceMetrics::DumpSavedMetrics() {}

// ---- GSDumpReplayer -------------------------------------------------------------------------------------------------
bool GSDumpReplayer::IsReplayingDump() { return false; }
bool GSDumpReplayer::IsRunner() { return false; }
void GSDumpReplayer::RenderUI() {}
void GSDumpReplayer::UpdateGSStats() {}

// ---- MTGS (only reached from PCSX2 hotkey handlers, which kzgs never fires) -----------------------------------------
void MTGS::WaitGS(bool syncRegs, bool weakWait, bool isMTVU) {}
void MTGS::RunOnGSThread(AsyncCallType func) { func(); }
void MTGS::ApplySettings() {}
void MTGS::ToggleSoftwareRendering() {}

// ---- SPU2 (video capture audio) -------------------------------------------------------------------------------------
u32 SPU2::GetConsoleSampleRate() { return 48000; }
void SPU2::SetAudioCaptureActive(bool active) {}

// ---- Config-struct helpers referenced by Pcsx2Config.cpp ------------------------------------------------------------
s32 USB::DeviceTypeNameToIndex(const std::string_view device) { return 0; }
const char* USB::DeviceTypeIndexToName(s32 device) { return "None"; }
std::string USB::GetConfigSection(int port) { return fmt::format("USB{}", port + 1); }

Pad::ControllerType Pad::GetDefaultPadType(u32 pad) { return Pad::ControllerType::NotConnected; }
static const Pad::ControllerInfo s_no_pad = {Pad::ControllerType::NotConnected, "None", "Not Connected", nullptr, {}, {}, {}};
const Pad::ControllerInfo* Pad::GetControllerInfo(Pad::ControllerType type) { return &s_no_pad; }
const Pad::ControllerInfo* Pad::GetControllerInfoByName(const std::string_view name) { return &s_no_pad; }
std::string Pad::GetConfigSection(u32 pad_index) { return fmt::format("Pad{}", pad_index + 1); }

uint FileMcd_GetMtapPort(uint slot) { return slot & 1; }
uint FileMcd_GetMtapSlot(uint slot) { return 0; }
bool FileMcd_IsMultitapSlot(uint slot) { return slot >= 2; }
std::string FileMcd_GetDefaultName(uint slot) { return fmt::format("Mcd{:03}.ps2", slot + 1); }

std::optional<AudioBackend> AudioStream::ParseBackendName(const char* str) { return std::nullopt; }
const char* AudioStream::GetBackendName(AudioBackend backend) { return "Null"; }
void AudioStreamParameters::LoadSave(SettingsWrapper& wrap, const char* section) {}

// ---- misc globals -----------------------------------------------------------------------------------------------------
bool FMVstarted = false; // set by PCSX2's EE counters when an IPU FMV starts; only affects texture dumping

namespace BuildVersion
{
	int GitTagHi = 2;
	int GitTagMid = 0;
	int GitTagLo = 0;
} // namespace BuildVersion
