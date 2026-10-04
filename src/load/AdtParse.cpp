// Native split-ADT reader: parses the root/_tex0/_obj0 trio and applies the in-place structural fixups.
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

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace wxl::runtime::adtsplit::detail;

    // Modern liquid placement redirect + per-type vertex formats (generated tables).
    #include "LiquidRemapData.inc"

    // Coverage sizing. The tile normalizes the map-header amplitude byte below, so a chunk's alpha
    // grid is always 64x64 texels; what one layer then occupies follows from the map's alpha layout
    // alone -- one byte per texel (wide) or one nibble (narrow) -- and the shadow bitmap from one bit.
    constexpr uint32_t kAlphaDim          = 64;
    constexpr uint32_t kAlphaWideBytes    = kAlphaDim * kAlphaDim;
    constexpr uint32_t kAlphaNarrowBytes  = kAlphaDim * kAlphaDim / 2;
    constexpr uint32_t kShadowBytes       = kAlphaDim * kAlphaDim / 8;
    constexpr uint32_t kMaxLayers         = 4;      ///< layer records one chunk has room for
    constexpr uint32_t kLayerEntrySize    = 16;
    constexpr uint32_t kLayerUsesAlpha    = 0x100u; ///< the layer carries a coverage map
    constexpr uint32_t kLayerAlphaPacked  = 0x200u; ///< run-length packed, honoured on the wide layout only

    /// Indexes one ROOT MCNK (0x80-byte SMChunk header + sub-chunks) into the chunk's fill record.
    void WalkRootMcnk(ChunkFill& f, uint8_t* hdr, uint32_t chunkSize)
    {
        f.rootMcnkHdr = hdr;
        if (chunkSize < 0x80) return;
        uint8_t* p   = hdr + 8 + 0x80;
        uint32_t rem = chunkSize - 0x80;
        while (rem >= 8)
        {
            const uint32_t tag = Rd32(p);
            const uint32_t sz  = Rd32(p + 4);
            if (sz > rem - 8) break;
            uint8_t* payload = p + 8;
            switch (tag)
            {
            case FourCC("MCVT"): f.mcvt = payload; break;
            case FourCC("MCCV"): f.mccv = payload; break;
            case FourCC("MCLV"): f.mclv = payload; break;      // parked (split-format baked light)
            case FourCC("MCNR"): f.mcnr = payload; break;
            case FourCC("MCLQ"): f.mclq = payload; f.mclqSize = sz; break;
            case FourCC("MCSE"): f.mcse = payload; f.mcseSize = sz; break;
            default: break; // MCDD/MCBB/... silently skipped
            }
            uint32_t adv = 8 + sz;
            // Monolithic-era MCNR stores size 0x1B3 with 13 physical pad bytes before the next tag;
            // resync when a split root kept that layout. (Direct-fill itself never needs the pad: no
            // consumer reads past the 435 normal bytes and the stock walker is bypassed.)
            if (tag == FourCC("MCNR") && sz == 0x1B3 && rem >= adv + 13 + 8 &&
                !LooksLikeSubTag(p + adv) && LooksLikeSubTag(p + adv + 13))
                adv += 13;
            if (adv > rem) break;
            p += adv;
            rem -= adv;
        }
    }

    /// Indexes one HEADER-LESS _tex0 MCNK (sub-chunks from data+0).
    void WalkTexMcnk(ChunkFill& f, uint8_t* data, uint32_t size)
    {
        uint32_t off = 0;
        while (off + 8 <= size)
        {
            const uint32_t tag = Rd32(data + off);
            const uint32_t sz  = Rd32(data + off + 4);
            if (sz > size - off - 8) break;
            uint8_t* payload = data + off + 8;
            switch (tag)
            {
            case FourCC("MCLY"): f.mcly = payload; f.mclySize = sz; break;
            case FourCC("MCAL"): f.mcal = payload; f.mcalSize = sz; break;
            case FourCC("MCSH"): f.mcsh = payload; f.mcshSize = sz; break;
            default: break; // MCMT/MAMP-level oddities skipped
            }
            off += 8 + sz;
        }
    }

    /// Indexes one HEADER-LESS _obj0 MCNK (sub-chunks from data+0).
    void WalkObjMcnk(ChunkFill& f, uint8_t* data, uint32_t size)
    {
        uint32_t off = 0;
        while (off + 8 <= size)
        {
            const uint32_t tag = Rd32(data + off);
            const uint32_t sz  = Rd32(data + off + 4);
            if (sz > size - off - 8) break;
            uint8_t* payload = data + off + 8;
            switch (tag)
            {
            case FourCC("MCRD"): f.mcrd = payload; f.mcrdSize = sz; break;
            case FourCC("MCRW"): f.mcrw = payload; f.mcrwSize = sz; break;
            default: break; // MCBB etc. skipped
            }
            off += 8 + sz;
        }
    }

    /// Collapses the split-format 8x8-bit high-res hole map to the stock 4x4-bit u16 (a low cell is holed when
    /// ANY of its 2x2 high bits is).
    uint16_t CollapseHoles(uint64_t hi)
    {
        uint16_t low = 0;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
            {
                const int hr = r * 2, hc = c * 2;
                const uint64_t mask = (3ull << (hr * 8 + hc)) | (3ull << ((hr + 1) * 8 + hc));
                if (hi & mask) low |= uint16_t(1u << (r * 4 + c));
            }
        return low;
    }

    /**
     * @brief Bytes one run-length packed coverage layer consumes, or 0 when the walk would leave the
     *        blob or overrun a decoded row.
     *
     * The decode is row by row and carries no end-of-input guard of its own: it stops when the row is
     * full, wherever the input cursor has reached. A truncated or mislabelled payload is therefore only
     * safe if the whole walk is proven to fit BEFORE the data is published.
     */
    uint32_t PackedAlphaFootprint(const uint8_t* blob, uint32_t size, uint32_t start)
    {
        uint32_t at = start;
        for (uint32_t row = 0; row < kAlphaDim; ++row)
        {
            uint32_t produced = 0;
            while (produced < kAlphaDim)
            {
                if (at >= size) return 0;
                const uint8_t  control = blob[at++];
                const uint32_t count   = control & 0x7Fu;
                if (count == 0) return 0;                 // a run that emits nothing never ends the row
                if ((control & 0x80u) != 0)               // fill: one repeated value
                {
                    if (at >= size) return 0;
                    ++at;
                }
                else                                      // copy: count literal bytes
                {
                    if (count > size - at) return 0;
                    at += count;
                }
                produced += count;                        // a run may overshoot the row, as the decode allows
            }
        }
        return at - start;
    }

    /**
     * @brief Drops the coverage of any layer whose payload does not provably fit its chunk's own blob.
     *
     * Coverage is read straight out of the resident tile buffer, at a per-layer offset, for an extent
     * implied by the map's alpha layout -- nothing in the data bounds it. So a payload shorter than the
     * layout implies (packed bytes described as unpacked, a truncated tile, a layer whose chunk has no
     * blob at all) is walked past the end of the buffer, and on the tile's last chunk that leaves the
     * allocation entirely. A layer that cannot be proven to fit has its coverage flag cleared, which
     * reads as fully transparent: the chunk draws with the layers underneath it instead, which is
     * always better than the read.
     *
     * @return how many layers of this chunk lost their coverage.
     */
    uint32_t ClampChunkAlpha(ChunkFill& f, bool wideAlpha)
    {
        uint32_t layers  = f.mclySize / kLayerEntrySize;
        uint32_t dropped = 0;
        if (layers > kMaxLayers) layers = kMaxLayers;
        for (uint32_t i = 0; i < layers; ++i)
        {
            uint8_t* entry = f.mcly + i * kLayerEntrySize;
            const uint32_t flags = Rd32(entry + 4);
            if ((flags & kLayerUsesAlpha) == 0) continue;

            const uint32_t start = Rd32(entry + 8);
            bool fits = f.mcal != nullptr && start < f.mcalSize;
            if (fits)
            {
                if (wideAlpha && (flags & kLayerAlphaPacked) != 0)
                    fits = PackedAlphaFootprint(f.mcal, f.mcalSize, start) != 0;
                else
                    fits = (wideAlpha ? kAlphaWideBytes : kAlphaNarrowBytes) <= f.mcalSize - start;
            }
            if (!fits)
            {
                Wr32(entry + 4, flags & ~kLayerUsesAlpha);
                ++dropped;
            }
        }
        return dropped;
    }

    /**
     * @brief In-place fixups of one root MCNK 0x80-byte header so the stock consumers read true counts:
     *        nLayers/nDoodadRefs/nMapObjRefs from the walked split sizes, sizeAlpha/sizeLiquid/
     *        nSndEmitters normalized, has_mcsh matched to a shadow bitmap that is actually whole, and
     *        high-res holes collapsed to the u16 the index build masks live (the u64 overlaps the dead
     *        ofsHeight/ofsNormal fields and is zeroed after parking).
     *
     * Both counts published here bound a read with no bound of its own: the layer count indexes a
     * fixed-size record block, and the shadow flag alone decides whether the bitmap is walked for its
     * full 64x64 bits. Neither may exceed what this chunk really carries.
     */
    void FixChunkHeader(SplitTile& t, ChunkFill& f)
    {
        uint8_t* h = f.rootMcnkHdr + 8;
        uint32_t flags = Rd32(h);
        if (flags & 0x10000u) // high_res_holes
        {
            f.hiResHoles    = Rd64(h + 0x14);
            f.hadHiResHoles = true;
            ++t.hiResHoleChunks;
            Wr16(h + 0x3C, CollapseHoles(f.hiResHoles));
            std::memset(h + 0x14, 0, 8);
            flags &= ~0x10000u;
        }
        const bool shadowWhole = f.mcsh != nullptr && f.mcshSize >= kShadowBytes;
        if (shadowWhole) flags |= 0x1u; else flags &= ~0x1u;       // gates the shadow bitmap walk
        Wr32(h, flags);
        uint32_t layers = f.mclySize / kLayerEntrySize;
        if (layers > kMaxLayers) layers = kMaxLayers;
        Wr32(h + 0x0C, layers);                                    // nLayers
        Wr32(h + 0x10, f.nDoodadRefs);                             // nDoodadRefs
        Wr32(h + 0x28, f.mcalSize + 8u);                           // sizeAlpha (stock: data + hdr)
        Wr32(h + 0x2C, shadowWhole ? f.mcshSize + 8u : 8u);        // sizeShadow (unread; hygiene)
        Wr32(h + 0x38, f.nMapObjRefs);                             // nMapObjRefs
        Wr32(h + 0x5C, f.mcse ? f.mcseSize / 0x1Cu : 0u);          // nSndEmitters
        Wr32(h + 0x64, (f.mclq && f.mclqSize) ? f.mclqSize + 8u : 8u); // sizeLiquid (>8 gates MCLQ)
        if (f.mclv) ++t.mclvChunks;
    }

    // ---------------------------------------------------------------- modern liquid (MH2O)
    // A modern layer header is {u32 offInstances, u32 layerCount, u32 offAttributes} per chunk, and an
    // instance is 0x18 bytes -- byte-identical to what the stock builder reads, EXCEPT that the second
    // word may name a placement object instead of a vertex format, and type ids extend past the stock
    // table. The fixup rewrites both words in place; everything else flows through untouched.
    constexpr uint32_t kMh2oHeaderBytes     = 256 * 12;
    constexpr uint32_t kMh2oInstanceSize    = 0x18;
    constexpr size_t   kMh2oOffExistsBitmap = 0x10;  // instance field: 0 = every cell exists
    constexpr size_t   kMh2oOffVertexData   = 0x14;  // instance field: 0 = flat layer, no height stream
    constexpr uint16_t kFirstPlacementId    = 42;    // below this the word IS the vertex format
    constexpr uint32_t kMaxLiquidLayers     = 16;    // per-chunk sanity cap
    // Bytes per grid entry of the offset_vertex_data stream, indexed by resolved vertex format:
    // 0 height+depth (float+u8), 1 height+uv (float+2xu16), 2 depth-only (u8), 3 height+depth+uv.
    constexpr uint32_t kLvfEntryBytes[4]    = { 5, 8, 1, 9 };
    constexpr uint16_t kBaseWaterType     = 1;     // stock fallback family for non-UV formats
    constexpr uint16_t kBaseMagmaType     = 3;     // stock family for the UV-carrying format

    const LiquidObjectRedirect* FindLiquidRedirect(uint16_t objectId)
    {
        const auto* end = kLiquidObjectRedirect + std::size(kLiquidObjectRedirect);
        const auto* it  = std::lower_bound(kLiquidObjectRedirect, end, uint32_t(objectId),
            [](const LiquidObjectRedirect& e, uint32_t id) { return e.objectId < id; });
        return (it != end && it->objectId == objectId) ? it : nullptr;
    }

    /// Vertex format of a modern liquid type, or 0xFFFF when the type is not in the table.
    uint16_t LiquidLvfForType(uint16_t liquidType)
    {
        const auto* end = kLiquidTypeLvf + std::size(kLiquidTypeLvf);
        const auto* it  = std::lower_bound(kLiquidTypeLvf, end, liquidType,
            [](const LiquidTypeLvf& e, uint16_t id) { return e.liquidType < id; });
        return (it != end && it->liquidType == liquidType) ? it->lvf : 0xFFFF;
    }

    /// True when the live LiquidType table can serve the id with a material the stock
    /// Liquid::CMaterialBank::GetMaterial can actually resolve (rows may have gaps: NULL row =
    /// unknown; a resolvable row can still carry a MaterialID outside GetMaterial's switch -- see
    /// kLiquidTypeMaterialId in ADT.hpp for why that is real, not defensive over-caution).
    bool LiquidTypeInDbc(uint32_t id)
    {
        const uint32_t minId = Rd32(reinterpret_cast<void*>(adt::kLiquidTypeDbMinId));
        const uint32_t maxId = Rd32(reinterpret_cast<void*>(adt::kLiquidTypeDbMaxId));
        if (id < minId || id > maxId) return false;
        const uint8_t* rows = reinterpret_cast<uint8_t*>(Rd32(reinterpret_cast<void*>(adt::kLiquidTypeDbRows)));
        if (!rows) return false;
        const uint32_t row = Rd32(rows + (id - minId) * 4u);
        if (!row) return false;
        const uint32_t materialId = Rd32(reinterpret_cast<void*>(row + adt::kLiquidTypeMaterialId));
        return materialId >= 1 && materialId <= 3;
    }

    /**
     * @brief Normalizes a modern MH2O block in place so the stock liquid builder reads it natively:
     *        placement-object ids become (liquid type, vertex format) via the embedded redirect table,
     *        and any type id the live LiquidType table cannot serve degrades to the base family of the
     *        same vertex format -- every downstream consumer then finds a row. Nulls mh2o (tile loses
     *        water, never faults) only when the block is too small to carry its header array.
     */
    void FixupMh2o(SplitTile& t, uint8_t*& mh2o)
    {
        if (!mh2o) return;
        const uint32_t sz = Rd32(mh2o + 4);
        if (sz < kMh2oHeaderBytes) { mh2o = nullptr; return; }
        uint8_t* data = mh2o + 8;
        uint32_t layers = 0, degraded = 0, oob = 0, guessed = 0;
        for (uint32_t c = 0; c < 256; ++c)
        {
            const uint32_t offInst = Rd32(data + c * 12);
            uint32_t count = Rd32(data + c * 12 + 4);
            if (!offInst || !count) continue;
            if (count > kMaxLiquidLayers) count = kMaxLiquidLayers;
            if (offInst > sz || count * kMh2oInstanceSize > sz - offInst) continue;
            for (uint32_t i = 0; i < count; ++i)
            {
                uint8_t* inst = data + offInst + i * kMh2oInstanceSize;
                uint16_t type = Rd16(inst);
                uint16_t lvf  = Rd16(inst + 2);
                ++layers;
                if (lvf >= kFirstPlacementId)
                {
                    if (lvf == kFirstPlacementId)
                    {
                        // Object id 42 is the client's hardcoded ocean sentinel (RegisterLiquidObject:
                        // LO == 42 always resolves to the ocean liquid object, independent of any
                        // LiquidObject.dbc row) -- always the classic flat-ocean convention, never a
                        // table lookup or a guess.
                        lvf = 2;
                    }
                    else if (const auto* r = FindLiquidRedirect(lvf)) { type = r->liquidType; lvf = r->lvf; }
                    else
                    {
                        // Unknown placement object: keep the instance's own type; without a format
                        // source, a data-less layer is the flat depth-only format, a data-carrying
                        // one the height+depth default.
                        const uint16_t byType = LiquidLvfForType(type);
                        if (byType != 0xFFFF)
                            lvf = byType;
                        else
                        {
                            lvf = Rd32(inst + kMh2oOffVertexData) ? 0 : 2;
                            ++guessed;
                        }
                    }
                    // xOffset/yOffset/width/height are left exactly as authored. The stock builder
                    // reads them unconditionally regardless of what the second word originally named,
                    // and the vertex-data stream this instance points at was sized to match them, not
                    // to a full 8x8 chunk -- rewriting them to a fixed grid desyncs the stream length
                    // from what was actually written and walks the read into the next instance's data.
                }
                // A layer with no vertex stream is the flat, fully-deep format: only the depth-only
                // format's no-data path answers max depth -- every other format would read past the
                // header (the classic flat-ocean convention).
                if (Rd32(inst + kMh2oOffVertexData) == 0)
                    lvf = 2;

                // Neither array-count above nor these two offsets are bounds-checked by the client --
                // it walks straight off inst+0x10/+0x14 with nothing to stop it reading past this
                // MH2O block. A stream that does not actually fit (truncated file, or a grid sized for
                // a different vertex format than the one this instance resolved to) then reads
                // adjacent heap bytes as height floats: exactly an isolated vertical spike in an
                // otherwise flat sheet. Degrade such an instance to the no-data convention instead of
                // letting the stock builder read past the block.
                const uint32_t vd = Rd32(inst + kMh2oOffVertexData);
                if (vd != 0)
                {
                    const uint32_t entries       = (uint32_t(inst[0xE]) + 1u) * (uint32_t(inst[0xF]) + 1u);
                    const uint32_t bytesPerEntry = (lvf < 4) ? kLvfEntryBytes[lvf] : 0;
                    const uint32_t need          = entries * bytesPerEntry;
                    if (bytesPerEntry == 0 || vd > sz || need > sz - vd)
                    {
                        Wr32(inst + kMh2oOffVertexData, 0);
                        lvf = 2;
                        ++oob;
                    }
                }
                const uint32_t eb = Rd32(inst + kMh2oOffExistsBitmap);
                if (eb != 0)
                {
                    const uint32_t bitmapBytes = (uint32_t(inst[0xE]) * inst[0xF] + 7u) / 8u;
                    if (eb > sz || bitmapBytes > sz - eb)
                    {
                        Wr32(inst + kMh2oOffExistsBitmap, 0); // 0 = every cell exists, the safe default
                        ++oob;
                    }
                }

                if (!LiquidTypeInDbc(type))
                {
                    type = (lvf == 1) ? kBaseMagmaType : kBaseWaterType;
                    ++degraded;
                }
                Wr16(inst, type);
                Wr16(inst + 2, lvf);
            }
        }
        g_statLiquidLayers.fetch_add(layers, std::memory_order_relaxed);
        if (degraded)
        {
            g_statLiquidDegraded.fetch_add(degraded, std::memory_order_relaxed);
            WLOG_WARN("adt-split: tile %d_%d degraded %u of %u liquid layer(s) to a base type "
                      "(id missing from the loaded LiquidType table -- extend the DBC)",
                      t.tileFirst, t.tileSecond, degraded, layers);
        }
        if (oob)
        {
            g_statLiquidOob.fetch_add(oob, std::memory_order_relaxed);
            WLOG_WARN("adt-split: tile %d_%d found %u of %u liquid layer(s) with an out-of-bounds "
                      "vertex-data/exists-bitmap offset, degraded to the no-data convention",
                      t.tileFirst, t.tileSecond, oob, layers);
        }
        if (guessed)
            WLOG_WARN("adt-split: tile %d_%d guessed a vertex format for %u of %u liquid layer(s) "
                      "(placement object and type both missing from the loaded tables)",
                      t.tileFirst, t.tileSecond, guessed, layers);
    }

    /**
     * @brief The whole per-tile parse: indexes the three resident buffers, builds the MCRF concat pool,
     *        fixes the 256 root headers in place, synthesizes MCIN (+ empty top-level chunks for absent
     *        tables) and patches the root MHDR offsets so the UNCHANGED stock tile-area create routine
     *        wires every area field itself.
     * @return false only on catastrophic input (no MVER/MHDR, zero MCNKs).
     */
    bool ParseAndPatch(SplitTile& t)
    {
        uint8_t* rb = t.buffers.rootBuf;
        const uint32_t rs = t.buffers.rootSize;
        if (!rb || rs < 0x60) return false;
        if (Rd32(rb) != FourCC("MVER")) return false;
        const uint32_t mverSize = Rd32(rb + 4);
        if (mverSize > rs || 8 + mverSize + 8 > rs) return false;
        uint8_t* mhdrHdr = rb + 8 + mverSize;
        if (Rd32(mhdrHdr) != FourCC("MHDR")) return false;
        const uint32_t mhdrSize = Rd32(mhdrHdr + 4);
        if (mhdrSize < 0x34 || (mhdrHdr - rb) + 8ull + mhdrSize > rs) return false;
        uint8_t* mhdr = mhdrHdr + 8;

        // --- index the root ---
        uint8_t* mh2o = nullptr; uint8_t* mfbo = nullptr;
        uint32_t nRoot = 0;
        WalkTop(rb, rs, [&](uint32_t tag, uint8_t* hdr, uint32_t sz) {
            if (tag == FourCC("MCNK")) { if (nRoot < 256) WalkRootMcnk(t.chunks[nRoot], hdr, sz); ++nRoot; }
            else if (tag == FourCC("MH2O")) mh2o = hdr;
            else if (tag == FourCC("MFBO")) mfbo = hdr;
        });
        if (nRoot == 0) return false;
        t.chunkCount = nRoot < 256 ? nRoot : 256;
        if (nRoot != 256)
            WLOG_WARN("adt-split: tile %d_%d root has %u MCNKs (expected 256)", t.tileFirst, t.tileSecond, nRoot);

        // --- index _tex0 ---
        uint8_t* mtex = nullptr; uint8_t* mtxf = nullptr;
        if (t.buffers.texBuf)
        {
            uint32_t nTex = 0;
            WalkTop(t.buffers.texBuf, t.buffers.texSize, [&](uint32_t tag, uint8_t* hdr, uint32_t sz) {
                if (tag == FourCC("MCNK")) { if (nTex < 256) WalkTexMcnk(t.chunks[nTex], hdr + 8, sz); ++nTex; }
                else if (tag == FourCC("MTEX")) { mtex = hdr; t.height.mtexData = hdr + 8; t.height.mtexSizeParked = sz; }
                else if (tag == FourCC("MTXF")) { mtxf = hdr; t.height.mtxfData = hdr + 8; t.height.mtxfSize = sz; }
                else if (tag == FourCC("MDID"))                                               // texture FileDataIDs
                {
                    const uint32_t n = sz / 4u;
                    t.texCache.mdid.resize(n);
                    for (uint32_t i = 0; i < n; ++i) t.texCache.mdid[i] = Rd32(hdr + 8 + i * 4);
                }
                else if (tag == FourCC("MHID"))                                               // height FileDataIDs
                {
                    const uint32_t n = sz / 4u;
                    t.height.mhid.resize(n);
                    for (uint32_t i = 0; i < n; ++i) t.height.mhid[i] = Rd32(hdr + 8 + i * 4);
                }
                else if (tag == FourCC("MTXP")) { t.height.mtxp = hdr + 8; t.height.mtxpSize = sz; }        // height params
                else if (tag == FourCC("MAMP")) { if (sz) t.mampValue = hdr[8]; }             // parked
            });
        }

        // --- index _obj0 ---
        uint8_t* mmdx = nullptr; uint8_t* mmid = nullptr; uint8_t* mwmo = nullptr;
        uint8_t* mwid = nullptr; uint8_t* mddf = nullptr; uint8_t* modf = nullptr;
        if (t.buffers.objBuf)
        {
            uint32_t nObj = 0;
            WalkTop(t.buffers.objBuf, t.buffers.objSize, [&](uint32_t tag, uint8_t* hdr, uint32_t sz) {
                if (tag == FourCC("MCNK")) { if (nObj < 256) WalkObjMcnk(t.chunks[nObj], hdr + 8, sz); ++nObj; }
                else if (tag == FourCC("MMDX")) mmdx = hdr;
                else if (tag == FourCC("MMID")) mmid = hdr;
                else if (tag == FourCC("MWMO")) mwmo = hdr;
                else if (tag == FourCC("MWID")) mwid = hdr;
                else if (tag == FourCC("MDDF")) mddf = hdr;
                else if (tag == FourCC("MODF")) modf = hdr;
            });
        }

        // --- doodad FileDataID resolution. A modern _obj0 places doodads by FileDataID: the MDDF entry
        // flag 0x40 means nameId IS the model's FileDataID and there is no MMDX/MMID name table. Resolve
        // each via ModelFilePath into a synthesized MMDX (paths) + MMID (offsets), rewrite every such
        // entry's nameId to its MMID index and clear the 0x40 flag -- so the stock placement path hands
        // the native M2 reader a valid name. Entry COUNT/order is preserved (per-chunk MCRD refs index
        // it); only nameId/flags change. Unresolved ids share one empty-name slot (stock -> ErrorCube).
        if (mddf)
        {
            const uint32_t nDood = Rd32(mddf + 4) / 0x24u;
            std::vector<uint8_t>  paths;   // MMDX payload
            std::vector<uint32_t> offs;    // MMID payload (byte offset into paths, one per name slot)
            std::unordered_map<uint32_t, uint32_t> fidToIdx;
            uint32_t unresolvedIdx = 0xFFFFFFFFu, resolved = 0;
            auto intern = [&](const char* p) -> uint32_t {
                const uint32_t idx = static_cast<uint32_t>(offs.size());
                offs.push_back(static_cast<uint32_t>(paths.size()));
                const size_t len = p ? std::strlen(p) : 0;
                paths.insert(paths.end(), p, p + len);
                paths.push_back('\0');
                return idx;
            };
            for (uint32_t i = 0; i < nDood; ++i)
            {
                uint8_t*  e = mddf + 8 + i * 0x24u;
                uint16_t  flags; std::memcpy(&flags, e + 0x22, 2);
                if ((flags & 0x40u) == 0) continue;                 // already a name-table index
                const uint32_t fid = Rd32(e);
                uint32_t idx;
                auto it = fidToIdx.find(fid);
                if (it != fidToIdx.end()) idx = it->second;
                else
                {
                    const char* path = wxl_modern_adt::ResolveModel(fid);
                    if (path && path[0]) { idx = intern(path); ++resolved; }
                    else { if (unresolvedIdx == 0xFFFFFFFFu) unresolvedIdx = intern(""); idx = unresolvedIdx; }
                    fidToIdx.emplace(fid, idx);
                }
                Wr32(e, idx);                                       // nameId -> MMID index
                flags &= ~0x40u; std::memcpy(e + 0x22, &flags, 2);  // clear the filedata-id flag
            }
            if (!offs.empty())
            {
                t.synth.mmdxChunk.assign(8, 0);
                Wr32(t.synth.mmdxChunk.data(), FourCC("MMDX"));
                Wr32(t.synth.mmdxChunk.data() + 4, static_cast<uint32_t>(paths.size()));
                t.synth.mmdxChunk.insert(t.synth.mmdxChunk.end(), paths.begin(), paths.end());
                t.synth.mmidChunk.assign(8, 0);
                Wr32(t.synth.mmidChunk.data(), FourCC("MMID"));
                Wr32(t.synth.mmidChunk.data() + 4, static_cast<uint32_t>(offs.size() * 4u));
                for (uint32_t o : offs) { uint8_t b[4]; Wr32(b, o); t.synth.mmidChunk.insert(t.synth.mmidChunk.end(), b, b + 4); }
                mmdx = t.synth.mmdxChunk.data();
                mmid = t.synth.mmidChunk.data();
            }
            g_statDoodadModels.fetch_add(resolved, std::memory_order_relaxed);
        }

        // --- map-object FileDataID resolution. Same shape as the doodads above, with one hard
        // difference: a doodad whose name does not resolve degrades to the ErrorCube, a MAP OBJECT kills
        // the client. The chunk's ref-spawn path hands the ref straight to the map's create-map-object-def
        // routine, which hashes the path immediately -- an absent name faults in the client's string-hash
        // lookup before any "file not found" path can run. So a resolved entry is rewritten to an MWID index exactly
        // like a doodad, and an UNRESOLVED entry is left untouched in MODF but struck from every MCRW
        // ref list, which is the only thing that would have spawned it.
        if (modf)
        {
            const uint32_t nWmo = Rd32(modf + 4) / 0x40u;
            std::vector<uint8_t>  paths;   // MWMO payload
            std::vector<uint32_t> offs;    // MWID payload (byte offset into paths, one per name slot)
            std::unordered_map<uint32_t, uint32_t> fidToIdx;
            uint32_t resolved = 0, dropped = 0;
            t.synth.modfDead.assign(nWmo, 0);
            auto intern = [&](const char* p) -> uint32_t {
                const uint32_t idx = static_cast<uint32_t>(offs.size());
                offs.push_back(static_cast<uint32_t>(paths.size()));
                const size_t len = p ? std::strlen(p) : 0;
                paths.insert(paths.end(), p, p + len);
                paths.push_back('\0');
                return idx;
            };
            for (uint32_t i = 0; i < nWmo; ++i)
            {
                uint8_t*  e = modf + 8 + i * 0x40u;
                uint16_t  flags; std::memcpy(&flags, e + 0x38, 2);
                if ((flags & 0x8u) == 0) continue;                  // already a name-table index
                const uint32_t fid = Rd32(e);
                auto it = fidToIdx.find(fid);
                if (it != fidToIdx.end())
                {
                    if (it->second == 0xFFFFFFFFu) { t.synth.modfDead[i] = 1; ++dropped; continue; }
                    Wr32(e, it->second);
                    flags &= ~0x8u; std::memcpy(e + 0x38, &flags, 2);
                    continue;
                }
                const char* path = wxl_modern_adt::ResolveModel(fid);
                if (path && path[0])
                {
                    const uint32_t idx = intern(path);
                    fidToIdx.emplace(fid, idx);
                    Wr32(e, idx);
                    flags &= ~0x8u; std::memcpy(e + 0x38, &flags, 2);
                    ++resolved;
                }
                else
                {
                    fidToIdx.emplace(fid, 0xFFFFFFFFu);
                    t.synth.modfDead[i] = 1;
                    ++dropped;
                }
            }
            if (!offs.empty())
            {
                t.synth.mwmoChunk.assign(8, 0);
                Wr32(t.synth.mwmoChunk.data(), FourCC("MWMO"));
                Wr32(t.synth.mwmoChunk.data() + 4, static_cast<uint32_t>(paths.size()));
                t.synth.mwmoChunk.insert(t.synth.mwmoChunk.end(), paths.begin(), paths.end());
                t.synth.mwidChunk.assign(8, 0);
                Wr32(t.synth.mwidChunk.data(), FourCC("MWID"));
                Wr32(t.synth.mwidChunk.data() + 4, static_cast<uint32_t>(offs.size() * 4u));
                for (uint32_t o : offs) { uint8_t b[4]; Wr32(b, o); t.synth.mwidChunk.insert(t.synth.mwidChunk.end(), b, b + 4); }
                mwmo = t.synth.mwmoChunk.data();
                mwid = t.synth.mwidChunk.data();
            }
            g_statMapObjects.fetch_add(resolved, std::memory_order_relaxed);
            g_statMapObjectsDropped.fetch_add(dropped, std::memory_order_relaxed);
            if (dropped)
                WLOG_WARN("adt-split: tile %d_%d dropped %u unresolved map object(s) of %u "
                          "(no ModelFilePath entry; refs struck from MCRW)",
                          t.tileFirst, t.tileSecond, dropped, nWmo);
        }

        // Modern MH2O flows through after in-place normalization (placement-object resolution + type
        // degradation for ids the loaded LiquidType table lacks); the remaining unguarded stock reader
        // is already neutralized by the liquid-row flag-test patch installed with the world feature.
        if constexpr (wxl_modern_adt::kEnabled)
            FixupMh2o(t, mh2o);

        // --- strike unresolved map objects from the per-chunk MCRW ref lists. Runs before the MCRF pool
        // so the concat below already sees the final sizes. The payload is rebuilt into a tile-owned
        // buffer sized to the worst case UP FRONT, so the per-chunk pointers taken here can never be
        // invalidated by a regrow. MODF itself is left intact -- entry indices stay valid.
        if (!t.synth.modfDead.empty())
        {
            uint32_t deadCount = 0;
            for (uint8_t d : t.synth.modfDead) deadCount += d;
            if (deadCount)
            {
                uint32_t total = 0;
                for (uint32_t i = 0; i < t.chunkCount; ++i) total += t.chunks[i].mcrwSize;
                t.synth.mcrwFiltered.resize(total);
                const uint32_t nWmo = static_cast<uint32_t>(t.synth.modfDead.size());
                uint32_t poolAt = 0;
                for (uint32_t i = 0; i < t.chunkCount; ++i)
                {
                    ChunkFill& f = t.chunks[i];
                    if (!f.mcrw || !f.mcrwSize) continue;
                    uint8_t* dst = t.synth.mcrwFiltered.data() + poolAt;
                    uint32_t kept = 0;
                    for (uint32_t r = 0; r < f.mcrwSize / 4u; ++r)
                    {
                        const uint32_t idx = Rd32(f.mcrw + r * 4u);
                        if (idx >= nWmo || t.synth.modfDead[idx]) continue;
                        Wr32(dst + kept * 4u, idx);
                        ++kept;
                    }
                    f.mcrw     = dst;
                    f.mcrwSize = kept * 4u;
                    poolAt    += kept * 4u;
                }
            }
        }

        // --- MCRF fixup pool: refs must be ONE contiguous u32 array, doodads first then wmos. When only
        // one side exists the split payload is aliased directly (zero copy). Both lists stay index-
        // aligned with their own table: MDDF keeps its entry count (only nameId/flags were rewritten),
        // and MODF keeps every entry too -- an unresolved map object was removed from the ref lists just
        // above, never from the table, so no index shifts.
        uint32_t poolBytes = 0;
        for (uint32_t i = 0; i < t.chunkCount; ++i)
        {
            ChunkFill& f = t.chunks[i];
            if (f.mcrdSize && f.mcrwSize) poolBytes += f.mcrdSize + f.mcrwSize;
        }
        t.synth.mcrfPool.resize(poolBytes);
        uint32_t poolOff = 0;
        for (uint32_t i = 0; i < t.chunkCount; ++i)
        {
            ChunkFill& f = t.chunks[i];
            const uint32_t dr = f.mcrdSize;
            const uint32_t wr = f.mcrwSize;
            f.nDoodadRefs = dr / 4u;
            f.nMapObjRefs = wr / 4u;
            if (dr && wr)
            {
                uint8_t* dst = t.synth.mcrfPool.data() + poolOff;
                std::memcpy(dst, f.mcrd, dr);
                std::memcpy(dst + dr, f.mcrw, wr);
                f.mcrf = dst;
                poolOff += dr + wr;
            }
            else if (dr) f.mcrf = f.mcrd;
            else if (wr) f.mcrf = f.mcrw;
            else         f.mcrf = nullptr;
        }
        g_statMcrfBytes.fetch_add(poolBytes, std::memory_order_relaxed);

        // --- per-chunk root header fixups (counts, holes, has_mcsh) + the coverage bound. The layout
        // is read live: the tile-load path has already made it agree with what a split map ships, and
        // the same value decides how much of each blob the terrain build will walk.
        const bool wideAlpha =
            (Rd32(reinterpret_cast<const void*>(adt::kMphdFlags)) & kMapWideAlpha) != 0;
        uint32_t alphaDropped = 0;
        for (uint32_t i = 0; i < t.chunkCount; ++i)
        {
            alphaDropped += ClampChunkAlpha(t.chunks[i], wideAlpha);
            FixChunkHeader(t, t.chunks[i]);
        }
        if (alphaDropped)
            WLOG_WARN("adt-split: tile %d_%d dropped %u layer coverage map(s) that do not fit their "
                      "chunk's blob (%s layout); those layers draw transparent",
                      t.tileFirst, t.tileSecond, alphaDropped, wideAlpha ? "wide" : "narrow");

        // --- synthesize MCIN (absolute offsets into the root buffer, as stock PrepareChunk adds them to
        // area+0x80). Missing slots on a short root duplicate the last chunk so a stray PrepareChunk
        // never dereferences garbage.
        t.synth.mcin.assign(8 + 256 * 16, 0);
        Wr32(t.synth.mcin.data(), FourCC("MCIN"));
        Wr32(t.synth.mcin.data() + 4, 256 * 16);
        for (uint32_t i = 0; i < 256; ++i)
        {
            const ChunkFill& f = t.chunks[i < t.chunkCount ? i : t.chunkCount - 1];
            uint8_t* e = t.synth.mcin.data() + 8 + i * 16;
            Wr32(e + 0, static_cast<uint32_t>(f.rootMcnkHdr - rb)); // absolute file offset
            Wr32(e + 4, Rd32(f.rootMcnkHdr + 4) + 8);               // size (unread; hygiene)
            // flags(+8) = 0 (not built), asyncId(+12) = 0
        }

        // --- synthesize empty top-level chunks for whatever the split trio did not provide;
        // the tile-area create routine dereferences these eight unconditionally.
        t.synth.synthEmpty.reserve(8 * 8);
        auto synth = [&](uint32_t tag) -> uint8_t* {
            const size_t at = t.synth.synthEmpty.size();
            t.synth.synthEmpty.resize(at + 8, 0);
            Wr32(t.synth.synthEmpty.data() + at, tag);
            return t.synth.synthEmpty.data() + at;
        };
        if (!mtex) mtex = synth(FourCC("MTEX"));
        if (!mmdx) mmdx = synth(FourCC("MMDX"));
        if (!mmid) mmid = synth(FourCC("MMID"));
        if (!mwmo) mwmo = synth(FourCC("MWMO"));
        if (!mwid) mwid = synth(FourCC("MWID"));
        if (!mddf) mddf = synth(FourCC("MDDF"));
        if (!modf) modf = synth(FourCC("MODF"));

        // --- patch the root MHDR so the stock parser finds everything. Offsets are stored as
        // (chunkHeader - mhdr); 32-bit wraparound makes cross-buffer deltas exact.
        auto delta = [&](const uint8_t* hdr) -> uint32_t {
            return hdr ? static_cast<uint32_t>(hdr - mhdr) : 0u;
        };
        Wr32(mhdr + 0x04, delta(t.synth.mcin.data()));
        Wr32(mhdr + 0x08, delta(mtex));
        Wr32(mhdr + 0x0C, delta(mmdx));
        Wr32(mhdr + 0x10, delta(mmid));
        Wr32(mhdr + 0x14, delta(mwmo));
        Wr32(mhdr + 0x18, delta(mwid));
        Wr32(mhdr + 0x1C, delta(mddf));
        Wr32(mhdr + 0x20, delta(modf));
        Wr32(mhdr + 0x24, delta(mfbo));
        Wr32(mhdr + 0x28, delta(mh2o)); // gated by non-zero offset + chunk size, both true iff found
        Wr32(mhdr + 0x2C, delta(mtxf));
        uint32_t mhdrFlags = Rd32(mhdr);
        mhdrFlags = mfbo ? (mhdrFlags | 0x1u) : (mhdrFlags & ~0x1u);
        Wr32(mhdr, mhdrFlags);
        // The client derives the alpha texture dim as 0x40 >> byte(MHDR+0x30); split content parks its
        // MAMP amplitude there. Park the value and zero the byte so the dim stays the stock 64.
        if (mhdrSize > 0x30)
        {
            if (mhdr[0x30]) t.mampValue = mhdr[0x30];
            mhdr[0x30] = 0;
        }
        return true;
    }
}

namespace wxl::runtime::adtsplit::detail
{
    /// SEH shell around the parser: a fault on malformed data becomes a logged failure, never a crash.
    bool ParseAndPatchGuarded(SplitTile* t)
    {
        return SehGuarded([t] { return ParseAndPatch(*t); }, false);
    }
}
