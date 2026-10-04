// Chunk build detour: publish OnAdtChunkBuild with the chunk and its texture-layer count.
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

#include "../ExtensionApi.hpp"

#include "engine/events/Event.hpp"
#include "offsets/game/ADT.hpp"

#include <cstdint>

namespace
{
    namespace ev  = wxl::events;
    namespace adt = wxl::offsets::game::adt;

    adt::Map_ChunkBuildFn g_origChunkBuild = nullptr;

    /**
     * @brief Detours map-chunk build, emitting OnAdtChunkBuild with the chunk and its layer count.
     *
     * Runs after the native build populates the sub-chunk pointers and layer units, so the
     * texture-layer count is readable.
     * @param chunk    map chunk.
     * @param edx      unused register slot for the thiscall convention.
     * @param rawMcnk  raw chunk data.
     * @param param2   native build parameter.
     */
    void __fastcall hkChunkBuild(void* chunk, void* edx, void* rawMcnk, int param2)
    {
        g_origChunkBuild(chunk, edx, rawMcnk, param2);
        uint32_t layers = 0;
        auto* header = reinterpret_cast<adt::McnkHeader*>(static_cast<adt::MapChunk*>(chunk)->mcnkHeader);
        if (header) layers = header->nLayers;
        ev::AdtChunkArgs a{ chunk, layers };
        wxl_modern_adt::g_api->Emit(uint32_t(ev::Event::OnAdtChunkBuild), &a);
    }
}

namespace wxl_modern_adt
{
    bool InstallChunkBuild()
    {
        HookAttachByName("Adt.ChunkBuild", &hkChunkBuild, &g_origChunkBuild);
        return true;
    }
}
