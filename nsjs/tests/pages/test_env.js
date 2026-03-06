/* Tests for ns.env.* API */
var out = [];

/* set an env variable */
out.push(ns.env.set("NS_JS_TEST_VAR", "hello42") === true ? "set:ok" : "set:fail");

/* get it back */
out.push(ns.env.get("NS_JS_TEST_VAR") === "hello42" ? "get:ok" : "get:fail");

/* names includes our variable */
var names = ns.env.names();
out.push(Array.isArray(names) && names.indexOf("NS_JS_TEST_VAR") >= 0 ? "names:ok" : "names:fail");

/* unset */
out.push(ns.env.unset("NS_JS_TEST_VAR") === true ? "unset:ok" : "unset:fail");

/* get after unset returns null */
out.push(ns.env.get("NS_JS_TEST_VAR") === null ? "get-null:ok" : "get-null:fail");

/* get non-existent */
out.push(ns.env.get("__NS_JS_NONEXISTENT__") === null ? "get-missing:ok" : "get-missing:fail");

ns.conn.write(out.join(","));
