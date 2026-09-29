// HLE for libsif loadfile calls the runtime lacks: sceSifSearchModuleByName and sceSifUnloadModule.
//
// Without these, the recompiled SDK versions bind the IOP LOADFILE RPC (sid 0x80000006), which never exists in the
// BIOS-less runtime, and spin forever (docs/findings.md#library-bindings). Both work on the runtime's own SIF module
// tracker (g_sif_modules_by_id / g_sif_module_id_by_path, filled by sceSifLoadModule).

#include "kz_sif_modules.h"

#include "Syscalls/Common.h"

#include <string>

namespace
{
    // IOP kernel error for an unknown module (KE_UNKNOWN_MODULE).
    constexpr int32_t kUnknownModule = -202;

    std::string baseName(std::string key)
    {
        const size_t slash = key.find_last_of("\\/:");
        if (slash != std::string::npos)
            key.erase(0, slash + 1);
        const size_t semi = key.find(';');
        if (semi != std::string::npos)
            key.erase(semi);
        const size_t dot = key.rfind('.');
        if (dot != std::string::npos)
            key.erase(dot);
        return toLowerAscii(key);
    }

    // Killzone passes either a load path or a bare module name ("dev9"); match on the full normalized path first,
    // then on the file's base name.
    int32_t findLoadedModule(const std::string &query)
    {
        const std::string pathKey = normalizeSifModulePathKey(query);
        const std::string wantBase = baseName(query);
        std::lock_guard<std::mutex> lock(g_sif_module_mutex);
        if (auto it = g_sif_module_id_by_path.find(pathKey); it != g_sif_module_id_by_path.end())
        {
            auto rec = g_sif_modules_by_id.find(it->second);
            if (rec != g_sif_modules_by_id.end() && rec->second.loaded)
                return rec->first;
        }
        for (const auto &[id, rec] : g_sif_modules_by_id)
        {
            if (rec.loaded && baseName(rec.pathKey) == wantBase)
                return id;
        }
        return kUnknownModule;
    }
}

void kzSceSifSearchModuleByName(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    (void)runtime;
    ctx->pc = getRegU32(ctx, 31);
    const std::string name = readGuestCStringBounded(rdram, getRegU32(ctx, 4), kMaxSifModulePathBytes);
    const int32_t id = name.empty() ? kUnknownModule : findLoadedModule(name);
    RUNTIME_LOG("[kz] sceSifSearchModuleByName(\"" << name << "\") = " << id);
    setReturnS32(ctx, id);
}

void kzSceSifUnloadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    (void)rdram;
    (void)runtime;
    ctx->pc = getRegU32(ctx, 31);
    const int32_t id = static_cast<int32_t>(getRegU32(ctx, 4));
    std::lock_guard<std::mutex> lock(g_sif_module_mutex);
    auto it = g_sif_modules_by_id.find(id);
    if (it == g_sif_modules_by_id.end())
    {
        setReturnS32(ctx, kUnknownModule);
        return;
    }
    if (auto p = g_sif_module_id_by_path.find(it->second.pathKey); p != g_sif_module_id_by_path.end() && p->second == id)
        g_sif_module_id_by_path.erase(p);
    g_sif_modules_by_id.erase(it);
    setReturnS32(ctx, 0);
}
