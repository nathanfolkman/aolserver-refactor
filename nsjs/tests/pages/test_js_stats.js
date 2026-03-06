/*
 * test_js_stats.js — exercise ns.js.stats API
 *
 * Query param ?action=global|scripts|reset selects what to return.
 */
var action = ns.conn.getQuery("action") || "global";

if (action === "global") {
    var g = ns.js.stats.global();
    ns.conn.setHeader("Content-Type", "application/json");
    ns.conn.write(JSON.stringify(g));
} else if (action === "scripts") {
    var s = ns.js.stats.scripts();
    ns.conn.setHeader("Content-Type", "application/json");
    ns.conn.write(JSON.stringify(s));
} else if (action === "reset") {
    ns.js.stats.reset();
    ns.conn.write("reset");
} else {
    ns.conn.write("unknown action");
}
