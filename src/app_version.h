#pragma once

// ============================================================================
// Windows Config Studio - Centralized Version Definitions
// ============================================================================
// Whenever a new release is prepared (e.g. 2.1, 2.2, 3.0, 5.0, 10.0, 15.0),
// simply update WCS_VERSION_MAJOR and WCS_VERSION_MINOR below.
// Both the PE VERSIONINFO in app.rc and the runtime version detection will
// automatically pick up these values.
// ============================================================================

#ifndef WCS_VERSION_MAJOR
#define WCS_VERSION_MAJOR 3
#define WCS_VERSION_MINOR 5
#define WCS_VERSION_PATCH 0
#define WCS_VERSION_BUILD 0
#endif

#define WCS_STRINGIFY_HELPER(x) #x
#define WCS_STRINGIFY(x) WCS_STRINGIFY_HELPER(x)

#define WCS_VERSION_NUM WCS_STRINGIFY(WCS_VERSION_MAJOR) "." WCS_STRINGIFY(WCS_VERSION_MINOR)
#define WCS_VERSION_STR WCS_STRINGIFY(WCS_VERSION_MAJOR) "." WCS_STRINGIFY(WCS_VERSION_MINOR) "." WCS_STRINGIFY(WCS_VERSION_PATCH) "." WCS_STRINGIFY(WCS_VERSION_BUILD)
#define WCS_DISPLAY_VERSION WCS_VERSION_NUM
