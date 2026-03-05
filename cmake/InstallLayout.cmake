# AOLserver install layout
# bin/  -> nsd, nstclsh, nsthreadtest, nsproxy binary, all *.so modules
# lib/  -> libnsthread, libnsd
# include/ -> public headers
# modules/tcl/ -> tcl scripts
# log/ -> empty directory
# servers/server1/pages/ -> index.adp

set(NS_INSTALL_BIN     bin)
set(NS_INSTALL_LIB     lib)
set(NS_INSTALL_INC     include)
set(NS_INSTALL_MODTCL  modules/tcl)

install(DIRECTORY DESTINATION ${NS_INSTALL_BIN})
install(DIRECTORY DESTINATION ${NS_INSTALL_LIB})
install(DIRECTORY DESTINATION log)
install(DIRECTORY DESTINATION servers/server1/pages)

install(FILES
    include/ns.h
    include/nsthread.h
    include/nsdb.h
    include/nspd.h
    include/nsextmsg.h
    include/nsattributes.h
    DESTINATION ${NS_INSTALL_INC})

install(DIRECTORY tcl/
    DESTINATION ${NS_INSTALL_MODTCL}
    FILES_MATCHING PATTERN "*.tcl")

install(FILES index.adp
    DESTINATION servers/server1/pages)

# Install Tcl runtime (dylib + scripts) and tcmalloc from the deps build
install(DIRECTORY "${DEPS_INSTALL_DIR}/lib/tcl8.6"
    DESTINATION ${NS_INSTALL_LIB})

if(APPLE)
    install(FILES
        "${DEPS_INSTALL_DIR}/lib/libtcl8.6.dylib"
        "${DEPS_INSTALL_DIR}/lib/libtcmalloc_minimal.dylib"
        "${DEPS_INSTALL_DIR}/lib/libtcmalloc_minimal.4.dylib"
        DESTINATION ${NS_INSTALL_LIB})
else()
    install(FILES
        "${DEPS_INSTALL_DIR}/lib/libtcl8.6.so"
        "${DEPS_INSTALL_DIR}/lib/libtcmalloc_minimal.so"
        DESTINATION ${NS_INSTALL_LIB})
endif()
