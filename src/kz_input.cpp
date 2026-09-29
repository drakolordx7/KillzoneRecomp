#include "kz_input.h"

#include "kz_config.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    // Binding source: a keyboard scancode, a mouse button, or a wheel direction.
    enum class SourceKind
    {
        Key,
        MouseButton,
        WheelUp,
        WheelDown,
    };

    struct Binding
    {
        SourceKind kind = SourceKind::Key;
        int code = 0; // SDL_Scancode or SDL mouse button index
        uint16_t padButton = 0;
        KzVirtualAxis axis = KzVirtualAxis::None;
    };

    struct NamedButton
    {
        const char *name;
        uint16_t mask;
    };
    constexpr NamedButton kButtonNames[] = {
        {"Cross", KZ_PAD_CROSS}, {"Circle", KZ_PAD_CIRCLE}, {"Square", KZ_PAD_SQUARE}, {"Triangle", KZ_PAD_TRIANGLE},
        {"L1", KZ_PAD_L1}, {"R1", KZ_PAD_R1}, {"L2", KZ_PAD_L2}, {"R2", KZ_PAD_R2}, {"L3", KZ_PAD_L3}, {"R3", KZ_PAD_R3},
        {"Start", KZ_PAD_START}, {"Select", KZ_PAD_SELECT}, {"Up", KZ_PAD_UP}, {"Down", KZ_PAD_DOWN},
        {"Left", KZ_PAD_LEFT}, {"Right", KZ_PAD_RIGHT},
    };

    struct NamedAxis
    {
        const char *name;
        KzVirtualAxis axis;
    };
    constexpr NamedAxis kAxisNames[] = {
        {"MoveForward", KzVirtualAxis::MoveForward}, {"MoveBack", KzVirtualAxis::MoveBack},
        {"StrafeLeft", KzVirtualAxis::StrafeLeft}, {"StrafeRight", KzVirtualAxis::StrafeRight},
        {"LookUp", KzVirtualAxis::LookUp}, {"LookDown", KzVirtualAxis::LookDown},
        {"LookLeft", KzVirtualAxis::LookLeft}, {"LookRight", KzVirtualAxis::LookRight},
    };

    // Provisional PC defaults. The pad targets follow common PS2 FPS layouts and must be checked against the game's
    // own controls screen (docs/findings.md#controls); every entry is overridable in killzone.ini [Bindings].
    constexpr std::pair<const char *, const char *> kDefaultBindings[] = {
        {"W", "MoveForward"}, {"S", "MoveBack"}, {"A", "StrafeLeft"}, {"D", "StrafeRight"},
        {"Up", "LookUp"}, {"Down", "LookDown"}, {"Left", "LookLeft"}, {"Right", "LookRight"},
        {"Mouse1", "R1"}, {"Mouse2", "L1"}, {"R", "R2"}, {"G", "L2"},
        {"Space", "Cross"}, {"C", "Circle"}, {"Left Ctrl", "Circle"}, {"E", "Square"}, {"F", "Square"},
        {"Q", "Triangle"}, {"WheelUp", "Triangle"}, {"WheelDown", "Triangle"}, {"Left Shift", "L3"}, {"V", "R3"},
        {"Escape", "Start"}, {"Return", "Cross"}, {"Tab", "Select"},
        {"1", "Up"}, {"2", "Right"}, {"3", "Down"}, {"4", "Left"},
    };

    constexpr uint64_t kWheelPulseMs = 60; // a wheel notch holds its button this long so the game sees one press

    struct State
    {
        std::mutex mutex;
        std::vector<Binding> bindings;
        std::array<bool, SDL_SCANCODE_COUNT> keys{};
        std::array<bool, 8> mouseButtons{};
        uint64_t wheelUpUntil = 0;
        uint64_t wheelDownUntil = 0;
        float mouseDx = 0.0f; // raw counts since last take
        float mouseDy = 0.0f;
        float stickMouseDx = 0.0f; // counts since last pad read (right-stick fallback)
        float stickMouseDy = 0.0f;
        bool captured = false;
        bool aimPatchActive = false;
        SDL_Gamepad *gamepad = nullptr;
        // latest gamepad snapshot, taken on the window thread
        uint16_t padButtons = 0; // active-high
        float lx = 0, ly = 0, rx = 0, ry = 0, l2 = 0, r2 = 0;
    };

    State &state()
    {
        static State s;
        return s;
    }

    bool parseSource(const std::string &name, Binding &b)
    {
        if (_strnicmp(name.c_str(), "Mouse", 5) == 0 && name.size() == 6 && name[5] >= '1' && name[5] <= '5')
        {
            b.kind = SourceKind::MouseButton;
            b.code = name[5] - '0'; // SDL_BUTTON_LEFT == 1
            return true;
        }
        if (_stricmp(name.c_str(), "WheelUp") == 0)
        {
            b.kind = SourceKind::WheelUp;
            return true;
        }
        if (_stricmp(name.c_str(), "WheelDown") == 0)
        {
            b.kind = SourceKind::WheelDown;
            return true;
        }
        const SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
        if (sc == SDL_SCANCODE_UNKNOWN)
            return false;
        b.kind = SourceKind::Key;
        b.code = sc;
        return true;
    }

    bool parseTarget(const std::string &name, Binding &b)
    {
        for (const auto &n : kButtonNames)
            if (_stricmp(n.name, name.c_str()) == 0)
            {
                b.padButton = n.mask;
                return true;
            }
        for (const auto &n : kAxisNames)
            if (_stricmp(n.name, name.c_str()) == 0)
            {
                b.axis = n.axis;
                return true;
            }
        return false;
    }

    std::string trim(std::string s)
    {
        const auto b = s.find_first_not_of(" \t\r");
        const auto e = s.find_last_not_of(" \t\r");
        return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
    }

    std::vector<Binding> loadBindings(const std::filesystem::path &ini)
    {
        std::vector<std::pair<std::string, std::string>> pairs;
        std::ifstream in(ini);
        bool inSection = false, any = false;
        std::string line;
        while (in && std::getline(in, line))
        {
            line = trim(line);
            if (line.empty() || line[0] == ';')
                continue;
            if (line.front() == '[')
            {
                inSection = _stricmp(line.c_str(), "[Bindings]") == 0;
                continue;
            }
            const auto eq = line.find('=');
            if (inSection && eq != std::string::npos)
            {
                pairs.emplace_back(trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
                any = true;
            }
        }
        if (!any)
            for (const auto &[k, v] : kDefaultBindings)
                pairs.emplace_back(k, v);
        std::vector<Binding> out;
        for (const auto &[src, dst] : pairs)
        {
            Binding b;
            if (parseSource(src, b) && parseTarget(dst, b))
                out.push_back(b);
            else
                SDL_Log("[kz-input] ignoring binding '%s=%s'", src.c_str(), dst.c_str());
        }
        return out;
    }

    bool sourceActive(const State &s, const Binding &b, uint64_t now)
    {
        switch (b.kind)
        {
        case SourceKind::Key: return b.code > 0 && b.code < SDL_SCANCODE_COUNT && s.keys[b.code];
        case SourceKind::MouseButton: return b.code > 0 && b.code < static_cast<int>(s.mouseButtons.size()) && s.mouseButtons[b.code];
        case SourceKind::WheelUp: return now < s.wheelUpUntil;
        case SourceKind::WheelDown: return now < s.wheelDownUntil;
        }
        return false;
    }

    uint8_t toStickByte(float v)
    {
        v = std::clamp(v, -1.0f, 1.0f);
        return static_cast<uint8_t>(std::lround(127.5f + v * 127.5f));
    }

    float applyDeadzone(float v, float dz)
    {
        const float a = std::fabs(v);
        if (a <= dz)
            return 0.0f;
        return std::copysign((a - dz) / (1.0f - dz), v);
    }

    // Compose the 32-byte DualShock 2 reply from current state. Caller holds the mutex.
    void compose(State &s, uint8_t *data, uint64_t now)
    {
        uint16_t pressed = s.padButtons;
        float mx = 0, my = 0, lx = 0, ly = 0;
        for (const Binding &b : s.bindings)
        {
            if (!sourceActive(s, b, now))
                continue;
            pressed |= b.padButton;
            switch (b.axis)
            {
            case KzVirtualAxis::MoveForward: ly -= 1; break;
            case KzVirtualAxis::MoveBack: ly += 1; break;
            case KzVirtualAxis::StrafeLeft: lx -= 1; break;
            case KzVirtualAxis::StrafeRight: lx += 1; break;
            case KzVirtualAxis::LookUp: my -= 1; break;
            case KzVirtualAxis::LookDown: my += 1; break;
            case KzVirtualAxis::LookLeft: mx -= 1; break;
            case KzVirtualAxis::LookRight: mx += 1; break;
            default: break;
            }
        }
        const float dz = kzConfig().stickDeadzone;
        lx += applyDeadzone(s.lx, dz);
        ly += applyDeadzone(s.ly, dz);
        float rx = applyDeadzone(s.rx, dz) + mx;
        float ry = applyDeadzone(s.ry, dz) + my;
        if (!s.aimPatchActive)
        {
            // Fallback until the engine aim patch is active: mouse counts per pad read -> right stick deflection.
            constexpr float kCountsForFullDeflection = 12.0f;
            rx += s.stickMouseDx / kCountsForFullDeflection;
            ry += s.stickMouseDy / kCountsForFullDeflection;
        }
        s.stickMouseDx = s.stickMouseDy = 0.0f;
        if (s.r2 > 0.5f) pressed |= KZ_PAD_R2;
        if (s.l2 > 0.5f) pressed |= KZ_PAD_L2;

        std::memset(data, 0, 32);
        data[0] = 0x00;
        data[1] = 0x73; // analog mode
        const uint16_t activeLow = static_cast<uint16_t>(~pressed);
        data[2] = static_cast<uint8_t>(activeLow & 0xFF);
        data[3] = static_cast<uint8_t>(activeLow >> 8);
        data[4] = toStickByte(rx);
        data[5] = toStickByte(ry);
        data[6] = toStickByte(lx);
        data[7] = toStickByte(ly);
    }
}

