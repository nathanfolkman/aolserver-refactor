add_library(nsperm MODULE
    nsperm.c
    ${PROJECT_SOURCE_DIR}/include/nsmodinit.c
)
target_compile_definitions(nsperm PRIVATE NS_MODINIT=NsPerm_ModInit)
target_include_directories(nsperm
    SYSTEM PRIVATE
        ${CMAKE_BINARY_DIR}/deps/install/include
)
target_link_libraries(nsperm PRIVATE nsd)
set_target_properties(nsperm PROPERTIES PREFIX "" SUFFIX ".so")
add_dependencies(nsperm tcl_ep gperftools_ep)

install(TARGETS nsperm LIBRARY DESTINATION bin)
