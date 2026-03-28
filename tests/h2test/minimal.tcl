# Minimal AOLserver config for HTTP/2 TLS testing.
# Run from repo root:
#   DYLD_LIBRARY_PATH=build/nsd:build/nsthread:deps-install/lib \
#   NS_TCL_LIBRARY=deps-install/lib/tcl8.6 \
#   build/nsd/nsd -ft tests/h2test/minimal.tcl
# Optional: NSD_BUILD_DIR (e.g. build-h3) so modules match the nsd binary; NSSOCK_PORT
# when 127.0.0.1:8080 is already in use; H2SPEC_TLS_PORT for nsssl (default 8443).
# TLS PEMs under servers/<server>/modules/nsssl/ are gitignored; run
# tests/h2test/generate-tls-certs.sh (or use run-h2spec.sh --start-nsd).

set homedir      [file normalize [file dirname [ns_info config]]]
set top          [file normalize [file join $homedir ../..]]
if {[info exists env(NSD_BUILD_DIR)] && $env(NSD_BUILD_DIR) ne ""} {
    set builddir [file normalize $env(NSD_BUILD_DIR)]
} else {
    set builddir [file join $top build]
}
file mkdir [file join $homedir log]
set servername   s1
# CMake uses .so for loadable modules on all platforms (not [info sharedlibextension]).
set nssockso     [file join $builddir nssock nssock.so]
set nssslso      [file join $builddir nsssl nsssl.so]

ns_section ns/parameters
ns_param   home            $homedir
ns_param   debug           true

ns_section ns/mimetypes
ns_param   default         "*/*"
ns_param   .html           "text/html; charset=utf-8"

ns_section ns/threads
ns_param   maxfd           1024

ns_section ns/servers
ns_param   $servername     $servername

ns_section ns/server/$servername
ns_param   hostname        localhost
ns_param   pageroot        [file join $homedir servers $servername pages]
ns_param   directoryfile   index.html
ns_param   maxthreads      10
ns_param   minthreads      2
ns_param   maxconnections  100
# Long h2spec/h3spec runs: avoid idle connection threads above minthreads exiting
# mid-suite (default threadtimeout 30s in tcl/pools.tcl).
ns_param   threadtimeout   600
ns_param   urlcharset      utf-8

ns_section ns/server/$servername/modules
ns_param   nssock          $nssockso
ns_param   nsssl           $nssslso

ns_section ns/server/$servername/module/nssock
ns_param   hostname        localhost
ns_param   address         127.0.0.1
set nssockport 8080
if {[info exists env(NSSOCK_PORT)]} {
    if {[string is integer -strict $env(NSSOCK_PORT)] \
            && $env(NSSOCK_PORT) >= 1024 && $env(NSSOCK_PORT) < 65536} {
        set nssockport $env(NSSOCK_PORT)
    }
}
ns_param   port            $nssockport

ns_section ns/server/$servername/module/nsssl
ns_param   hostname        localhost
ns_param   address         127.0.0.1
set h2tlsport 8443
if {[info exists env(H2SPEC_TLS_PORT)]} {
    if {[string is integer -strict $env(H2SPEC_TLS_PORT)] \
            && $env(H2SPEC_TLS_PORT) >= 1024 && $env(H2SPEC_TLS_PORT) < 65536} {
        set h2tlsport $env(H2SPEC_TLS_PORT)
    }
}
ns_param   port            $h2tlsport
ns_param   certificate     [file join $homedir servers $servername modules nsssl cert.pem]
ns_param   key             [file join $homedir servers $servername modules nsssl key.pem]
