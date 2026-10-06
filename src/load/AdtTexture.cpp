// Native split-ADT reader: the split-tile texture manager and per-texture height-blend slot builder.
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

#include "AdtSplit.hpp"
#include "AdtSplitInternal.hpp"

#include <cstdint>
#include <cstring>

namespace
{
    using namespace wxl::runtime::adtsplit::detail;

    adt::Map_AreaLoadTexturesFn   g_origAreaLoadTextures   = nullptr;
    adt::Map_LoadTerrainTextureFn g_origLoadTerrainTexture = nullptr;

    /**
     * @brief Detours tile texture load so a split tile's tex-owner array is built from the real
     *        MDID FileDataIDs instead of the (absent) MTEX.
     *
     * Each MDID id is resolved through TextureFilePath.db2 to a client path; the paths are packed
     * NUL-terminated into the tile's persistent texNameBlob (MDID order = MCLY.textureId order) and that
     * blob is handed to the UNCHANGED stock builder. The builder sizes area+0x60 to the name count and
     * points each slot's name at the blob (resident for the tile lifetime). Nothing here touches the
     * ADT's MTEX -- the client reads the modern format natively, per the project rule.
     */
    void __fastcall hkAreaLoadTextures(void* area, void* edx, const void* mtexData, uint32_t mtexSize)
    {
        if (SplitTile* t = FindTileBrief(area); t && !t->texCache.mdid.empty())
        {
            if (t->texCache.texNameBlob.empty())
            {
                for (uint32_t fdid : t->texCache.mdid)
                {
                    const char* p = wxl_modern_adt::ResolveTexture(fdid);
                    const char* s = p ? p : ""; // unresolved -> empty name -> client renders green
                    t->texCache.texNameBlob.insert(t->texCache.texNameBlob.end(), s, s + std::strlen(s) + 1);
                }
            }
            g_origAreaLoadTextures(area, edx, t->texCache.texNameBlob.data(),
                                    static_cast<uint32_t>(t->texCache.texNameBlob.size()));
            return;
        }
        g_origAreaLoadTextures(area, edx, mtexData, mtexSize);
    }

    /**
     * @brief Detours the terrain per-slot texture loader (slot[1]=Load(slot[0])).
     *
     * For a split tile the slot name is already a fully-resolved client path (set above), so it is
     * opened as-is via the by-name map texture loader, bypassing the stock "_s.blp" suffix rewrite that assumes the
     * legacy base/diffuse+specular naming. An empty (unresolved) name yields a null handle, which the
     * client draws as the green missing-texture placeholder -- never fatal.
     */
    void __fastcall hkLoadTerrainTexture(void* area, void* edx, void** slot, uint32_t index)
    {
        if (SplitTile* t = FindTileBrief(area); t && !t->texCache.mdid.empty())
        {
            const char* name = static_cast<const char*>(slot[0]);
            slot[1] = (name && name[0])
                            ? wxl::game::Native<adt::Map_LoadTextureFn>(adt::kMapLoadTexture)(name)
                            : nullptr;
            return;
        }
        g_origLoadTerrainTexture(area, edx, slot, index);
    }

    /// i-th NUL-terminated string of a packed name blob, or null when out of range/unterminated.
    const char* NthName(const char* blob, uint32_t size, uint32_t index)
    {
        const char* p   = blob;
        const char* end = blob + size;
        for (uint32_t i = 0; p < end; ++i)
        {
            const char* s = p;
            while (p < end && *p) ++p;
            if (p >= end) return nullptr; // unterminated tail
            if (i == index) return s;
            ++p;
        }
        return nullptr;
    }

