// Full ns.shared API exercise: set / get / exists / unset / incr
var arr = "test_ops_" + ns.conn.getQuery("run");

// set + get
ns.shared.set(arr, "k1", "hello");
var v1 = ns.shared.get(arr, "k1");

// exists
var e1 = ns.shared.exists(arr, "k1");   // true
var e2 = ns.shared.exists(arr, "k2");   // false (never set)

// unset
ns.shared.unset(arr, "k1");
var e3 = ns.shared.exists(arr, "k1");   // false after unset

// incr
ns.shared.set(arr, "n", "10");
var n1 = ns.shared.incr(arr, "n", 5);  // 15
var n2 = ns.shared.incr(arr, "n", -3); // 12

// get missing key
var vNull = ns.shared.get(arr, "nokey");

ns.conn.write([v1, e1, e2, e3, n1, n2, String(vNull)].join(","));
