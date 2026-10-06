// wxl-modern-adt: definition of the extension-wide service table pointer.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "ExtensionApi.hpp"

namespace wxl_modern_adt
{
    const WXL_Api* g_api = nullptr;
    const WXL_FdidApi* g_fdid = nullptr;
    const WXL_LoadPoolApi* g_loadPool = nullptr;
}
