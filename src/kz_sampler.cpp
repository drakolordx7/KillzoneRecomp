// Periodic stack sampler: every N seconds, suspends each other thread in the process, walks its stack with
// dbghelp and prints the top frames. Generated functions are named FUN_<ee address>, so the output shows which
// guest code is running — the main tool for diagnosing hangs during bring-up.

#include "kz_sampler.h"

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#pragma comment(lib, "dbghelp.lib")

namespace
{
    std::string describe(HANDLE process, DWORD64 address)
    {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
        auto *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 displacement = 0;
        char out[320];
        if (SymFromAddr(process, address, &displacement, sym))
            std::snprintf(out, sizeof(out), "%s+0x%llx", sym->Name, static_cast<unsigned long long>(displacement));
        else
            std::snprintf(out, sizeof(out), "0x%llx", static_cast<unsigned long long>(address));
        return out;
    }

    void sampleThread(HANDLE process, DWORD tid, int maxFrames, FILE *out)
    {
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!thread)
            return;
        if (SuspendThread(thread) == static_cast<DWORD>(-1))
        {
            CloseHandle(thread);
            return;
        }
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &ctx))
        {
            STACKFRAME64 frame = {};
            frame.AddrPC.Offset = ctx.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = ctx.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = ctx.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;
            std::fprintf(out, "  thread %lu:\n", tid);
            for (int i = 0; i < maxFrames; ++i)
            {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr,
                                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
                    frame.AddrPC.Offset == 0)
                    break;
                std::fprintf(out, "    #%02d %s\n", i, describe(process, frame.AddrPC.Offset).c_str());
            }
        }
        ResumeThread(thread);
        CloseHandle(thread);
    }
}

void kzStartStackSampler(int intervalSeconds, int maxFrames, const char *path)
{
    std::thread([=]() {
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(process, nullptr, TRUE);
        const DWORD self = GetCurrentThreadId();
        for (int n = 1;; ++n)
        {
            std::this_thread::sleep_for(std::chrono::seconds(intervalSeconds));
            FILE *out = std::fopen(path, "a");
            if (!out)
                continue;
            std::fprintf(out, "=== sample %d (t=%ds)\n", n, n * intervalSeconds);
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te = {sizeof(te)};
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            {
                if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != self)
                    sampleThread(process, te.th32ThreadID, maxFrames, out);
            }
            CloseHandle(snap);
            std::fclose(out);
        }
    }).detach();
}
