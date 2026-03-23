add_library(nsdb MODULE
    nsdb.c
    dbdrv.c
    dbinit.c
    dbtcl.c
    dbutil.c
    ${PROJECT_SOURCE_DIR}/include/nsmodinit.c
)
target_compile_definitions(nsdb PRIVATE NS_MODINIT=NsDb_ModInit)
target_include_directories(nsdb
    SYSTEM PRIVATE
        ${DEPS_INSTALL_DIR}/include
)
target_link_libraries(nsdb PRIVATE nsd)
set_target_properties(nsdb PROPERTIES PREFIX "" SUFFIX ".so")
add_dependencies(nsdb tcl_ep gperftools_ep)

install(TARGETS nsdb LIBRARY DESTINATION bin)
