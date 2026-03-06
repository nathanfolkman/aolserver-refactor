/* Tests for ns.crypt() */
var out = [];

/* crypt — returns a string */
var h = ns.crypt("password", "ab");
out.push(typeof h === "string" && h.length > 0 ? "crypt:ok" : "crypt:fail");

/* same inputs produce same output */
var h2 = ns.crypt("password", "ab");
out.push(h === h2 ? "crypt-deterministic:ok" : "crypt-deterministic:fail");

/* different inputs produce different output */
var h3 = ns.crypt("other", "ab");
out.push(h !== h3 ? "crypt-distinct:ok" : "crypt-distinct:fail");

ns.conn.write(out.join(","));
