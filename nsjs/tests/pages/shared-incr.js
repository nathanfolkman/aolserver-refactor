// Atomically increment stats.hits and return the new value.
var v = ns.shared.incr("test_hits", "count", 1);
ns.conn.write(String(v));
