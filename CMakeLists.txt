cmake_minimum_required(VERSION 3.20)
project(AOLserver C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(NS_BUILD_DEPS "Download and build Tcl, OpenSSL, gperftools from source" ON)
option(NS_WITH_SSL "Build nsssl module" ON)
option(NS_WITH_ZLIB "Build nszlib module" ON)
option(NS_WITH_V8 "Build nsjs JavaScript module (requires V8)" OFF)
option(NS_WITH_HTTP2 "HTTP/2 over TLS (ALPN h2) via nghttp2" ON)
option(NS_WITH_HTTP3 "HTTP/3 over QUIC (experimental)" OFF)
option(NS_USE_SYSTEM_OPENSSL "Use system OpenSSL (FindOpenSSL) instead of building openssl_ep" OFF)

set(CMAKE_INSTALL_PREFIX "/usr/local/aolserver" CACHE PATH "Install prefix")

# RPATH settings
if(APPLE)
    set(CMAKE_INSTALL_RPATH "@loader_path/../lib")
else()
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
endif()
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
set(CMAKE_MACOSX_RPATH TRUE)

include(ExternalProject)
# Autotools deps (nghttp2, nghttp3, ngtcp2) use Makefiles; do not use CMAKE_MAKE_PROGRAM,
# which is `ninja` when the top-level generator is Ninja.
find_program(MAKE_EXECUTABLE NAMES gmake make REQUIRED)
include(CheckFunctionExists)
include(CheckIncludeFile)

include(cmake/CompilerWarnings.cmake)
if(NS_WITH_HTTP3 AND NS_USE_SYSTEM_OPENSSL)
    message(FATAL_ERROR
        "NS_WITH_HTTP3 requires bundled OpenSSL 3.5+ (QUIC APIs). "
        "Set NS_USE_SYSTEM_OPENSSL=OFF or disable NS_WITH_HTTP3.")
endif()
include(cmake/Dependencies.cmake)
if(NS_WITH_HTTP2)
    include(cmake/Http23Deps.cmake)
endif()
if(NS_WITH_HTTP3)
    if(NOT NS_WITH_HTTP2)
        message(FATAL_ERROR "NS_WITH_HTTP3 requires NS_WITH_HTTP2=ON")
    endif()
    if(NOT NS_WITH_SSL)
        message(FATAL_ERROR "NS_WITH_HTTP3 requires NS_WITH_SSL=ON")
    endif()
    include(cmake/Http3Deps.cmake)
endif()
include(cmake/InstallLayout.cmake)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Feature checks (replacing autoconf)
check_function_exists(timegm HAVE_TIMEGM)
check_function_exists(drand48 HAVE_DRAND48)
check_function_exists(random HAVE_RANDOM)
check_function_exists(getaddrinfo HAVE_GETADDRINFO)
check_function_exists(getnameinfo HAVE_GETNAMEINFO)
check_include_file(inttypes.h HAVE_INTTYPES_H)

# poll is broken on Apple
if(NOT APPLE)
    check_function_exists(poll HAVE_POLL)
endif()

# _NSGetEnviron is always present on Apple
if(APPLE)
    set(HAVE__NSGETENVIRON 1)
endif()

# NO_CONST: always defined (Tcl API compat)
set(NO_CONST 1)

set(HAVE_NGHTTP2 0)
if(NS_WITH_HTTP2)
    set(HAVE_NGHTTP2 1)
endif()
set(HAVE_NGHTTP3 0)
if(NS_WITH_HTTP3)
    set(HAVE_NGHTTP3 1)
endif()

configure_file(cmake/nsconfig.h.in ${CMAKE_BINARY_DIR}/include/nsconfig.h)

# Signal ns.h to include nsconfig.h
add_compile_definitions(HAVE_NSCONFIG_H)

# Global include dirs for all targets
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/include
)

add_subdirectory(nsthread)
add_subdirectory(nsd)
add_subdirectory(nstclsh)
add_subdirectory(nssock)
add_subdirectory(nslog)
add_subdirectory(nsdb)
add_subdirectory(nscgi)
add_subdirectory(nscp)
add_subdirectory(nsperm)
add_subdirectory(nsproxy)
add_subdirectory(nszlib)
if(NS_WITH_SSL)
    add_subdirectory(nsssl)
endif()

if(NS_WITH_V8)
    enable_language(CXX)
    find_package(V8 REQUIRED)
    enable_testing()
    add_subdirectory(nsjs)
endif()
