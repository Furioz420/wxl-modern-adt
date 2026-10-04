// wxl-modern-adt: the extension-wide service table pointer and hook-install convenience, shared by every
// translation unit in this DLL.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include "common/ExtensionConfig.hpp"
#include "wxl/FdidApi.h"
#include "wxl/LoadPoolApi.h"
#include "wxl/PluginApi.h"

#include <cstdint>
#include <cstdlib>

/// The core hands the service table to WXL_Load once and it "lives for the process lifetime" (see
/// PluginApi.h), so every detour installed later reaches it through this one process-lifetime pointer
/// rather than threading an `api` parameter through every call in the chain -- including callbacks the
/// engine itself invokes by address (async completions), which have no parameter list of ours to extend.
namespace wxl_modern_adt
{
    extern const WXL_Api* g_api;

    /// wxl-db2's FileDataID resolver. Fetched lazily rather than at load: extensions load in
    /// alphabetical folder order (wxl-modern-adt before wxl-db2), so wxl-db2 may not have published it yet
    /// when this extension's own WXL_Load runs. By the time any real tile load asks for a path, every
    /// extension has finished loading, so a cache-on-first-use is both correct and free afterwards.
    extern const WXL_FdidApi* g_fdid;

    inline const WXL_FdidApi* Fdid()
    {
        if (!g_fdid)
            g_fdid = static_cast<const WXL_FdidApi*>(g_api->GetInterface("wxl.fdid", WXL_FDID_API_VERSION));
        return g_fdid;
    }

    /// wxl-engine-reforged's background job pool for load-time fixup work. Same lazy-fetch reasoning
    /// as Fdid() (wxl-modern-adt loads before wxl-engine-reforged alphabetically). A caller that gets null
    /// back (extension absent/disabled) falls back to calling its stage1/stage2 inline itself.
    extern const WXL_LoadPoolApi* g_loadPool;

    inline const WXL_LoadPoolApi* LoadPool()
    {
        if (!g_loadPool)
            g_loadPool = static_cast<const WXL_LoadPoolApi*>(
                g_api->GetInterface("wxl.loadpool", WXL_LOADPOOL_API_VERSION));
        return g_loadPool;
    }

    /// True builds the whole native ADT pipeline in; false is the single point that would leave the
    /// client on its stock 335 ADT parser (ex wxl::features::modernADTSupport).
    inline constexpr bool kEnabled = true;

    /// Model FileDataID (MDDF/MODF) -> backslash path, or null when unresolved (including "wxl-db2
    /// not loaded", the same fallback every call site already takes for an unresolved id).
    inline const char* ResolveModel(uint32_t fileDataId)
    {
        const WXL_FdidApi* fdid = Fdid();
        return fdid ? fdid->ResolveModel(fileDataId) : nullptr;
    }

    /// Texture FileDataID (MDID/MHID) -> backslash path, or null when unresolved.
    inline const char* ResolveTexture(uint32_t fileDataId)
    {
        const WXL_FdidApi* fdid = Fdid();
        return fdid ? fdid->ResolveTexture(fileDataId) : nullptr;
    }

    /**
     * @brief Typed detour install over WXL_Api::HookAttach: detour and original share one function
     *        type, deduced, so wiring a hook to the wrong original no longer compiles. Mirrors
     *        wxl::hook::Install's own convenience overload, which this extension cannot link against.
     */
    template <class Fn>
    inline int HookAttach(const char* name, uintptr_t target, Fn* detour, Fn** original,
                          int priority = WXL_HOOK_DEFAULT_PRIORITY)
    {
        return g_api->HookAttach(name, target, reinterpret_cast<void*>(detour),
                                 reinterpret_cast<void**>(original), priority);
    }

    /**
     * @brief Typed detour install over WXL_Api::HookAttachByName: same deduced-Fn safety as
     *        HookAttach, but resolves the target by name through the core's own hook-point table
     *        (see HookPoints.cpp) instead of an offsets/ address -- this extension needs no offsets/
     *        header just to name a hook target.
     */
    template <class Fn>
    inline int HookAttachByName(const char* pointName, Fn* detour, Fn** original,
                                int priority = WXL_HOOK_DEFAULT_PRIORITY)
    {
        return g_api->HookAttachByName(pointName, reinterpret_cast<void*>(detour),
                                       reinterpret_cast<void**>(original), priority);
    }

    // The two always-on installers (unconditional, not gated by kEnabled -- see their own files),
    // called from WXL_Load in Module.cpp.
    bool InstallChunkBuild();  // ChunkBuild.cpp
    bool InstallHeightBlend(); // HeightBlend.cpp

    /// Every extension keeps its own <name>.cfg next to its DLL rather than share one file, so a
    /// config edit for one module can never collide with another's key names.
    inline bool ConfigTruthy(const char* raw, bool fallback) { return wxl::ext::config::Truthy(raw, fallback); }

    inline bool ConfigRaw(const char* name, char* buf, size_t cap)
    {
        return wxl::ext::config::Raw(name, buf, cap, "Extensions\\wxl-modern-adt\\wxl-modern-adt.cfg");
    }

    inline uint64_t ConfigU64(const char* name, uint64_t fallback, uint64_t minValue, uint64_t maxValue)
    {
        char value[32] = {};
        if (!ConfigRaw(name, value, sizeof value)) return fallback;
        char* end = nullptr;
        const uint64_t parsed = std::strtoull(value, &end, 10);
        if (end == value) return fallback;
        if (parsed < minValue) return minValue;
        if (parsed > maxValue) return maxValue;
        return parsed;
    }

    inline uint32_t ConfigU32(const char* name, uint32_t fallback, uint32_t minValue, uint32_t maxValue)
    {
        return static_cast<uint32_t>(ConfigU64(name, fallback, minValue, maxValue));
    }

    inline float ConfigFloat(const char* name, float fallback, float minValue, float maxValue)
    {
        char value[32] = {};
        if (!ConfigRaw(name, value, sizeof value)) return fallback;
        char* end = nullptr;
        const float parsed = std::strtof(value, &end);
        if (end == value) return fallback;
        if (parsed < minValue) return minValue;
        if (parsed > maxValue) return maxValue;
        return parsed;
    }
}

// common/Log.hpp's WLOG_* macros need common/Log.cpp linked in, which is core/host/patcher-only (see
// its own doc comment) -- an extension has no such object file, so these route the same call-site
// syntax through WXL_Api::Log instead. Not namespaced: WLOG_* is meant to read as a bare keyword at
// every call site, exactly like the core's own.
#define WLOG_TRACE(...) ::wxl_modern_adt::g_api->Log(WXL_LOG_TRACE, "wxl-modern-adt", __VA_ARGS__)
#define WLOG_DEBUG(...) ::wxl_modern_adt::g_api->Log(WXL_LOG_DEBUG, "wxl-modern-adt", __VA_ARGS__)
#define WLOG_INFO(...)  ::wxl_modern_adt::g_api->Log(WXL_LOG_INFO,  "wxl-modern-adt", __VA_ARGS__)
#define WLOG_WARN(...)  ::wxl_modern_adt::g_api->Log(WXL_LOG_WARN,  "wxl-modern-adt", __VA_ARGS__)
#define WLOG_ERROR(...) ::wxl_modern_adt::g_api->Log(WXL_LOG_ERROR, "wxl-modern-adt", __VA_ARGS__)
