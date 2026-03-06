/* Tests for extended ns.conn.* response functions */
var out = [];

/* parseHeader — valid header line */
var h = ns.conn.parseHeader("X-Custom-Header: my-value");
out.push(h !== null ? "parseHeader:ok" : "parseHeader:fail");
out.push(h && h.key && h.key.toLowerCase() === "x-custom-header" ? "parseHeader-key:ok" : "parseHeader-key:fail");
out.push(h && h.value === "my-value" ? "parseHeader-value:ok" : "parseHeader-value:fail");

/* parseHeader — invalid (no colon) */
var h2 = ns.conn.parseHeader("invalid-no-colon");
out.push("parseHeader-invalid:ok"); /* non-crashing is the main test */

/* authorize — test that function exists and returns boolean */
var auth = ns.conn.authorize("GET", "/", "", "", "127.0.0.1");
out.push(typeof auth === "boolean" ? "authorize:ok" : "authorize:fail");

/* return(status, type, body) — send a custom response */
ns.conn.return(200, "text/plain", out.join(","));
