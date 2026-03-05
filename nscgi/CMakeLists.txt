add_library(nscgi MODULE
    nscgi.c
    ${PROJECT_SOURCE_DIR}/include/nsmodinit.c
)
target_compile_definitions(nscgi PRIVATE NS_MODINIT=NsCgi_ModInit)
target_include_directories(nscgi
    SYSTEM PRIVATE
        ${CMAKE_BINARY_DIR}/deps/install/include
)
target_link_libraries(nscgi PRIVATE nsd)
set_target_properties(nscgi PROPERTIES PREFIX "" SUFFIX ".so")
add_dependencies(nscgi tcl_ep gperftools_ep)

install(TARGETS nscgi LIBRARY DESTINATION bin)
