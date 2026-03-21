# nghttp2 for HTTP/2 (RFC 7540). Built into deps install prefix.

if(APPLE)
    set(NGHTTP2_LIB "${DEPS_INSTALL_DIR}/lib/libnghttp2.dylib")
else()
    set(NGHTTP2_LIB "${DEPS_INSTALL_DIR}/lib/libnghttp2.so")
endif()

ExternalProject_Add(nghttp2_ep
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    URL https://github.com/nghttp2/nghttp2/releases/download/v1.61.0/nghttp2-1.61.0.tar.gz
    URL_HASH SHA256=aa7594c846e56a22fbf3d6e260e472268808d3b49d5e0ed339f589e9cc9d484c
    # h2spec http2/7.2: default libnghttp2 server sends GOAWAY after 1000 inbound
    # RST_STREAM frames on one connection; a full h2spec run exceeds that before §7.2.
    PATCH_COMMAND patch -d <SOURCE_DIR> -p1 -i "${CMAKE_CURRENT_LIST_DIR}/patches/nghttp2-1.61.0-disable-stream-reset-goaway.patch"
    INSTALL_DIR "${DEPS_INSTALL_DIR}"
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR>
        --enable-lib-only --disable-static --enable-shared
    BUILD_COMMAND ${MAKE_EXECUTABLE} -C lib -j4
    INSTALL_COMMAND ${MAKE_EXECUTABLE} -C lib install
    BUILD_BYPRODUCTS "${NGHTTP2_LIB}"
)

add_library(Nghttp2::nghttp2 SHARED IMPORTED GLOBAL)
set_target_properties(Nghttp2::nghttp2 PROPERTIES
    IMPORTED_LOCATION "${NGHTTP2_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${DEPS_INSTALL_DIR}/include")
add_dependencies(Nghttp2::nghttp2 nghttp2_ep)
