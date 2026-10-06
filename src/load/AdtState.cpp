// Native split-ADT reader: the shared per-tile registry and telemetry counters.
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

namespace wxl::runtime::adtsplit::detail
{
    std::mutex g_mutex;
    std::unordered_map<void*, std::unique_ptr<SplitTile>> g_tiles;
    std::unordered_map<std::string, bool> g_splitMaps;

    std::atomic<uint32_t> g_statSplitMaps{ 0 }, g_statTilesLoaded{ 0 }, g_statTilesResident{ 0 },
        g_statChunksFilled{ 0 }, g_statMcrfBytes{ 0 }, g_statMtxpTiles{ 0 },
        g_statMclvChunks{ 0 }, g_statHoleChunks{ 0 }, g_statFailures{ 0 },
        g_statWdlRead{ 0 }, g_statHeightTex{ 0 }, g_statDoodadModels{ 0 },
        g_statMapObjects{ 0 }, g_statMapObjectsDropped{ 0 },
        g_statLiquidLayers{ 0 }, g_statLiquidDegraded{ 0 }, g_statLiquidOob{ 0 };

    SplitTile* FindTileLocked(void* area)
    {
        auto it = g_tiles.find(area);
        return it != g_tiles.end() ? it->second.get() : nullptr;
    }

    // Load-thread lookup that takes g_mutex only for the map access, then releases it: the texture
    // detours call the stock originals while NOT holding the lock (the eager path re-enters
    // the terrain per-slot texture loader, which would deadlock a non-recursive mutex). Safe because a
    // tile is only ever erased on this same load thread (the tile-area destructor), so the pointer cannot dangle.
    SplitTile* FindTileBrief(void* area)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return FindTileLocked(area);
    }
}
