add_library(nslog MODULE
    nslog.c
    ${PROJECT_SOURCE_DIR}/include/nsmodinit.c
)
target_compile_definitions(nslog PRIVATE NS_MODINIT=NsLog_ModInit)
target_include_directories(nslog
    SYSTEM PRIVATE
        ${DEPS_INSTALL_DIR}/include
)
target_link_libraries(nslog PRIVATE nsd)
set_target_properties(nslog PROPERTIES PREFIX "" SUFFIX ".so")
add_dependencies(nslog tcl_ep gperftools_ep)

install(TARGETS nslog LIBRARY DESTINATION bin)
