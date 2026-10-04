// Native split-ADT reader (split root/_tex0/_obj0 tiles): public query surface for stats and status.
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

#include <cstdint>

/// Read-only query surface over the split-ADT reader (features/adtsplit): per-map split detection
/// state, session counters, and per-resident-tile status. Everything is snapshot-by-value so the
/// Lua methods (wxl.adt.*) never hold pointers into the feature's mutable state.
namespace wxl::runtime::adtsplit
{
    /** @brief Session counters for the split-ADT reader (all monotonic except tilesResident). */
    struct Stats
    {
        uint32_t splitMapsDetected; ///< maps whose _tex0 probe answered "split"
        uint32_t tilesLoaded;       ///< split tiles that completed their 3-file load
        uint32_t tilesResident;     ///< split tiles currently alive
        uint32_t chunksFilled;      ///< chunk objects direct-filled from split buffers
        uint32_t mcrfBytes;         ///< bytes materialized for the MCRD‖MCRW -> MCRF concat fixups
        uint32_t parkedMtxpTiles;   ///< tiles whose MTXP (height texturing params) is parked
        uint32_t parkedMclvChunks;  ///< chunks whose MCLV (baked light) is parked
        uint32_t parkedHoleChunks;  ///< chunks whose original high-res hole mask is parked
        uint32_t loadFailures;      ///< split loads that fell back / failed to parse
        uint32_t wdlRead;           ///< split WDLs read directly into the stock low-detail runtime
        uint32_t heightTexLoaded;   ///< "_h" height texture handles created for height-blend tiles
        uint32_t doodadModels;      ///< MDDF FileDataIDs resolved into the synthesized MMDX/MMID table
        uint32_t mapObjects;        ///< MODF FileDataIDs resolved into the synthesized MWMO/MWID table
        /// MODF entries whose FileDataID has no ModelFilePath row. Their refs are struck from MCRW
        /// rather than spawned: unlike a doodad (which degrades to the ErrorCube), a map object with an
        /// unresolvable name faults in the client's string-hash path the moment the map-object spawn hashes it.
        uint32_t mapObjectsDropped;
        uint32_t liquidLayers;   ///< MH2O layers normalized for the stock liquid builder
        uint32_t liquidDegraded; ///< layers whose type id the live LiquidType table could not serve
        /// layers whose offset_exists_bitmap/offset_vertex_data pointed outside the MH2O block (or
        /// undersized for their resolved format+grid); degraded to the no-data convention rather than
        /// let the stock builder read past the block.
        uint32_t liquidOob;
    };

    /** @brief Returns a snapshot of the session counters. */
    Stats GetStats();

    /**
     * @brief Reports the cached split-detection state of the CURRENT map.
     * @return 1 when the map is split (root/_tex0/_obj0 trio), 0 when monolithic, -1 when not yet probed
     *         (no tile of this map has been requested) or no map is loaded.
     */
    int IsSplitMap();

    /** @brief Status of one resident split tile. */
    struct TileStatus
    {
        int      tileFirst;        ///< first  %d of "<Map>_%d_%d.adt"
        int      tileSecond;       ///< second %d of "<Map>_%d_%d.adt"
        uint32_t rootSize;         ///< resident root buffer bytes
        uint32_t texSize;          ///< resident _tex0 buffer bytes (0 = missing)
        uint32_t objSize;          ///< resident _obj0 buffer bytes (0 = missing)
        uint32_t chunkCount;       ///< MCNKs indexed from the root (256 on a well-formed tile)
        bool     complete;         ///< the 3-file load + stock parse finished
        bool     hasMtxp;          ///< a parked MTXP block exists for this tile
        uint32_t mclvChunks;       ///< chunks with parked MCLV
        uint32_t hiResHoleChunks;  ///< chunks with parked high-res holes
    };

    /** @brief Returns the number of resident split tiles. */
    uint32_t ResidentTileCount();

    /**
     * @brief Fetches a resident split tile's status by enumeration index (0..ResidentTileCount()-1).
     *        Enumeration order is unspecified and may change as tiles stream in/out.
     * @return true when the index was valid and out was filled.
     */
    bool GetResidentTile(uint32_t index, TileStatus& out);

    /**
     * @brief Finds a resident split tile by its filename indices.
     * @return true when the tile is resident and out was filled.
     */
    bool FindTile(int tileFirst, int tileSecond, TileStatus& out);

    // --- height-blend data surface (consumed by features/adtsplit/HeightBlend.cpp, draw thread) ---

    namespace detail { struct SplitTile; } // opaque here; see ResolveHeightTile

    /** @brief One terrain layer's height-blend inputs, resolved from MTXP + MHID / "_h.blp". */
    struct HeightLayer
    {
        void*    texture;      ///< GPU texture handle for "_h"; null = use the neutral solid-white texture
        float    heightScale;  ///< MTXP heightScale (0.0 when the record is default/absent)
        float    heightOffset; ///< MTXP heightOffset (1.0 when the record is default/absent)
        uint32_t tilingExp;    ///< MTXP/MTXF flags bits 4..7: layer UV scale divides by (1 << exp)
    };

    /**
     * @brief Resolves `area`'s split-tile height record once, for reuse across one chunk draw's
     *        per-layer GetHeightLayer calls -- the single-shot overload below would otherwise redo
     *        the tile-registry lock and hash lookup for the very same tile once per layer (up to 4x
     *        per chunk, every frame the chunk is drawn). Also builds the tile's per-texture height
     *        slot table on first use per tile (MTXP record, or fallback {MTXF flags, 0, 1}; a
     *        non-default record without flag 0x1 resolves its "_h" texture via MHID FileDataID or
     *        the "<diffuse>_h.blp" name). Must be called on the main/draw thread.
     *
     * @return an opaque handle for the GetHeightLayer(handle, ...) overload, or null when `area` is
     *         not a completed split tile carrying MTXP (caller must run the untouched stock draw).
     */
    detail::SplitTile* ResolveHeightTile(void* area);

    /**
     * @brief Resolves one layer's height-blend inputs from a handle already returned by
     *        ResolveHeightTile. No locking, no lookup -- safe to call once per layer of the chunk the
     *        handle was resolved for.
     * @param textureId  the layer record's MCLY.textureId (index into the tile's texture set).
     */
    void GetHeightLayer(detail::SplitTile* tile, uint32_t textureId, HeightLayer& out);

    /**
     * @brief Resolves the height-blend inputs of one layer of a resident split tile.
     *
     * Convenience one-shot form of ResolveHeightTile + GetHeightLayer, for a caller that only needs a
     * single layer. area is the tile-area object the drawn chunk belongs to.
     *
     * @return false when the tile is not a completed split tile or carries no MTXP (caller must run
     *         the untouched stock draw); true with out filled otherwise (texture may be null=white).
     */
    bool GetHeightLayer(void* area, uint32_t textureId, HeightLayer& out);

    /** @brief Lock-free resident split-tile count (relaxed) for per-draw fast-path gating. */
    uint32_t ResidentTilesRelaxed();
}
