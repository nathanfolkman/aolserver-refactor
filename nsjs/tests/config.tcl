#
# nsjs test server configuration.
#
# Usage: nsd -ft config.tcl
#
# Required env vars (set by run_tests.py or CTest):
#   NSJS_INSTALL   — AOLserver install prefix (e.g. /tmp/aolserver-test)
#   NSJS_PORT      — port to listen on (default 8765)
#   NSJS_PAGES     — directory containing test .js/.jsadp pages
#

set home    $::env(NSJS_INSTALL)
set port    [expr {[info exists ::env(NSJS_PORT)] ? $::env(NSJS_PORT) : 8765}]
set pages   $::env(NSJS_PAGES)

file mkdir $home/servers/server1/modules/nslog

ns_section "ns/parameters"
    ns_param home     $home
    ns_param logdebug false

ns_section "ns/servers"
    ns_param server1 "nsjs-test"

ns_section "ns/server/server1"
    ns_param pageroot    $pages
    ns_param maxthreads  10
    ns_param minthreads  2

ns_section "ns/server/server1/modules"
    ns_param nssock  nssock.so
    ns_param nslog   nslog.so
    ns_param nsjs    nsjs.so

ns_section "ns/server/server1/module/nssock"
    ns_param hostname 127.0.0.1
    ns_param address  127.0.0.1
    ns_param port     $port

ns_section "ns/server/server1/module/nslog"
    ns_param rolllog false

set jscp_port [expr {[info exists ::env(NSJS_JSCP_PORT)] ? $::env(NSJS_JSCP_PORT) : 9090}]

ns_section "ns/server/server1/module/nsjs"
    ns_param jscp_address         127.0.0.1
    ns_param jscp_port            $jscp_port
    ns_param jscp_users           "admin:secret"
    ns_param jscp_log             false
    ns_param jscp_max_sessions    5
    ns_param js_script_cache      true
    ns_param js_cache_stat_always true
