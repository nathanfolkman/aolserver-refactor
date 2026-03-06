/* Tests for ns.http.get() */
var out = [];

/* internal path — fetch a known page from the test server */
var r = ns.http.get("/test_hello.js");
out.push(r !== null && typeof r === "object" ? "get-internal:ok" : "get-internal:fail");
out.push(r && typeof r.body === "string" ? "get-body:ok" : "get-body:fail");
out.push(r && typeof r.headers === "object" ? "get-headers:ok" : "get-headers:fail");

/* missing internal page */
var r2 = ns.http.get("/nsjs_nonexistent_page_zzz.js");
/* May return null or object with empty body */
out.push("get-missing:ok"); /* just checking no crash */

ns.conn.write(out.join(","));
