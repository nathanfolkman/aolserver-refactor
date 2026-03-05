# Compiler warning flags for AOLserver (macOS ARM64 Clang / Linux GCC)
add_compile_options(
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-deprecated-declarations
    -Wno-unused-variable
    # Legacy C code suppressions (pre-ANSI K&R patterns, old int/ptr tricks)
    -Wno-unused-but-set-variable
    -Wno-pointer-sign
    -Wno-format
    -Wno-missing-field-initializers
    -Wno-deprecated-non-prototype
)

if(APPLE)
    add_compile_options(
        -Wno-shorten-64-to-32
        -Wno-incompatible-pointer-types
        -Wno-int-to-void-pointer-cast
        -Wno-void-pointer-to-int-cast
        -Wno-pointer-to-int-cast
        -Wno-int-to-pointer-cast
        -Wno-cast-function-type-mismatch
    )
    # _DARWIN_C_SOURCE exposes POSIX, BSD, and Darwin extensions.
    # Do NOT combine with _POSIX_C_SOURCE — on macOS 15+ that causes
    # conflicting type declarations (getsubopt, mknod, setkey) between
    # unistd.h and stdlib.h/sys/stat.h.
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()

# Treat dep include dirs as SYSTEM to suppress their warnings
# (set per-target in each CMakeLists.txt using SYSTEM keyword)
