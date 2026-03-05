cmake_minimum_required(VERSION 3.20)
project(AOLserver C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(NS_BUILD_DEPS "Download and build Tcl, OpenSSL, gperftools from source" ON)
option(NS_WITH_SSL "Build nsssl module" ON)
option(NS_WITH_ZLIB "Build nszlib module" ON)

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
include(CheckFunctionExists)
include(CheckIncludeFile)

include(cmake/CompilerWarnings.cmake)
include(cmake/Dependencies.cmake)
include(cmake/InstallLayout.cmake)

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
