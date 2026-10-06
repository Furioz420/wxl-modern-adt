// wxl-modern-adt: native split-ADT reader (split root/_tex0/_obj0 tiles) + terrain height blend, as an
// out-of-core extension. Entry point.
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

#include "load/AdtSplitInternal.hpp"
#include "ExtensionApi.hpp"

const WXL_PluginInfo* __cdecl WXL_Query(void)
{
    static const WXL_PluginInfo info = {
        sizeof(WXL_PluginInfo),
        WXL_API_VERSION,
        "wxl-modern-adt",
        1,
        WXL_CLIENT_BUILD,
    };
    return &info;
}

int __cdecl WXL_Load(const WXL_Api* api)
{
    if (!api || api->apiVersion != WXL_API_VERSION) return 0;

    wxl_modern_adt::g_api = api;

    // adt-chunk-build is unconditional (publishes OnAdtChunkBuild regardless of kEnabled -- always
    // was, even back when this was a core WXL_REGISTER_FEATURE("adt-chunk-build", true, ...)).
    wxl_modern_adt::InstallChunkBuild();

    if constexpr (wxl_modern_adt::kEnabled)
    {
        wxl::runtime::adtsplit::detail::InstallAdtSplit();
        wxl_modern_adt::InstallHeightBlend();
    }

    api->Log(WXL_LOG_INFO, "wxl-modern-adt", "native split-ADT reader active");
    return 1;
}
