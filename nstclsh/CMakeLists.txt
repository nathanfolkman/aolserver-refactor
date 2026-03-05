add_executable(nstclsh nstclsh.c)
target_link_libraries(nstclsh PRIVATE nsd)
add_dependencies(nstclsh tcl_ep gperftools_ep)

install(TARGETS nstclsh RUNTIME DESTINATION bin)
