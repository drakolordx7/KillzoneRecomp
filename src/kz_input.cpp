#include "kz_input.h"

#include "kz_config.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
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

    // PC defaults, derived from the game's own default controller map (profile settings +0xEC, docs/findings.md
    // "Controls"): R1 fire, R2 secondary fire, X action/use, O switch weapon, L1 grenade, square special item,
    // triangle reload, L2 crouch, L3 sprint, R3 zoom mode, Start pause, Select objectives. Killzone has no jump.
    // Every entry is overridable in killzone.ini [Bindings].
    constexpr std::pair<const char *, const char *> kDefaultBindings[] = {
        {"Mouse1", "R1"}, {"Mouse2", "R3"}, {"Mouse3", "R2"}, {"R", "Triangle"}, {"G", "L1"},
        {"E", "Cross"}, {"F", "Cross"}, {"Q", "Circle"}, {"WheelUp", "Circle"}, {"WheelDown", "Circle"},
        {"C", "L2"}, {"Left Ctrl", "L2"}, {"Left Shift", "L3"}, {"X", "Square"}, {"Mouse4", "Square"},
        {"Space", "Cross"}, {"Return", "Cross"},
        {"W", "MoveForward"}, {"S", "MoveBack"}, {"A", "StrafeLeft"}, {"D", "StrafeRight"},
        {"Up", "LookUp"}, {"Down", "LookDown"}, {"Left", "LookLeft"}, {"Right", "LookRight"},
        {"Escape", "Start"}, {"Tab", "Select"}, {"Backspace", "Triangle"},
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
        bool usingKbm = true; // last device with activity was keyboard/mouse (vs gamepad)
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

    double (*g_scriptClock)() = nullptr;

    // KZ_INPUT_SCRIPT="t:button[:dur];..." for automated runs: presses `button` (a pad button name, or
    // lx/ly/rx/ry with =value e.g. ly=-1) at t seconds after the first pad read, for dur seconds (default 0.15).
    // mx/my=counts inject one raw mouse motion (after sensitivity) at t, e.g. "70:mx=400".
    struct ScriptEvent
    {
        double t, dur;
        uint16_t mask;
        int axis; // 0 none, 1 lx, 2 ly, 3 rx, 4 ry, 5 mouse x, 6 mouse y
        float value;
        bool fired = false;
    };

    std::vector<ScriptEvent> &script()
    {
        static std::vector<ScriptEvent> events = []() {
            std::vector<ScriptEvent> out;
            const char *env = std::getenv("KZ_INPUT_SCRIPT");
            if (!env)
                return out;
            std::string all(env);
            size_t pos = 0;
            while (pos < all.size())
            {
                size_t end = all.find(';', pos);
                std::string item = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                pos = end == std::string::npos ? all.size() : end + 1;
                const size_t c1 = item.find(':');
                if (c1 == std::string::npos)
                    continue;
                ScriptEvent ev{std::atof(item.c_str()), 0.15, 0, 0, 0.0f};
                std::string what = item.substr(c1 + 1);
                if (const size_t c2 = what.find(':'); c2 != std::string::npos)
                {
                    ev.dur = std::atof(what.c_str() + c2 + 1);
                    what.resize(c2);
                }
                if (const size_t eq = what.find('='); eq != std::string::npos)
                {
                    static const char *axes[] = {"", "lx", "ly", "rx", "ry", "mx", "my"};
                    for (int a = 1; a <= 6; ++a)
                        if (_stricmp(what.substr(0, eq).c_str(), axes[a]) == 0)
                            ev.axis = a;
                    ev.value = static_cast<float>(std::atof(what.c_str() + eq + 1));
                }
                else
                    for (const auto &n : kButtonNames)
                        if (_stricmp(n.name, what.c_str()) == 0)
                            ev.mask = n.mask;
                if (ev.mask || ev.axis)
                    out.push_back(ev);
            }
            return out;
        }();
        return events;
    }

    void applyScript(uint16_t &pressed, float &lx, float &ly, float &rx, float &ry)
    {
        auto &events = script();
        if (events.empty())
            return;
        const double t = g_scriptClock ? g_scriptClock() : SDL_GetTicks() / 1000.0;
        for (const ScriptEvent &e : events)
        {
            if (t < e.t || t >= e.t + e.dur || e.axis > 4)
                continue;
            pressed |= e.mask;
            float *axis[] = {nullptr, &lx, &ly, &rx, &ry};
            if (e.axis)
                *axis[e.axis] = e.value;
        }
    }

    bool parseSource(const std::string &name, Binding &b)
    {
        if (_strnicmp(name.c_str(), "Mouse", 5) == 0 && name.size() == 6 && name[5] >= '1' && name[5] <= '5')
        {
            // Mouse1 left, Mouse2 right, Mouse3 middle, Mouse4/5 side buttons (SDL numbers middle 2, right 3).
            static constexpr int kSdlButton[6] = {0, SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE, SDL_BUTTON_X1, SDL_BUTTON_X2};
            b.kind = SourceKind::MouseButton;
            b.code = kSdlButton[name[5] - '0'];
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

    std::vector<std::pair<std::string, std::string>> loadBindingPairs(const std::filesystem::path &ini)
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
        return pairs;
    }

    std::vector<Binding> loadBindings(const std::filesystem::path &ini)
    {
        const auto pairs = loadBindingPairs(ini);
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
        applyScript(pressed, lx, ly, rx, ry);
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
        // DualShock 2 pressure bytes (used when the game switches the pad to pressure mode): right, left, up, down,
        // triangle, circle, cross, square, L1, R1, L2, R2.
        static constexpr uint16_t kPressureOrder[12] = {KZ_PAD_RIGHT, KZ_PAD_LEFT, KZ_PAD_UP, KZ_PAD_DOWN,
                                                        KZ_PAD_TRIANGLE, KZ_PAD_CIRCLE, KZ_PAD_CROSS, KZ_PAD_SQUARE,
                                                        KZ_PAD_L1, KZ_PAD_R1, KZ_PAD_L2, KZ_PAD_R2};
        for (int i = 0; i < 12; ++i)
            data[8 + i] = (pressed & kPressureOrder[i]) ? 0xFF : 0x00;
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
        s.usingKbm = true;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button < s.mouseButtons.size())
            s.mouseButtons[e.button.button] = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        s.usingKbm = true;
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
            s.usingKbm = true;
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
    if (s.padButtons || std::fabs(s.rx) > 0.3f || std::fabs(s.ry) > 0.3f || std::fabs(s.lx) > 0.3f || std::fabs(s.ly) > 0.3f)
        s.usingKbm = false;
}

bool kzInputUsingKeyboardMouse()
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.usingKbm;
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
    if (!script().empty())
    {
        const KzConfig &cfg = kzConfig();
        const double t = g_scriptClock ? g_scriptClock() : SDL_GetTicks() / 1000.0;
        for (ScriptEvent &e : script())
            if (e.axis > 4 && !e.fired && t >= e.t)
            {
                e.fired = true;
                (e.axis == 5 ? d.dx : d.dy) += e.value * cfg.mouseSensitivity * (e.axis == 6 && cfg.invertY ? -1.0f : 1.0f);
            }
    }
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
    check((static_cast<uint16_t>(~(d[2] | (d[3] << 8))) & KZ_PAD_CIRCLE) != 0, "WheelUp pulse -> Circle (switch weapon)");
    check(d[17] == 0xFF, "R1 pressure byte");
    compose(s, d, 200);
    check((static_cast<uint16_t>(~(d[2] | (d[3] << 8))) & KZ_PAD_CIRCLE) == 0, "WheelUp pulse expires");
    s.keys.fill(false);
    s.mouseButtons.fill(false);
    s.wheelUpUntil = 0;
    s.bindings = saved;
    return failures;
}

