// Height-based terrain layer blending: runtime settings + status surface.
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

/// Tuning and status of the terrain height blend. The settings are plain fields mutated on the game
/// thread and read by the draw detour per chunk; no other memory is touched through here.
namespace wxl::features::heightblend
{
    /** @brief Live tunables, seeded from the runtime config at install. */
    struct Settings
    {
        bool  enabled   = true; ///< master switch (WXL_ADT_HEIGHT_BLEND)
        float sharpness = 2.0f; ///< sharpen strength, published per layer that carries height data;
                                ///< 0 = plain scale/offset blend (WXL_ADT_HEIGHT_SHARPNESS)
    };

    /** @brief Returns the mutable live settings (game thread). */
    Settings& Get();

    /** @brief Session counters + activity flag. */
    struct Stats
    {
        uint32_t chunksDrawn; ///< chunk draws that published height data
        bool     active;      ///< the fast-path gate is currently open (map + tiles + toggle)
    };

    /** @brief Returns a snapshot of the session counters. */
    Stats GetStats();

    /** @brief True once the draw detour is installed. */
    bool Installed();
}
