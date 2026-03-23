set(NSD_SRCS
    adpcmds.c adpeval.c adpparse.c adprequest.c auth.c binder.c
    cache.c callbacks.c cls.c compress.c config.c conn.c connio.c
    crypt.c dns.c driver.c dsprintf.c dstring.c encoding.c exec.c
    fastpath.c fd.c filter.c form.c httptime.c http2.c index.c info.c
    init.c limits.c lisp.c listen.c log.c mimetypes.c modload.c
    nsconf.c nsmain.c nsthread.c op.c pathname.c pidfile.c pools.c
    proc.c queue.c quotehtml.c random.c request.c return.c
    rollfile.c sched.c server.c set.c sock.c sockcallback.c str.c
    task.c tclcache.c     tclcmds.c tclconf.c tclenv.c tclfile.c
    tclhttp.c tclhttp2.c tclhttp3.c tclimg.c tclinit.c tcljob.c tclloop.c tclmisc.c
    tclobj.c tclrequest.c tclresp.c tclsched.c tclset.c tclshare.c
    tclsock.c tclstore.c tclthread.c tclvar.c     tclxkeylist.c url.c
    urlencode.c urlopen.c urlspace.c uuencode.c stamp.c unix.c
)
if(NS_WITH_HTTP3)
    list(APPEND NSD_SRCS http3.c)
endif()

add_library(nsd SHARED ${NSD_SRCS})
target_compile_definitions(nsd PRIVATE "NSD_EXPORTS=")
target_include_directories(nsd
    SYSTEM PRIVATE
        ${DEPS_INSTALL_DIR}/include
)
target_link_libraries(nsd
    PUBLIC nsthread Tcl::Tcl
    PRIVATE Tcmalloc::Tcmalloc
)
if(NS_WITH_HTTP2)
    target_link_libraries(nsd PRIVATE Nghttp2::nghttp2)
    add_dependencies(nsd nghttp2_ep)
endif()
if(NS_WITH_HTTP3)
    target_link_libraries(nsd PRIVATE
        Nghttp3::nghttp3
        Ngtcp2::ngtcp2
        Ngtcp2::crypto_ossl
        OpenSSL::SSL
        OpenSSL::Crypto
    )
    add_dependencies(nsd nghttp3_ep ngtcp2_ep)
endif()
set_target_properties(nsd PROPERTIES
    OUTPUT_NAME nsd
    VERSION 1.0
    SOVERSION 1
)

# nsd server binary
add_executable(nsd_bin main.c)
set_target_properties(nsd_bin PROPERTIES OUTPUT_NAME nsd)
target_link_libraries(nsd_bin PRIVATE nsd)
add_dependencies(nsd_bin tcl_ep gperftools_ep)

# Install init.tcl alongside nsd binary
install(TARGETS nsd LIBRARY DESTINATION lib)
install(TARGETS nsd_bin RUNTIME DESTINATION bin)
install(FILES init.tcl DESTINATION bin)