void kzInputSetScriptClock(double (*clock)())
{
    g_scriptClock = clock;
}

std::vector<std::pair<std::string, std::string>> kzInputBindingPairs(const std::filesystem::path &bindingsIni);

std::vector<std::pair<std::string, std::string>> kzInputDescribeBindings(const std::filesystem::path &bindingsIni)
{
    // In-game meaning of each pad target under Killzone's default controller map.
    static const std::pair<const char *, const char *> kMeaning[] = {
        {"MoveForward", "Move forward"}, {"MoveBack", "Move back"}, {"StrafeLeft", "Strafe left"},
        {"StrafeRight", "Strafe right"}, {"LookUp", "Look up"}, {"LookDown", "Look down"}, {"LookLeft", "Look left"},
        {"LookRight", "Look right"}, {"R1", "Fire"}, {"R2", "Secondary fire"}, {"Cross", "Use / confirm"},
        {"Circle", "Switch weapon"}, {"L1", "Throw grenade"}, {"Square", "Special item"}, {"Triangle", "Reload / back"},
        {"L2", "Crouch"}, {"L3", "Sprint"}, {"R3", "Zoom"}, {"Start", "Pause"}, {"Select", "Objectives"},
        {"Up", "D-pad up"}, {"Down", "D-pad down"}, {"Left", "D-pad left"}, {"Right", "D-pad right"},
    };
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto &[src, dst] : kzInputBindingPairs(bindingsIni))
    {
        std::string meaning = dst;
        for (const auto &[pad, text] : kMeaning)
            if (_stricmp(pad, dst.c_str()) == 0)
                meaning = text;
        out.emplace_back(src, meaning);
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> kzInputBindingPairs(const std::filesystem::path &bindingsIni)
{
    return loadBindingPairs(bindingsIni);
}
