# nghttp3 + ngtcp2 (OpenSSL crypto_ossl) for HTTP/3. Requires OpenSSL 3.5+ from Dependencies.cmake.

if(APPLE)
    set(NGHTTP3_LIB "${DEPS_INSTALL_DIR}/lib/libnghttp3.dylib")
    set(NGTCP2_LIB "${DEPS_INSTALL_DIR}/lib/libngtcp2.dylib")
    set(NGTCP2_CRYPTO_OSSL_LIB "${DEPS_INSTALL_DIR}/lib/libngtcp2_crypto_ossl.dylib")
else()
    set(NGHTTP3_LIB "${DEPS_INSTALL_DIR}/lib/libnghttp3.so")
    set(NGTCP2_LIB "${DEPS_INSTALL_DIR}/lib/libngtcp2.so")
    set(NGTCP2_CRYPTO_OSSL_LIB "${DEPS_INSTALL_DIR}/lib/libngtcp2_crypto_ossl.so")
endif()

ExternalProject_Add(nghttp3_ep
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    DEPENDS openssl_ep
    URL https://github.com/ngtcp2/nghttp3/releases/download/v1.12.0/nghttp3-1.12.0.tar.bz2
    URL_HASH SHA256=2859783537af213d67a5ce4277ddb79c8941ad45edbfa5ccdd52d9cf4ffa422d
    INSTALL_DIR "${DEPS_INSTALL_DIR}"
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND
        env PKG_CONFIG_PATH=<INSTALL_DIR>/lib/pkgconfig
        <SOURCE_DIR>/configure --prefix=<INSTALL_DIR>
        --enable-lib-only --enable-shared --disable-static
    BUILD_COMMAND ${MAKE_EXECUTABLE} -C lib -j4
    INSTALL_COMMAND ${MAKE_EXECUTABLE} -C lib install
    BUILD_BYPRODUCTS "${NGHTTP3_LIB}"
)

ExternalProject_Add(ngtcp2_ep
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    DEPENDS openssl_ep nghttp3_ep
    URL https://github.com/ngtcp2/ngtcp2/releases/download/v1.21.0/ngtcp2-1.21.0.tar.xz
    URL_HASH SHA256=2d1c07e6aa509c017516c08307b0b707cd165a17275ab5f1caff9aaa0e3b6c7d
    # h3spec TLS 8.2 sends a minimal ClientHello (no QUIC TP) in a short datagram;
    # stock ngtcp2 drops it before TLS runs, so no CONNECTION_CLOSE with TLS alert.
    PATCH_COMMAND ${CMAKE_CURRENT_LIST_DIR}/patches/apply-ngtcp2-undersized-initial.sh
        <SOURCE_DIR> ${CMAKE_CURRENT_LIST_DIR}/patches/ngtcp2-1.21.0-allow-undersized-initial.patch
    INSTALL_DIR "${DEPS_INSTALL_DIR}"
    BUILD_IN_SOURCE 1
    # Many dev machines lack pkg-config; ngtcp2's OpenSSL probe then fails. Pin bundled OpenSSL.
    CONFIGURE_COMMAND
        ${CMAKE_COMMAND} -E env
        "PKG_CONFIG_PATH=${DEPS_INSTALL_DIR}/lib/pkgconfig"
        "OPENSSL_CFLAGS=-I${DEPS_INSTALL_DIR}/include"
        "OPENSSL_LIBS=-L${DEPS_INSTALL_DIR}/lib -lssl -lcrypto"
        <SOURCE_DIR>/configure --prefix=<INSTALL_DIR>
        --with-openssl --enable-lib-only --enable-shared --disable-static
    BUILD_COMMAND ${MAKE_EXECUTABLE} -j4
    INSTALL_COMMAND ${MAKE_EXECUTABLE} install
    BUILD_BYPRODUCTS "${NGTCP2_LIB}" "${NGTCP2_CRYPTO_OSSL_LIB}"
)

add_library(Nghttp3::nghttp3 SHARED IMPORTED GLOBAL)
set_target_properties(Nghttp3::nghttp3 PROPERTIES
    IMPORTED_LOCATION "${NGHTTP3_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${DEPS_INSTALL_DIR}/include")
add_dependencies(Nghttp3::nghttp3 nghttp3_ep)

add_library(Ngtcp2::ngtcp2 SHARED IMPORTED GLOBAL)
set_target_properties(Ngtcp2::ngtcp2 PROPERTIES
    IMPORTED_LOCATION "${NGTCP2_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${DEPS_INSTALL_DIR}/include")
add_dependencies(Ngtcp2::ngtcp2 ngtcp2_ep)

add_library(Ngtcp2::crypto_ossl SHARED IMPORTED GLOBAL)
set_target_properties(Ngtcp2::crypto_ossl PROPERTIES
    IMPORTED_LOCATION "${NGTCP2_CRYPTO_OSSL_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${DEPS_INSTALL_DIR}/include"
    INTERFACE_LINK_LIBRARIES "OpenSSL::SSL;OpenSSL::Crypto")
add_dependencies(Ngtcp2::crypto_ossl ngtcp2_ep)
