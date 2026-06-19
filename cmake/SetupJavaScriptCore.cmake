# SetupJavaScriptCore.cmake
#
# Locates JavaScriptCore for embedding. Resolution order:
#   1. macOS system framework (`-framework JavaScriptCore`)
#   2. Vendored prebuilt from oven-sh/WebKit under third_party/bun-webkit/
#      (downloaded once by scripts/fetch_jsc.sh — see BUILD.md)
#   3. System pkg-config (`javascriptcoregtk-4.0` / `-4.1`) as a fallback
#      for distro-managed installs (functionally the same engine).
#
# Outputs:
#   JSC_INCLUDE_DIRS
#   JSC_LIBRARIES
#   JSC_FOUND

set(JSC_FOUND FALSE)

if(APPLE)
    find_library(JSC_FRAMEWORK JavaScriptCore REQUIRED)
    set(JSC_INCLUDE_DIRS "")
    set(JSC_LIBRARIES ${JSC_FRAMEWORK})
    set(JSC_FOUND TRUE)
    message(STATUS "JSC: macOS system framework")
else()
    # (2) Vendored oven-sh/WebKit prebuilt
    set(_VENDORED_ROOT "${CMAKE_SOURCE_DIR}/third_party/bun-webkit")
    if(EXISTS "${_VENDORED_ROOT}/include/JavaScriptCore/JavaScript.h"
       AND EXISTS "${_VENDORED_ROOT}/lib/libJavaScriptCore.a")
        set(JSC_INCLUDE_DIRS "${_VENDORED_ROOT}/include")
        # Link order matters for static libs.
        set(JSC_LIBRARIES
            "${_VENDORED_ROOT}/lib/libJavaScriptCore.a"
            "${_VENDORED_ROOT}/lib/libWTF.a"
            "${_VENDORED_ROOT}/lib/libbmalloc.a"
            "${_VENDORED_ROOT}/lib/libicui18n.a"
            "${_VENDORED_ROOT}/lib/libicuuc.a"
            "${_VENDORED_ROOT}/lib/libicutu.a"
            "${_VENDORED_ROOT}/lib/libicudata.a"
        )
        set(JSC_FOUND TRUE)
        message(STATUS "JSC: vendored oven-sh/WebKit prebuilt at ${_VENDORED_ROOT}")
    else()
        # (3) Distro pkg-config fallback
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(JSC_GTK QUIET javascriptcoregtk-4.1)
            if(NOT JSC_GTK_FOUND)
                pkg_check_modules(JSC_GTK QUIET javascriptcoregtk-4.0)
            endif()
            if(JSC_GTK_FOUND)
                set(JSC_INCLUDE_DIRS ${JSC_GTK_INCLUDE_DIRS})
                set(JSC_LIBRARIES ${JSC_GTK_LIBRARIES})
                set(JSC_FOUND TRUE)
                message(STATUS "JSC: distro pkg-config (${JSC_GTK_MODULES})")
            endif()
        endif()
    endif()
endif()

if(NOT JSC_FOUND)
    message(FATAL_ERROR
        "JavaScriptCore was not found.\n"
        "On Linux, run: bash scripts/fetch_jsc.sh\n"
        "On macOS, the system framework is used automatically.\n"
        "Alternatively install libjavascriptcoregtk-4.1-dev via your distro's package manager.\n"
        "See BUILD.md for full details.")
endif()
