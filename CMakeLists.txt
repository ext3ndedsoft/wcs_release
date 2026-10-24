cmake_minimum_required(VERSION 3.20)
project(WindowsConfigStudio VERSION 3.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/utf-8 /EHsc /permissive- /W3 /DNOMINMAX /Z7)
endif()

# -----------------------------------------------------------------------------
# Include directories
# Module folders are added so that existing `#include "header.h"` lines keep working.
# -----------------------------------------------------------------------------
include_directories(
    .
    src
    src/core
    src/tweaks
    src/ui
    third_party
    third_party/webview2/build/native/include
)

# -----------------------------------------------------------------------------
# Source modules
#   core    - utilities, config, logging, resources, system info, timers, restore, updater
#   tweaks  - tweak engines, memory cleaner, game boost, startup/autostart, MSI
#   ui      - entry point, WebView2 window, JS <-> native bridge
# -----------------------------------------------------------------------------
set(CORE_SOURCES
    src/core/common_utils.h
    src/core/common_utils.cpp
    src/core/config_manager.h
    src/core/config_manager.cpp
    src/core/debug_logger.h
    src/core/debug_logger.cpp
    src/core/debug_console.h
    src/core/debug_console.cpp
    src/core/resource_bundle.h
    src/core/resource_bundle.cpp
    src/core/restore_manager.h
    src/core/restore_manager.cpp
    src/core/system_info.h
    src/core/system_info.cpp
    src/core/timer_resolution.h
    src/core/timer_resolution.cpp
    src/core/update_manager.h
    src/core/update_manager.cpp
)

set(TWEAKS_SOURCES
    src/tweaks/tweaks_engine.h
    src/tweaks/tweaks_engine.cpp
    src/tweaks/system_tweaker.h
    src/tweaks/system_tweaker.cpp
    src/tweaks/system_tweaks.h
    src/tweaks/system_tweaks.cpp
    src/tweaks/win11_tweaker.h
    src/tweaks/win11_tweaker.cpp
    src/tweaks/memory_cleaner.h
    src/tweaks/memory_cleaner.cpp
    src/tweaks/game_boost_manager.h
    src/tweaks/game_boost_manager.cpp
    src/tweaks/startup_manager.h
    src/tweaks/startup_manager.cpp
    src/tweaks/autostart_manager.h
    src/tweaks/autostart_manager.cpp
    src/tweaks/msi_manager.h
    src/tweaks/msi_manager.cpp
)

set(UI_SOURCES
    src/ui/main.cpp
    src/ui/webview_window.h
    src/ui/webview_window.cpp
    src/ui/native_bridge.h
    src/ui/native_bridge.cpp
)

set(APP_SOURCES
    src/app_version.h
    src/resource.h
    src/app.rc
)

set(SOURCES
    ${CORE_SOURCES}
    ${TWEAKS_SOURCES}
    ${UI_SOURCES}
    ${APP_SOURCES}
)

# -----------------------------------------------------------------------------
# Shared configuration for both executables
# -----------------------------------------------------------------------------
set(WCS_LINK_LIBS
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/build/native/x64/WebView2LoaderStatic.lib"
    dwmapi
    user32
    gdi32
    shlwapi
    shell32
    ole32
    version
    wininet
    crypt32
    advapi32
    powrprof
    winmm
    setupapi
    cfgmgr32
    pdh
)

function(wcs_configure_target target)
    target_link_libraries(${target} PRIVATE ${WCS_LINK_LIBS})
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endfunction()

set(WCS_MANIFEST_OPT "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")

# -----------------------------------------------------------------------------
# Standalone Single-EXE GUI Executable (WCS.exe)
# -----------------------------------------------------------------------------
add_executable(WCS WIN32 ${SOURCES})
wcs_configure_target(WCS)

if (MSVC)
    target_link_options(WCS PRIVATE
        "${WCS_MANIFEST_OPT}" "/DEBUG" "/MAP:WCS.map")
endif()
