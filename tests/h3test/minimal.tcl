# Minimal AOLserver config for HTTP/3 (QUIC) + TLS HTTP/2 testing.
# Reuses certificate material from tests/h2test.
# Build with: cmake .. -DNS_WITH_HTTP3=ON
# Run:
#   DYLD_LIBRARY_PATH=build/nsd:build/nsthread:deps-install/lib \
#   NS_TCL_LIBRARY=deps-install/lib/tcl8.6 \
#   build/nsd/nsd -ft tests/h3test/minimal.tcl
# Optional: export H3SPEC_PORT=38443 (1024–65535) so nsssl TLS + QUIC use that port
# (run-h3spec.sh --start-nsd sets it automatically).

set homedir      [file normalize [file dirname [ns_info config]]]
set top          [file normalize [file join $homedir ../..]]
if {[info exists env(NSD_BUILD_DIR)] && $env(NSD_BUILD_DIR) ne ""} {
    set builddir [file normalize $env(NSD_BUILD_DIR)]
} else {
    set builddir [file join $top build]
}
set servername   s1
file mkdir [file join $homedir log]
file mkdir [file join $homedir bin]
set _init_src [file join $top tests h2test bin init.tcl]
set _init_dst [file join $homedir bin init.tcl]
if {[file exists $_init_src]} {
    file copy -force $_init_src $_init_dst
}
set nssockso     [file join $builddir nssock nssock.so]
set nssslso      [file join $builddir nsssl nsssl.so]
# Shared self-signed cert from h2test
set certdir      [file join $top tests h2test servers $servername modules nsssl]

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
ns_param   urlcharset      utf-8

ns_section ns/server/$servername/modules
ns_param   nssock          $nssockso
ns_param   nsssl           $nssslso

# Plain HTTP (nssock). Override with NSSOCK_PORT when QUIC port is auto-picked (run-h3spec.sh).
set nssockport 8080
if {[info exists env(NSSOCK_PORT)]} {
    if {[string is integer -strict $env(NSSOCK_PORT)] \
            && $env(NSSOCK_PORT) >= 1024 && $env(NSSOCK_PORT) < 65536} {
        set nssockport $env(NSSOCK_PORT)
    }
}

ns_section ns/server/$servername/module/nssock
ns_param   hostname        localhost
ns_param   address         127.0.0.1
ns_param   port            $nssockport

# TLS + QUIC share one port. Override with env H3SPEC_PORT (e.g. run-h3spec.sh).
set h3port 8443
if {[info exists env(H3SPEC_PORT)]} {
    if {[string is integer -strict $env(H3SPEC_PORT)] \
            && $env(H3SPEC_PORT) >= 1024 && $env(H3SPEC_PORT) < 65536} {
        set h3port $env(H3SPEC_PORT)
    }
}

ns_section ns/server/$servername/module/nsssl
ns_param   hostname        localhost
ns_param   address         127.0.0.1
ns_param   port            $h3port
ns_param   certificate     [file join $certdir cert.pem]
ns_param   key             [file join $certdir key.pem]
ns_param   h3              1
ns_param   h3_udp_port     $h3port