void kzInputInit(const std::filesystem::path &bindingsIni)
{
    SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS);
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.bindings = loadBindings(bindingsIni);
}

void kzInputShutdown()
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.gamepad)
        SDL_CloseGamepad(s.gamepad);
    s.gamepad = nullptr;
}

void kzInputSetCaptured(bool captured)
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.captured = captured;
    if (!captured)
    {
        s.keys.fill(false);
        s.mouseButtons.fill(false);
    }
}

void kzInputSetAimPatchActive(bool active)
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.aimPatchActive = active;
}

void kzInputOnEvent(const SDL_Event &e)
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    switch (e.type)
    {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (e.key.scancode < SDL_SCANCODE_COUNT)
            s.keys[e.key.scancode] = e.type == SDL_EVENT_KEY_DOWN;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button < s.mouseButtons.size())
            s.mouseButtons[e.button.button] = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (e.wheel.y > 0)
            s.wheelUpUntil = SDL_GetTicks() + kWheelPulseMs;
        else if (e.wheel.y < 0)
            s.wheelDownUntil = SDL_GetTicks() + kWheelPulseMs;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (s.captured)
        {
            const KzConfig &cfg = kzConfig();
            const float dx = e.motion.xrel * cfg.mouseSensitivity;
            const float dy = e.motion.yrel * cfg.mouseSensitivity * (cfg.invertY ? -1.0f : 1.0f);
            s.mouseDx += dx;
            s.mouseDy += dy;
            s.stickMouseDx += dx;
            s.stickMouseDy += dy;
        }
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!s.gamepad)
            s.gamepad = SDL_OpenGamepad(e.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (s.gamepad && SDL_GetGamepadID(s.gamepad) == e.gdevice.which)
        {
            SDL_CloseGamepad(s.gamepad);
            s.gamepad = nullptr;
        }
        break;
    default:
        break;
    }
}

