// Native split-ADT reader: split-map detection (the once-per-map _tex0 probe) and the per-load
// alpha-layout flag the reader depends on.
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

#include "AdtSplitInternal.hpp"

#include <cstring>
#include <string>

namespace
{
    using namespace wxl::runtime::adtsplit::detail;

    /**
     * @brief Derives the per-map cache key from a tile filename.
     *
     * "<dir>\<name>_%d_%d.adt" -> "<dir>\<name>". Returns false when the name does not look like a tile
     * path (no ".adt" tail or no two trailing _<digits> groups) -- such a name is never split.
     */
    bool MapKeyFromTileName(const char* name, std::string& key)
    {
        const size_t len = name ? std::strlen(name) : 0;
        if (len < 9) return false; // "a_0_0.adt" is the shortest plausible
        const char* ext = name + len - 4;
        if (_stricmp(ext, ".adt") != 0) return false;

        // scan back across two _<digits> groups
        const char* p = ext;
        for (int group = 0; group < 2; ++group)
        {
            const char* d = p;
            while (d > name && d[-1] >= '0' && d[-1] <= '9') --d;
            if (d == p || d == name || d[-1] != '_') return false;
            p = d - 1;
        }
        key.assign(name, static_cast<size_t>(p - name));
        return true;
    }
}

namespace wxl::runtime::adtsplit::detail
{
    /**
     * @brief Reports (and lazily probes) whether the map a tile filename belongs to is split.
     *
     * The probe opens "<prefix>_%d_%d_tex0.adt" of THIS tile once through the storage seam and caches
     * the answer per map prefix -- an all-or-nothing-per-map dataset assumption, matching how repacks
     * ship. Cost: one open/close per map per session.
     */
    bool IsSplitTileName(const char* name, std::string& keyOut)
    {
        if (!MapKeyFromTileName(name, keyOut)) return false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_splitMaps.find(keyOut);
            if (it != g_splitMaps.end()) return it->second;
        }
        const size_t len = std::strlen(name);
        std::string probe(name, len - 4);
        probe += "_tex0.adt";
        const bool split = static_cast<bool>(ScopedFile(probe.c_str()));
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_splitMaps[keyOut] = split;
        }
        if (split)
        {
            g_statSplitMaps.fetch_add(1, std::memory_order_relaxed);
            WLOG_INFO("adt-split: map '%s' detected as SPLIT (root/_tex0/_obj0 layout)", keyOut.c_str());
        }
        return split;
    }

    /**
     * @brief Makes the map's alpha-layout flag agree with the coverage a split map actually ships.
     *
     * A map that advertises per-layer height texturing authors its layer coverage one byte per texel,
     * but the terrain build only sizes for that when the map header says so outright -- and the two
     * statements disagree in every split dataset we read. The bit is therefore OR'd in, which also
     * selects the shader family the height blend publishes into.
     *
     * The header word this touches is rebuilt from the map's own file every time a world loads, so
     * this is re-armed per load rather than once per session: a cheap idempotent OR, silent once the
     * bit is already standing. Nothing happens on a map that does not advertise height texturing, so
     * a classic dataset is never touched.
     */
    void EnsureAlphaLayoutFlag()
    {
        uint32_t& flags = *reinterpret_cast<uint32_t*>(adt::kMphdFlags);
        if ((flags & kMapHeightTexturing) != 0 && (flags & kMapWideAlpha) == 0)
        {
            flags |= kMapWideAlpha;
            WLOG_INFO("adt-split: map advertises height texturing; selecting the wide layer-coverage layout");
        }
    }
}
