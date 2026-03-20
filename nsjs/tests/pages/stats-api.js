/*
 * stats-api.js — JSON endpoint for live polling by the dashboard client
 *
 * Returns the same data structure as stats.jsadp but as JSON.
 * Used by the 5-second polling interval in client-entry.jsx.
 */

/* --- Basic Auth check --- */
var cfgSection  = "ns/server/" + ns.info.serverName() + "/module/nsjs";
var cfgUser     = ns.config(cfgSection, "stats_user",     "nsadmin");
var cfgPass     = ns.config(cfgSection, "stats_password", "");

var authUser   = ns.conn.getAuthUser();
var authPasswd = ns.conn.getAuthPasswd();

if (authUser !== cfgUser || authPasswd !== cfgPass) {
    ns.conn.setHeader("WWW-Authenticate", 'Basic realm="AOLserver Stats"');
    ns.conn.setContentType("application/json");
    ns.conn.returnHtml(401, JSON.stringify({ error: "Unauthorized" }));
} else {
    var data = {
        info: {
            serverName: ns.info.serverName(),
            version:    ns.info.version(),
            uptime:     ns.info.uptime(),
            boottime:   ns.info.boottime(),
            pid:        ns.info.pid(),
            hostname:   ns.info.hostname(),
            address:    ns.info.address(),
            pageroot:   ns.info.pageroot(),
            platform:   ns.info.platform(),
            buildDate:  ns.info.buildDate(),
        },
        jsStats:   ns.js.stats.global(),
        memory:    ns.memory.stats(),
        server: {
            threads:     ns.server.threads(),
            active:      ns.server.active(),
            queued:      ns.server.queued(),
            waiting:     ns.server.waiting(),
            keepalive:   ns.server.keepalive(),
            connections: ns.server.connections(),
            pools:       ns.server.pools(),
            urlstats:    ns.server.urlstats(),
        },
        drivers:   ns.driver.list(),
        http2:     ns.http2.stats(),
        locks:     ns.info.locks(),
        threads:   ns.info.threads(),
        scheduled: ns.info.scheduled(),
        callbacks: ns.info.callbacks(),
        caches:    (function() {
            var raw = ns.cache.statsAll();
            var out = {};
            for (var name in raw) {
                var s = raw[name]; var c = {};
                for (var k in s) { c[k.replace(/:$/, "")] = s[k]; }
                out[name] = c;
            }
            return out;
        })(),
        adpStats:          ns.adp.stats(),
        sockcallbacks:     ns.info.sockcallbacks(),
        logTail:           ns.log.tail(8192),
        memorySizeClasses: ns.memory.sizeClasses(),
    };

    ns.conn.setContentType("application/json");
    ns.conn.returnHtml(200, JSON.stringify(data));
}
