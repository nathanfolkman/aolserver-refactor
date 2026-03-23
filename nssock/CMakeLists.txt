add_library(nssock MODULE
    nssock.c
    ${PROJECT_SOURCE_DIR}/include/nsmodinit.c
)
target_compile_definitions(nssock PRIVATE NS_MODINIT=NsSock_ModInit)
target_include_directories(nssock
    SYSTEM PRIVATE
        ${DEPS_INSTALL_DIR}/include
)
target_link_libraries(nssock PRIVATE nsd)
set_target_properties(nssock PROPERTIES PREFIX "" SUFFIX ".so")
add_dependencies(nssock tcl_ep gperftools_ep)

install(TARGETS nssock LIBRARY DESTINATION bin)
