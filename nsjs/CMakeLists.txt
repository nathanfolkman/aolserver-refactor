enable_language(CXX)

add_library(nsjs MODULE
    nsjs.cc
    ${PROJECT_SOURCE_DIR}/include/nsmodinit.c
)

set_target_properties(nsjs PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    PREFIX                ""
    SUFFIX                ".so"
    INSTALL_RPATH_USE_LINK_PATH TRUE
)

target_compile_definitions(nsjs PRIVATE
    "NS_MODINIT=NsJs_ModInit"
)
if(NS_WITH_HTTP3)
    target_compile_definitions(nsjs PRIVATE HAVE_NGHTTP3=1)
endif()


target_include_directories(nsjs SYSTEM PRIVATE
    ${CMAKE_BINARY_DIR}/deps/install/include
)

target_link_libraries(nsjs PRIVATE nsd V8::V8 Tcmalloc::Tcmalloc)

add_dependencies(nsjs tcl_ep gperftools_ep)

install(TARGETS nsjs LIBRARY DESTINATION bin)

# ---------------------------------------------------------------------------
# Integration tests
# ---------------------------------------------------------------------------
# Discover V8 lib directory for DYLD_LIBRARY_PATH on macOS
if(APPLE)
    execute_process(
        COMMAND brew --prefix v8
        OUTPUT_VARIABLE _brew_v8_prefix
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_brew_v8_prefix)
        set(_v8_lib_dir "${_brew_v8_prefix}/lib")
    endif()
endif()

set(_test_port 8765)
set(_test_install "${CMAKE_INSTALL_PREFIX}")
set(_tests_dir "${CMAKE_CURRENT_SOURCE_DIR}/tests")

add_test(
    NAME nsjs_integration
    COMMAND ${CMAKE_COMMAND} -E env
        "NSJS_INSTALL=${_test_install}"
        "NSJS_PORT=${_test_port}"
        "NSJS_V8_LIB=${_v8_lib_dir}"
        python3 test_nsjs.py -v
    WORKING_DIRECTORY "${_tests_dir}"
)

set_tests_properties(nsjs_integration PROPERTIES
    TIMEOUT 120
    LABELS  "nsjs"
)
