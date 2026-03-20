set(DEPS_INSTALL_DIR "${CMAKE_BINARY_DIR}/deps/install")

# Create the include dir now so CMake's configure-time validation of
# INTERFACE_INCLUDE_DIRECTORIES on IMPORTED targets doesn't fail.
file(MAKE_DIRECTORY "${DEPS_INSTALL_DIR}/include")

# ── gperftools 2.15 ──────────────────────────────────────────────────────────
if(APPLE)
    set(TCMALLOC_LIB "${DEPS_INSTALL_DIR}/lib/libtcmalloc_minimal.dylib")
else()
    set(TCMALLOC_LIB "${DEPS_INSTALL_DIR}/lib/libtcmalloc_minimal.so")
endif()

ExternalProject_Add(gperftools_ep
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    URL https://github.com/gperftools/gperftools/releases/download/gperftools-2.15/gperftools-2.15.tar.gz
    # NOTE: verify this SHA256 hash against the official release
    URL_HASH SHA256=c69fef855628c81ef56f12e3c58f2b7ce1f326c0a1fe783e5cae0b88cbbe9a80
    INSTALL_DIR "${DEPS_INSTALL_DIR}"
    CONFIGURE_COMMAND <SOURCE_DIR>/configure
        --prefix=<INSTALL_DIR>
        --enable-shared
        --disable-static
        --disable-cpu-profiler
        --disable-heap-profiler
        --disable-heap-checker
        --disable-debugalloc
    BUILD_COMMAND make -j4 libtcmalloc_minimal.la
    INSTALL_COMMAND make install-libLTLIBRARIES install-perftoolsincludeHEADERS
    BUILD_BYPRODUCTS "${TCMALLOC_LIB}"
)

add_library(Tcmalloc::Tcmalloc SHARED IMPORTED GLOBAL)
set_target_properties(Tcmalloc::Tcmalloc PROPERTIES
    IMPORTED_LOCATION "${TCMALLOC_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${DEPS_INSTALL_DIR}/include")
add_dependencies(Tcmalloc::Tcmalloc gperftools_ep)

# ── Tcl 8.6.15 ───────────────────────────────────────────────────────────────
if(APPLE)
    set(TCL_LIB "${DEPS_INSTALL_DIR}/lib/libtcl8.6.dylib")
    set(TCMALLOC_LDFLAGS "-Wl,-force_load,${TCMALLOC_LIB}")
else()
    set(TCL_LIB "${DEPS_INSTALL_DIR}/lib/libtcl8.6.so")
    set(TCMALLOC_LDFLAGS "-Wl,--whole-archive,${TCMALLOC_LIB},--no-whole-archive")
endif()

ExternalProject_Add(tcl_ep
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    DEPENDS gperftools_ep
    URL https://prdownloads.sourceforge.net/tcl/tcl8.6.15-src.tar.gz
    URL_HASH SHA256=861e159753f2e2fbd6ec1484103715b0be56be3357522b858d3cbb5f893ffef1
    SOURCE_SUBDIR unix
    INSTALL_DIR "${DEPS_INSTALL_DIR}"
    CONFIGURE_COMMAND <SOURCE_DIR>/unix/configure
        --prefix=<INSTALL_DIR>
        --enable-threads
        --enable-shared
        --disable-static
    BUILD_COMMAND make -j4 binaries libraries "LDFLAGS=${TCMALLOC_LDFLAGS}"
    INSTALL_COMMAND make install-binaries install-libraries install-headers install-private-headers
    BUILD_BYPRODUCTS "${TCL_LIB}"
)

# On macOS, patch Tcl's dylib ID to @rpath so installed binaries use RPATH lookup
# rather than the absolute build-dir path.
if(APPLE)
    ExternalProject_Add_Step(tcl_ep fix_install_name
        COMMAND install_name_tool -id "@rpath/libtcl8.6.dylib" "${TCL_LIB}"
        DEPENDEES install
    )
endif()

add_library(Tcl::Tcl SHARED IMPORTED GLOBAL)
set_target_properties(Tcl::Tcl PROPERTIES
    IMPORTED_LOCATION "${TCL_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${DEPS_INSTALL_DIR}/include")
add_dependencies(Tcl::Tcl tcl_ep)

# ── OpenSSL 3.5.0 (LTS; QUIC server APIs for HTTP/3 via ngtcp2 crypto_ossl) ───
if(APPLE)
    set(OPENSSL_SSL_LIB "${DEPS_INSTALL_DIR}/lib/libssl.dylib")
    set(OPENSSL_CRYPTO_LIB "${DEPS_INSTALL_DIR}/lib/libcrypto.dylib")
else()
    set(OPENSSL_SSL_LIB "${DEPS_INSTALL_DIR}/lib/libssl.so")
    set(OPENSSL_CRYPTO_LIB "${DEPS_INSTALL_DIR}/lib/libcrypto.so")
endif()

if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    set(OPENSSL_TARGET "darwin64-arm64-cc")
elseif(APPLE)
    set(OPENSSL_TARGET "darwin64-x86_64-cc")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    set(OPENSSL_TARGET "linux-aarch64")
else()
    set(OPENSSL_TARGET "linux-x86_64")
endif()

ExternalProject_Add(openssl_ep
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    URL https://www.openssl.org/source/openssl-3.5.0.tar.gz
    URL_HASH SHA256=344d0a79f1a9b08029b0744e2cc401a43f9c90acd1044d09a530b4885a8e9fc0
    INSTALL_DIR "${DEPS_INSTALL_DIR}"
    CONFIGURE_COMMAND <SOURCE_DIR>/Configure
        ${OPENSSL_TARGET}
        --prefix=<INSTALL_DIR>
        --openssldir=<INSTALL_DIR>/ssl
        shared
        no-tests
    BUILD_COMMAND make -j4
    INSTALL_COMMAND make install_sw
    BUILD_BYPRODUCTS "${OPENSSL_SSL_LIB}" "${OPENSSL_CRYPTO_LIB}"
)

add_library(OpenSSL::SSL SHARED IMPORTED GLOBAL)
set_target_properties(OpenSSL::SSL PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_SSL_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${DEPS_INSTALL_DIR}/include")
add_dependencies(OpenSSL::SSL openssl_ep)

add_library(OpenSSL::Crypto SHARED IMPORTED GLOBAL)
set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${DEPS_INSTALL_DIR}/include")
add_dependencies(OpenSSL::Crypto openssl_ep)
