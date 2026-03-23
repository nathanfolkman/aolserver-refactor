# nsproxy module (loaded by AOLserver)
add_library(nsproxy MODULE
    nsproxylib.c
    nsproxymod.c
    ${PROJECT_SOURCE_DIR}/include/nsmodinit.c
)
target_compile_definitions(nsproxy PRIVATE NS_MODINIT=NsProxy_ModInit)
target_include_directories(nsproxy
    SYSTEM PRIVATE
        ${DEPS_INSTALL_DIR}/include
)
target_link_libraries(nsproxy PRIVATE nsd)
set_target_properties(nsproxy PROPERTIES PREFIX "" SUFFIX ".so")
add_dependencies(nsproxy tcl_ep gperftools_ep)

install(TARGETS nsproxy LIBRARY DESTINATION bin)

# nsproxy standalone worker binary (nsproxy.c + nsproxylib.c)
add_executable(nsproxybin nsproxy.c nsproxylib.c)
set_target_properties(nsproxybin PROPERTIES OUTPUT_NAME "nsproxy")
target_include_directories(nsproxybin
    SYSTEM PRIVATE
        ${DEPS_INSTALL_DIR}/include
)
target_link_libraries(nsproxybin PRIVATE nsd)
add_dependencies(nsproxybin tcl_ep gperftools_ep)

install(TARGETS nsproxybin RUNTIME DESTINATION bin)
install(FILES nsproxy.h DESTINATION include)