void kzInputPoll()
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.padButtons = 0;
    s.lx = s.ly = s.rx = s.ry = s.l2 = s.r2 = 0.0f;
    SDL_Gamepad *g = s.gamepad;
    if (!g)
        return;
    struct Map
    {
        SDL_GamepadButton b;
        uint16_t mask;
    };
    static constexpr Map kMap[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, KZ_PAD_CROSS}, {SDL_GAMEPAD_BUTTON_EAST, KZ_PAD_CIRCLE},
        {SDL_GAMEPAD_BUTTON_WEST, KZ_PAD_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH, KZ_PAD_TRIANGLE},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, KZ_PAD_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, KZ_PAD_R1},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, KZ_PAD_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, KZ_PAD_R3},
        {SDL_GAMEPAD_BUTTON_START, KZ_PAD_START}, {SDL_GAMEPAD_BUTTON_BACK, KZ_PAD_SELECT},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, KZ_PAD_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, KZ_PAD_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, KZ_PAD_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, KZ_PAD_RIGHT},
    };
    for (const Map &m : kMap)
        if (SDL_GetGamepadButton(g, m.b))
            s.padButtons |= m.mask;
    auto axis = [g](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(g, a) / 32767.0f; };
    s.lx = axis(SDL_GAMEPAD_AXIS_LEFTX);
    s.ly = axis(SDL_GAMEPAD_AXIS_LEFTY);
    s.rx = axis(SDL_GAMEPAD_AXIS_RIGHTX);
    s.ry = axis(SDL_GAMEPAD_AXIS_RIGHTY);
    s.l2 = axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    s.r2 = axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
}

