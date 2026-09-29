// KZ_PROFILE=<start s>,<duration s>: statistical profiler. Samples every thread of the process at ~1 kHz for
// <duration> seconds, starting <start> seconds after launch. Stacks are unwound with RtlVirtualUnwind (no dbghelp in
// the sampling loop) and symbolized once at the end. Writes the self-time and inclusive-time share of the top
// functions of each busy thread.

#include "kz_sampler.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <mmsystem.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

namespace
{
    struct ProfSample
    {
        DWORD tid;
        int depth;
        DWORD64 pcs[32];
    };

    int unwindThread(HANDLE thread, DWORD64 *pcs, int maxFrames)
    {
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_FULL;
        if (!GetThreadContext(thread, &ctx))
            return 0;
        int n = 0;
        for (; n < maxFrames && ctx.Rip; ++n)
        {
            pcs[n] = ctx.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (!fn)
            {
                // Leaf function: the return address is at [rsp].
                ctx.Rip = *reinterpret_cast<DWORD64 *>(ctx.Rsp);
                ctx.Rsp += 8;
                continue;
            }
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
        }
        return n;
    }

    std::string symName(HANDLE process, DWORD64 address)
    {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
        auto *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (SymFromAddr(process, address, &displacement, sym))
            return sym->Name;
        char out[32];
        std::snprintf(out, sizeof(out), "0x%llx", static_cast<unsigned long long>(address));
        return out;
    }

    bool isWait(const std::string &top)
    {
        static const char *kWaits[] = {"Wait", "Delay", "NtRemoveIo", "GetMessage", "Sleep", "ZwRemoveIo"};
        for (const char *w : kWaits)
            if (top.find(w) != std::string::npos)
                return true;
        return false;
    }
}

void kzStartProfiler(double startSeconds, double durationSeconds, const char *path)
{
    const std::string outPath = path;
    std::thread([=]() {
        timeBeginPeriod(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int64_t>(startSeconds * 1000)));
        const DWORD self = GetCurrentThreadId();
        const DWORD pid = GetCurrentProcessId();
        std::vector<ProfSample> samples;
        samples.reserve(static_cast<size_t>(durationSeconds * 1000 * 16));
        std::map<DWORD, HANDLE> handles;
        const auto end = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(static_cast<int64_t>(durationSeconds * 1000));
        uint64_t ticks = 0;
        while (std::chrono::steady_clock::now() < end)
        {
            if ((ticks++ % 500) == 0) // refresh the thread list every ~0.5 s
            {
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                THREADENTRY32 te = {sizeof(te)};
                for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                {
                    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || handles.count(te.th32ThreadID))
                        continue;
                    if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                              FALSE, te.th32ThreadID))
                        handles[te.th32ThreadID] = h;
                }
                CloseHandle(snap);
            }
            for (auto &[tid, h] : handles)
            {
                if (SuspendThread(h) == static_cast<DWORD>(-1))
                    continue;
                ProfSample s{tid, 0, {}};
                s.depth = unwindThread(h, s.pcs, 32);
                ResumeThread(h);
                if (s.depth)
                    samples.push_back(s);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        timeEndPeriod(1);

        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(process, nullptr, TRUE);
        std::unordered_map<DWORD64, std::string> names;
        auto name = [&](DWORD64 a) -> const std::string & {
            auto it = names.find(a);
            if (it == names.end())
                it = names.emplace(a, symName(process, a)).first;
            return it->second;
        };
        struct Stats
        {
            uint64_t total = 0, busy = 0;
            std::unordered_map<std::string, uint64_t> self, incl;
            std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>> callers; // self fn -> chain
        };
        std::map<DWORD, Stats> stats;
        for (const ProfSample &s : samples)
        {
            Stats &st = stats[s.tid];
            ++st.total;
            const std::string &top = name(s.pcs[0]);
            if (isWait(top))
                continue;
            ++st.busy;
            ++st.self[top];
            std::string chain;
            for (int i = 1; i < std::min(s.depth, 6); ++i)
                chain += (i > 1 ? " <- " : "") + name(s.pcs[i]);
            ++st.callers[top][chain];
            std::unordered_set<std::string> seen;
            for (int i = 0; i < s.depth; ++i)
            {
                const std::string &n = name(s.pcs[i]);
                if (seen.insert(n).second)
                    ++st.incl[n];
            }
        }
        FILE *out = std::fopen(outPath.c_str(), "w");
        if (!out)
            return;
        std::fprintf(out, "profile: %.1f s, %zu samples\n", durationSeconds, samples.size());
        for (auto &[tid, st] : stats)
        {
            if (st.busy < st.total / 20 + 5)
                continue; // mostly idle thread
            std::fprintf(out, "\n=== thread %lu: busy %.1f%% (%llu of %llu samples)\n", tid,
                         100.0 * static_cast<double>(st.busy) / static_cast<double>(st.total),
                         static_cast<unsigned long long>(st.busy), static_cast<unsigned long long>(st.total));
            auto dump = [&](const char *title, const std::unordered_map<std::string, uint64_t> &m, size_t n) {
                std::vector<std::pair<uint64_t, std::string>> v;
                for (auto &[k, c] : m)
                    v.emplace_back(c, k);
                std::sort(v.rbegin(), v.rend());
                std::fprintf(out, "  -- %s\n", title);
                for (size_t i = 0; i < std::min(n, v.size()); ++i)
                    std::fprintf(out, "  %5.1f%%  %s\n", 100.0 * static_cast<double>(v[i].first) / static_cast<double>(st.busy),
                                 v[i].second.c_str());
            };
            dump("self", st.self, 40);
            {
                std::vector<std::pair<uint64_t, std::string>> tops;
                for (auto &[k, c] : st.self)
                    tops.emplace_back(c, k);
                std::sort(tops.rbegin(), tops.rend());
                std::fprintf(out, "  -- callers of the top self entries\n");
                for (size_t i = 0; i < std::min<size_t>(12, tops.size()); ++i)
                {
                    std::vector<std::pair<uint64_t, std::string>> ch;
                    for (auto &[k, c] : st.callers[tops[i].second])
                        ch.emplace_back(c, k);
                    std::sort(ch.rbegin(), ch.rend());
                    std::fprintf(out, "  %s\n", tops[i].second.c_str());
                    for (size_t j = 0; j < std::min<size_t>(3, ch.size()); ++j)
                        std::fprintf(out, "      %5.1f%%  <- %s\n", 100.0 * static_cast<double>(ch[j].first) / static_cast<double>(st.busy), ch[j].second.c_str());
                }
            }
            dump("inclusive", st.incl, 70);
        }
        std::fclose(out);
        std::fprintf(stderr, "[kz] profile written: %s\n", outPath.c_str());
    }).detach();
}
