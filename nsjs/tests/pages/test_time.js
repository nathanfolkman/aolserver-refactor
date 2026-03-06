/* Tests for ns.time* API */
var out = [];

var t = ns.time();
out.push(typeof t === "number" && t > 1000000000 ? "now:ok" : "now:fail");

/* format */
var fmt = ns.time.format(t, "%Y");
out.push(typeof fmt === "string" && fmt.length === 4 ? "format:ok" : "format:fail");

/* httpTime */
var ht = ns.time.httpTime(t);
out.push(typeof ht === "string" && ht.length > 10 ? "httpTime:ok" : "httpTime:fail");

/* parseHttpTime round-trip */
var t2 = ns.time.parseHttpTime(ht);
out.push(typeof t2 === "number" ? "parseHttpTime:ok" : "parseHttpTime:fail");

/* parseHttpTime invalid */
out.push(ns.time.parseHttpTime("not-a-date") === null ? "parseInvalid:ok" : "parseInvalid:fail");

/* gmtime */
var gm = ns.time.gmtime(t);
out.push(typeof gm === "object" && gm !== null && typeof gm.year === "number" && gm.year >= 2024 ? "gmtime:ok" : "gmtime:fail");

/* localtime */
var lc = ns.time.localtime(t);
out.push(typeof lc === "object" && lc !== null && typeof lc.year === "number" ? "localtime:ok" : "localtime:fail");

ns.conn.write(out.join(","));