bool kzPadProvider(int port, int slot, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;
    if (port != 0 || slot != 0)
    {
        // Only player 1 is mapped; other ports read as an idle pad (no buttons, sticks centered).
        std::memset(data, 0, 32);
        data[1] = 0x73;
        data[2] = data[3] = 0xFF;
        data[4] = data[5] = data[6] = data[7] = 0x80;
        return true;
    }
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    compose(s, data, SDL_GetTicks());
    return true;
}

KzMouseDelta kzInputTakeMouseDelta()
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    KzMouseDelta d{s.mouseDx, s.mouseDy};
    s.mouseDx = s.mouseDy = 0.0f;
    return d;
}

int kzInputSelfTest()
{
    int failures = 0;
    auto check = [&](bool ok, const char *what) {
        if (!ok)
        {
            SDL_Log("[kz-input selftest] FAIL: %s", what);
            ++failures;
        }
    };
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const auto saved = s.bindings;
    s.bindings.clear();
    for (const auto &[k, v] : kDefaultBindings)
    {
        Binding b;
        const bool ok = parseSource(k, b) && parseTarget(v, b);
        check(ok, k);
        if (ok)
            s.bindings.push_back(b);
    }
    s.keys.fill(false);
    s.mouseButtons.fill(false);
    s.padButtons = 0;
    s.lx = s.ly = s.rx = s.ry = s.l2 = s.r2 = 0;
    s.aimPatchActive = true;
    uint8_t d[32];
    compose(s, d, 0);
    check(d[1] == 0x73 && d[2] == 0xFF && d[3] == 0xFF, "idle: analog mode, no buttons");
    check(d[4] == 0x80 && d[5] == 0x80 && d[6] == 0x80 && d[7] == 0x80, "idle: sticks centered");
    s.keys[SDL_SCANCODE_W] = true;
    s.keys[SDL_SCANCODE_D] = true;
    s.mouseButtons[SDL_BUTTON_LEFT] = true;
    compose(s, d, 0);
    check(d[7] == 0x00, "W: left stick full up");
    check(d[6] == 0xFF, "D: left stick full right");
    const uint16_t pressed = static_cast<uint16_t>(~(d[2] | (d[3] << 8)));
    check((pressed & KZ_PAD_R1) != 0, "Mouse1 -> R1");
    s.wheelUpUntil = 100;
    compose(s, d, 50);
    check((static_cast<uint16_t>(~(d[2] | (d[3] << 8))) & KZ_PAD_TRIANGLE) != 0, "WheelUp pulse -> Triangle");
    compose(s, d, 200);
    check((static_cast<uint16_t>(~(d[2] | (d[3] << 8))) & KZ_PAD_TRIANGLE) == 0, "WheelUp pulse expires");
    s.keys.fill(false);
    s.mouseButtons.fill(false);
    s.wheelUpUntil = 0;
    s.bindings = saved;
    return failures;
}
