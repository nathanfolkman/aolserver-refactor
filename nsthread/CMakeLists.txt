set(NSTHREAD_SRCS
    error.c master.c memory.c mutex.c cslock.c rwlock.c
    reentrant.c sema.c thread.c tls.c compat.c time.c
    pthread.c fork.c signal.c
)

add_library(nsthread SHARED ${NSTHREAD_SRCS})
target_compile_definitions(nsthread PRIVATE "NSTHREAD_EXPORTS=")
target_include_directories(nsthread
    SYSTEM PRIVATE
        ${DEPS_INSTALL_DIR}/include
)
target_link_libraries(nsthread
    PUBLIC Tcl::Tcl
    PRIVATE Tcmalloc::Tcmalloc
)
set_target_properties(nsthread PROPERTIES
    OUTPUT_NAME nsthread
    VERSION 1.0
    SOVERSION 1
)

# nsthreadtest binary
add_executable(nsthreadtest nsthreadtest.c)
target_link_libraries(nsthreadtest PRIVATE nsthread)
add_dependencies(nsthreadtest tcl_ep gperftools_ep)

install(TARGETS nsthread LIBRARY DESTINATION lib)
install(TARGETS nsthreadtest RUNTIME DESTINATION bin)
