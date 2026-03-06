/* Tests for ns.url.* API */
var out = [];

/* encode/decode round-trip */
var s = "hello world & foo=bar";
var enc = ns.url.encode(s);
out.push(enc.indexOf(" ") === -1 ? "encode:ok" : "encode:fail");
var dec = ns.url.decode(enc);
out.push(dec === s ? "decode:ok" : "decode:fail");

/* parse */
var q = ns.url.parse("name=alice&age=30&city=New+York");
out.push(q.name === "alice" ? "parse-name:ok" : "parse-name:fail");
out.push(q.age === "30" ? "parse-age:ok" : "parse-age:fail");

/* toFile — maps a URL to file path */
var fp = ns.url.toFile("/test_url.js");
out.push(typeof fp === "string" && fp.length > 0 ? "toFile:ok" : "toFile:fail");

ns.conn.write(out.join(","));
