/* Tests for extended ns.conn.* API */
var out = [];

out.push(typeof ns.conn.getPeerAddr() === "string" ? "peer:ok" : "peer:fail");
out.push(typeof ns.conn.getHost() === "string" ? "host:ok" : "host:fail");
out.push(typeof ns.conn.getPort() === "number" ? "port:ok" : "port:fail");
out.push(typeof ns.conn.getId() === "number" ? "id:ok" : "id:fail");

/* auth user/passwd may be null (no auth header sent) */
var au = ns.conn.getAuthUser();
out.push(au === null || typeof au === "string" ? "authuser:ok" : "authuser:fail");

/* getAllHeaders returns object */
var hdrs = ns.conn.getAllHeaders();
out.push(typeof hdrs === "object" && hdrs !== null ? "allhdrs:ok" : "allhdrs:fail");

/* getAllQuery returns object */
var qry = ns.conn.getAllQuery();
out.push(typeof qry === "object" && qry !== null ? "allqry:ok" : "allqry:fail");

/* getContent returns string (may be empty for GET) */
var body = ns.conn.getContent();
out.push(body === null || typeof body === "string" ? "content:ok" : "content:fail");

/* setStatus doesn't crash */
ns.conn.setStatus(200);
out.push("status:ok");

/* setContentType doesn't crash */
ns.conn.setContentType("text/plain");
out.push("ctype:ok");

/* location() returns a string like "http://host:port" */
var loc = ns.conn.location();
out.push(typeof loc === "string" && loc.length > 0 ? "location:ok" : "location:fail");

ns.conn.write(out.join(","));