    /**
     * @brief Lazily builds a tile's per-texture height slots (MTXP params + "_h" texture handles).
     *
     * Modern semantics: record i = MTXP[i] when present, else {MTXF[i]
     * flags, scale 0, offset 1}; flag 0x1 or default {0,1} params degrade to the solid white height
     * (weight = plain alpha). Otherwise the "_h" texture resolves by MHID FileDataID (TextureFilePath.
     * db2) or the "<diffuse>_h.blp" name, created through the by-name map texture loader (content
     * streams in asynchronously, like every terrain texture). Existence is probed through the storage
     * seam first so a missing file degrades to white instead of a dead handle. Main thread only.
     */
    void BuildHeightSlots(SplitTile& t)
    {
        t.height.heightBuilt = true;
        uint32_t count = t.height.mtxpSize / 16u;
        if (t.texCache.mdid.size() > count) count = static_cast<uint32_t>(t.texCache.mdid.size());
        if (t.height.mhid.size() > count) count = static_cast<uint32_t>(t.height.mhid.size());
        if (count == 0 || count > 512) return; // sanity cap
        t.height.heightSlots.resize(count);

        uint32_t loaded = 0, white = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            HeightState::HeightSlot& hs = t.height.heightSlots[i];
            uint32_t flags = 0;
            if (t.height.mtxp && (i + 1) * 16u <= t.height.mtxpSize)
            {
                flags = Rd32(t.height.mtxp + i * 16u);
                std::memcpy(&hs.scale, t.height.mtxp + i * 16u + 4, 4);
                std::memcpy(&hs.offset, t.height.mtxp + i * 16u + 8, 4);
            }
            else
            {
                if (t.height.mtxfData && (i + 1) * 4u <= t.height.mtxfSize) flags = Rd32(t.height.mtxfData + i * 4u);
                hs.scale = 0.0f; hs.offset = 1.0f;
            }
            hs.exp = static_cast<uint8_t>((flags >> 4) & 0xFu); // UV-tiling exponent (modern semantics)
            if ((flags & 0x1u) != 0 ||                       // "no _s/_h variants exist"
                (hs.scale == 0.0f && hs.offset == 1.0f))     // default params -> white (matches modern semantics)
            {
                ++white;
                continue;
            }

            // Resolve the "_h" file: FileDataID first (modern MHID), then the name convention.
            char nameBuf[300];
            const char* path = nullptr;
            if (i < t.height.mhid.size() && t.height.mhid[i])
                path = wxl_modern_adt::ResolveTexture(t.height.mhid[i]);
            if (!path)
            {
                const char* dif = nullptr;
                if (!t.texCache.texNameBlob.empty())
                    dif = NthName(t.texCache.texNameBlob.data(), static_cast<uint32_t>(t.texCache.texNameBlob.size()), i);
                else if (t.height.mtexData)
                    dif = NthName(reinterpret_cast<const char*>(t.height.mtexData), t.height.mtexSizeParked, i);
                if (dif && dif[0])
                {
                    size_t len = std::strlen(dif);
                    if (len > 4 && _stricmp(dif + len - 4, ".blp") == 0) len -= 4;
                    if (len + 7 <= sizeof nameBuf)
                    {
                        std::memcpy(nameBuf, dif, len);
                        std::memcpy(nameBuf + len, "_h.blp", 7);
                        path = nameBuf;
                    }
                }
            }
            if (path && path[0])
            {
                if (ScopedFile probe(path); probe) // storage seam: host + archives both answer
                {
                    hs.tex   = wxl::game::Native<adt::Map_LoadTextureFn>(adt::kMapLoadTexture)(path);
                    hs.owned = hs.tex != nullptr;
                }
            }
            if (hs.tex) ++loaded; else ++white;
        }
        g_statHeightTex.fetch_add(loaded, std::memory_order_relaxed);
        WLOG_INFO("adt-split: tile %d_%d height slots built (%u textures, %u _h loaded, %u white)",
                  t.tileFirst, t.tileSecond, count, loaded, white);
    }
}

namespace wxl::runtime::adtsplit
{
    detail::SplitTile* ResolveHeightTile(void* area)
    {
        detail::SplitTile* t = detail::FindTileBrief(area);
        if (!t || !t->complete || !t->height.mtxp) return nullptr; // no MTXP = nothing to blend by height
        if (!t->height.heightBuilt) BuildHeightSlots(*t);          // main thread, same class as load/teardown
        return t;
    }

    void GetHeightLayer(detail::SplitTile* tile, uint32_t textureId, HeightLayer& out)
    {
        if (textureId < tile->height.heightSlots.size())
        {
            const detail::HeightState::HeightSlot& hs = tile->height.heightSlots[textureId];
            out.texture      = hs.tex;
            out.heightScale  = hs.scale;
            out.heightOffset = hs.offset;
            out.tilingExp    = hs.exp;
        }
        else
        {
            out.texture = nullptr; out.heightScale = 0.0f; out.heightOffset = 1.0f; out.tilingExp = 0;
        }
    }

    bool GetHeightLayer(void* area, uint32_t textureId, HeightLayer& out)
    {
        detail::SplitTile* t = ResolveHeightTile(area);
        if (!t) return false;
        GetHeightLayer(t, textureId, out);
        return true;
    }
}

namespace wxl::runtime::adtsplit::detail
{
    bool InstallTextures()
    {
        const bool a = wxl_modern_adt::HookAttachByName("Adt.AreaLoadTextures",
                                                 &hkAreaLoadTextures, &g_origAreaLoadTextures);
        const bool b = wxl_modern_adt::HookAttachByName("Adt.LoadTerrainTexture",
                                                 &hkLoadTerrainTexture, &g_origLoadTerrainTexture);
        return a && b;
    }
}
