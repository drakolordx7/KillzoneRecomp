#pragma once

// Host input -> PS2 DualShock 2 state, plus raw mouse deltas for the engine aim patch.
//
// Threading: the window thread feeds events (kzInputOnEvent) and polls gamepads (kzInputPoll); the game thread reads
// through kzPadProvider (registered with ps2SetPadProvider) and kzInputTakeMouseDelta. State is guarded by a mutex.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

union SDL_Event;

// PS2 pad button masks (active-low in the pad reply).
enum KzPadButton : uint16_t
{
    KZ_PAD_SELECT = 0x0001, KZ_PAD_L3 = 0x0002, KZ_PAD_R3 = 0x0004, KZ_PAD_START = 0x0008,
    KZ_PAD_UP = 0x0010, KZ_PAD_RIGHT = 0x0020, KZ_PAD_DOWN = 0x0040, KZ_PAD_LEFT = 0x0080,
    KZ_PAD_L2 = 0x0100, KZ_PAD_R2 = 0x0200, KZ_PAD_L1 = 0x0400, KZ_PAD_R1 = 0x0800,
    KZ_PAD_TRIANGLE = 0x1000, KZ_PAD_CIRCLE = 0x2000, KZ_PAD_CROSS = 0x4000, KZ_PAD_SQUARE = 0x8000,
};

// Keyboard/mouse virtual stick directions, bindable like buttons.
enum class KzVirtualAxis
{
    None,
    MoveForward, MoveBack, StrafeLeft, StrafeRight,
    LookUp, LookDown, LookLeft, LookRight,
    // D-pad presses that only act outside gameplay (menus). In gameplay the D-pad is the scope zoom axis.
    MenuUp, MenuDown, MenuLeft, MenuRight,
};

void kzInputInit(const std::filesystem::path &bindingsIni);
void kzInputShutdown();
void kzInputOnEvent(const SDL_Event &event);
void kzInputPoll(); // once per host frame
void kzInputSetCaptured(bool captured); // mouse captured by the game window (relative mode)

// Registered with ps2SetPadProvider. Port 0 slot 0 only; returns false for other ports so they report disconnected.
bool kzPadProvider(int port, int slot, uint8_t *data, size_t size);

// Mouse motion accumulated since the last call, in counts, sensitivity/invert already applied.
struct KzMouseDelta
{
    float dx = 0.0f;
    float dy = 0.0f;
};
KzMouseDelta kzInputTakeMouseDelta();

// True when the most recent input came from keyboard/mouse rather than a gamepad (pad-only assists are then disabled).
bool kzInputUsingKeyboardMouse();

// Hold-to-aim support (kz_aim): true while a key/mouse binding to R3 (the game's zoom toggle) is held and
// [Input] AimMode=Hold; such bindings then do not press R3 themselves. kzInputPulseButton presses pad buttons for a
// short time (the aim patch pulses R3 to bring the game's zoom state in line).
bool kzInputZoomHeld();
void kzInputPulseButton(uint16_t padMask, uint32_t milliseconds);

// When the engine aim patch is active, mouse motion goes there; otherwise it is mapped onto the right stick.
void kzInputSetAimPatchActive(bool active);

// Text entry (the game's on-screen keyboard, e.g. the profile name). The game thread calls kzInputSetTextEntryActive()
// every frame the keyboard screen is open; while it is, typing keys are queued as text instead of driving pad buttons.
// kzInputTakeTextKey() pops one key: a printable ASCII character, or KZ_TEXT_ENTER / KZ_TEXT_CANCEL / KZ_TEXT_BACKSPACE.
enum : uint16_t
{
    KZ_TEXT_BACKSPACE = 0x08,
    KZ_TEXT_ENTER = 0x0D,
    KZ_TEXT_CANCEL = 0x1B,
};
void kzInputSetTextEntryActive();
bool kzInputTakeTextKey(uint16_t &key);
// KZ_INPUT_SCRIPT "t:type=abc" queues text at t (automated tests of text entry; "\n" is not available, use t:enter).

// Clock for KZ_INPUT_SCRIPT event times (seconds). Default: wall time.
void kzInputSetScriptClock(double (*clock)());

// Raw Key=Target pairs from killzone.ini [Bindings], or the defaults when the section is missing.
std::vector<std::pair<std::string, std::string>> kzInputBindingPairs(const std::filesystem::path &bindingsIni);

// Current key -> in-game action list (defaults or killzone.ini [Bindings]) for the launcher.
std::vector<std::pair<std::string, std::string>> kzInputDescribeBindings(const std::filesystem::path &bindingsIni);

// Self-test of binding parsing and pad composition (no devices needed). Returns number of failures.
int kzInputSelfTest();
