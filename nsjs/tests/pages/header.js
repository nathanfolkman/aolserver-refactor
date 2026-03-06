// Read X-Test request header; echo it back in response header and body
var val = ns.conn.getHeader("X-Test");
ns.conn.setHeader("X-Echo", val !== null ? val : "missing");
ns.conn.write(val !== null ? val : "null");
