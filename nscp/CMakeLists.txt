add_library(nscp MODULE
    nscp.c
    ${PROJECT_SOURCE_DIR}/include/nsmodinit.c
)
target_compile_definitions(nscp PRIVATE NS_MODINIT=NsCp_ModInit)
target_include_directories(nscp
    SYSTEM PRIVATE
        ${DEPS_INSTALL_DIR}/include
)
target_link_libraries(nscp PRIVATE nsd)
set_target_properties(nscp PROPERTIES PREFIX "" SUFFIX ".so")
add_dependencies(nscp tcl_ep gperftools_ep)

install(TARGETS nscp LIBRARY DESTINATION bin)
