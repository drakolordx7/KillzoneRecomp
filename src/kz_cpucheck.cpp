// CPU check that runs before anything else in the process (issue #14).
//
// The rest of the exe is built for a CPU baseline (KZ_ISA in CMakeLists.txt, passed here as KZ_CPUCHECK_ISA):
//   0 MULTI: SSE4.1 (the AVX/AVX2 code paths are chosen at run time, so only the baseline is required)
//   1 SSE4:  SSE4.2 + POPCNT (x86-64-v2)
//   2 AVX2:  AVX2, FMA3, BMI1/2, LZCNT, F16C, MOVBE (x86-64-v3)
// On an older CPU the first unsupported instruction in the C runtime start-up or a static initializer kills the process
// with no window and no log. This file is compiled for plain x86-64 (see CMakeLists.txt) and kzCpuCheckEntry is the
// linker entry point: it checks the CPU, explains the problem in a message box and killzone.log when something is
// missing, and otherwise hands over to the normal C runtime start-up. Only Win32 calls here: the C runtime is not
// initialised yet.
//
// KZ_CPUCHECK_FAIL=1 shows the message on any CPU (to test the message itself).

#include <windows.h>
#include <intrin.h>

#pragma comment(lib, "user32.lib")

#ifndef KZ_CPUCHECK_ISA
#define KZ_CPUCHECK_ISA 2
#endif

extern "C" int mainCRTStartup(void);

namespace
{
    void append(wchar_t *dst, size_t cap, const wchar_t *src)
    {
        size_t n = 0;
        while (n < cap - 1 && dst[n])
            ++n;
        while (n < cap - 1 && *src)
            dst[n++] = *src++;
        dst[n] = 0;
    }

    bool bit(int v, int b) { return ((v >> b) & 1) != 0; }

    // Writes the missing feature names into `missing` (comma separated); returns true when all are present.
    bool cpuOk(wchar_t *missing, size_t cap)
    {
        int r[4] = {};
        __cpuid(r, 0);
        const int maxLeaf = r[0];
        __cpuid(r, 0x80000000);
        const unsigned maxExt = static_cast<unsigned>(r[0]);

        int l1[4] = {}, l7[4] = {}, e1[4] = {};
        if (maxLeaf >= 1)
            __cpuid(l1, 1);
        if (maxLeaf >= 7)
            __cpuidex(l7, 7, 0);
        if (maxExt >= 0x80000001u)
            __cpuid(e1, 0x80000001);

        const bool osxsave = bit(l1[2], 27);
        const bool osYmm = osxsave && ((_xgetbv(0) & 6) == 6); // Windows saves the AVX registers

        struct Feature { const wchar_t *name; bool have; };
#if KZ_CPUCHECK_ISA == 0
        (void)osYmm;
        const Feature features[] = {
            {L"SSE4.1", bit(l1[2], 19)},
            {L"SSSE3", bit(l1[2], 9)},
            {L"SSE3", bit(l1[2], 0)},
            {L"SSE2", bit(l1[3], 26)},
            {L"CMPXCHG16B", bit(l1[2], 13)},
        };
#elif KZ_CPUCHECK_ISA == 1
        (void)osYmm;
        const Feature features[] = {
            {L"SSE4.2", bit(l1[2], 20)},
            {L"SSE4.1", bit(l1[2], 19)},
            {L"SSSE3", bit(l1[2], 9)},
            {L"POPCNT", bit(l1[2], 23)},
            {L"CMPXCHG16B", bit(l1[2], 13)},
        };
#else
        const Feature features[] = {
            {L"AVX2", bit(l7[1], 5) && bit(l1[2], 28) && osYmm},
            {L"FMA3", bit(l1[2], 12)},
            {L"BMI1", bit(l7[1], 3)},
            {L"BMI2", bit(l7[1], 8)},
            {L"LZCNT", bit(e1[2], 5)},
            {L"F16C", bit(l1[2], 29)},
            {L"MOVBE", bit(l1[2], 22)},
            {L"POPCNT", bit(l1[2], 23)},
        };
#endif
        bool ok = true;
        missing[0] = 0;
        for (const Feature &f : features)
        {
            if (f.have)
                continue;
            if (!ok)
                append(missing, cap, L", ");
            append(missing, cap, f.name);
            ok = false;
        }
        return ok;
    }

    // killzone.log next to the exe, so a bug report carries the reason too.
    void writeLog(const wchar_t *text)
    {
        wchar_t path[MAX_PATH + 32];
        DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return;
        while (n > 0 && path[n - 1] != L'\\' && path[n - 1] != L'/')
            --n;
        path[n] = 0;
        append(path, sizeof(path) / sizeof(path[0]), L"killzone.log");
        HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return;
        char utf8[2048];
        const int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), nullptr, nullptr);
        if (len > 1)
        {
            DWORD written = 0;
            WriteFile(h, utf8, static_cast<DWORD>(len - 1), &written, nullptr);
            WriteFile(h, "\r\n", 2, &written, nullptr);
        }
        CloseHandle(h);
    }
}

extern "C" int kzCpuCheckEntry(void)
{
    wchar_t missing[128];
    bool ok = cpuOk(missing, sizeof(missing) / sizeof(missing[0]));
    wchar_t force[8];
    if (GetEnvironmentVariableW(L"KZ_CPUCHECK_FAIL", force, 8) == 1 && force[0] == L'1')
    {
        ok = false;
        if (!missing[0])
            append(missing, sizeof(missing) / sizeof(missing[0]), L"none (KZ_CPUCHECK_FAIL=1)");
    }
    if (ok)
        return mainCRTStartup();

#if KZ_CPUCHECK_ISA == 0
    const wchar_t *needs = L"This build needs a processor with SSE4.1: Intel Core 2 (45 nm, Penryn) or newer, "
                           L"or AMD FX (Bulldozer) or newer.";
    const wchar_t *needsShort = L"SSE4.1 (Intel Core 2 Penryn / AMD FX or newer)";
#elif KZ_CPUCHECK_ISA == 1
    const wchar_t *needs = L"This build needs a processor with SSE4.2 and POPCNT: Intel Core i3/i5/i7 (Nehalem) or newer, "
                           L"or AMD FX (Bulldozer) or newer.";
    const wchar_t *needsShort = L"SSE4.2 and POPCNT (Intel Nehalem / AMD FX or newer)";
#else
    const wchar_t *needs = L"This build needs a processor with AVX2: Intel Core 4th generation (Haswell) or newer, "
                           L"or AMD Ryzen or newer.";
    const wchar_t *needsShort = L"AVX2 (Intel Haswell / AMD Ryzen or newer)";
#endif
    wchar_t msg[1024] = L"Killzone can't run on this CPU.\n\n";
    append(msg, 1024, needs);
    append(msg, 1024, L"\n\nMissing: ");
    append(msg, 1024, missing);
    append(msg, 1024, L"\n\nSee https://github.com/drakolordx7/KillzoneRecomp/issues/8");

    wchar_t log[1024] = L"[cpu] This CPU is missing instructions the game needs: ";
    append(log, 1024, missing);
    append(log, 1024, L". It needs ");
    append(log, 1024, needsShort);
    append(log, 1024, L". See issue #8.");
    writeLog(log);

    MessageBoxW(nullptr, msg, L"Killzone", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}
