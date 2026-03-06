/* Tests for extended ns.shared.* API */
var arr = "shext_" + ns.time();
var out = [];

/* append */
ns.shared.set(arr, "a", "hello");
ns.shared.append(arr, "a", " world");
out.push(ns.shared.get(arr, "a") === "hello world" ? "append:ok" : "append:fail");

/* lappend */
ns.shared.set(arr, "b", "x");
ns.shared.lappend(arr, "b", "y");
out.push(ns.shared.get(arr, "b") === "x y" ? "lappend:ok" : "lappend:fail");

/* names */
var names = ns.shared.names(arr + "*");
out.push(Array.isArray(names) && names.indexOf(arr) >= 0 ? "names:ok" : "names:fail");

/* keys */
var keys = ns.shared.keys(arr);
out.push(Array.isArray(keys) && keys.length >= 2 ? "keys:ok" : "keys:fail");

/* getAll */
var all = ns.shared.getAll(arr);
out.push(typeof all === "object" && all !== null && all["a"] === "hello world" ? "getAll:ok" : "getAll:fail");

ns.conn.write(out.join(","));
