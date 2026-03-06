# FindV8.cmake
#
# Discovers a system-installed V8 engine.
# Priority:
#   1. User override: -DV8_ROOT=/path
#   2. pkg-config v8 (Linux)
#   3. brew --prefix v8 (macOS fallback)
#
# Produces: V8::V8 INTERFACE IMPORTED target

if(TARGET V8::V8)
    return()
endif()

# --- 1. User-specified root ---
if(DEFINED V8_ROOT)
    set(_v8_hint_include "${V8_ROOT}/include")
    set(_v8_hint_lib     "${V8_ROOT}/lib")
endif()

# --- 2. pkg-config (Linux) ---
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT DEFINED V8_ROOT)
    pkg_check_modules(PC_V8 QUIET v8)
    if(PC_V8_FOUND)
        set(_v8_hint_include ${PC_V8_INCLUDE_DIRS})
        set(_v8_hint_lib     ${PC_V8_LIBRARY_DIRS})
    endif()
endif()

# --- 3. brew fallback (macOS) ---
if(APPLE AND NOT _v8_hint_include)
    find_program(_brew brew)
    if(_brew)
        execute_process(
            COMMAND ${_brew} --prefix v8
            OUTPUT_VARIABLE _brew_v8_prefix
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(_brew_v8_prefix AND NOT _brew_v8_prefix STREQUAL "")
            set(_v8_hint_include "${_brew_v8_prefix}/include")
            set(_v8_hint_lib     "${_brew_v8_prefix}/lib")
        endif()
    endif()
endif()

# --- Find header ---
find_path(V8_INCLUDE_DIR
    NAMES v8.h
    HINTS ${_v8_hint_include}
    PATH_SUFFIXES include
)

# --- Find platform header (v8-platform.h lives alongside v8.h) ---
find_path(V8_PLATFORM_INCLUDE_DIR
    NAMES v8-platform.h
    HINTS ${_v8_hint_include}
    PATH_SUFFIXES include
)

# --- Find libraries ---
# Prefer monolith; fall back to split v8 + v8_libplatform
find_library(V8_MONOLITH_LIBRARY
    NAMES v8_monolith
    HINTS ${_v8_hint_lib}
    PATH_SUFFIXES lib
)

find_library(V8_LIBRARY
    NAMES v8
    HINTS ${_v8_hint_lib}
    PATH_SUFFIXES lib
)

find_library(V8_LIBPLATFORM_LIBRARY
    NAMES v8_libplatform
    HINTS ${_v8_hint_lib}
    PATH_SUFFIXES lib
)

# --- Determine which library set to use ---
if(V8_MONOLITH_LIBRARY)
    set(_v8_libs "${V8_MONOLITH_LIBRARY}")
elseif(V8_LIBRARY AND V8_LIBPLATFORM_LIBRARY)
    set(_v8_libs "${V8_LIBRARY};${V8_LIBPLATFORM_LIBRARY}")
elseif(V8_LIBRARY)
    set(_v8_libs "${V8_LIBRARY}")
endif()

# --- Standard find_package machinery ---
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(V8
    REQUIRED_VARS V8_INCLUDE_DIR _v8_libs
    FAIL_MESSAGE "V8 not found. Install via: brew install v8 (macOS) or apt install libv8-dev (Linux), or set V8_ROOT."
)

if(V8_FOUND)
    add_library(V8::V8 INTERFACE IMPORTED)
    set_target_properties(V8::V8 PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${V8_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES      "${_v8_libs}"
        # V8 14+ is built with pointer compression and sandbox enabled by default
        # on 64-bit platforms.  Embedder code must define the same flags so that
        # v8-internal.h chooses matching struct layouts; V8::Initialize() aborts
        # at startup if there is a mismatch.
        INTERFACE_COMPILE_DEFINITIONS "V8_COMPRESS_POINTERS;V8_ENABLE_SANDBOX"
    )
    mark_as_advanced(V8_INCLUDE_DIR V8_MONOLITH_LIBRARY V8_LIBRARY V8_LIBPLATFORM_LIBRARY)
endif()
