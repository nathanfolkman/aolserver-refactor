/* Tests for ns.cache.* API */
var cname = "jstest_cache_" + ns.time();
var out = [];

/* create */
ns.cache.create(cname, 1024 * 1024);
out.push("create:ok");

/* set and get */
ns.cache.set(cname, "k1", "hello");
out.push(ns.cache.get(cname, "k1") === "hello" ? "set-get:ok" : "set-get:fail");

/* overwrite */
ns.cache.set(cname, "k1", "world");
out.push(ns.cache.get(cname, "k1") === "world" ? "overwrite:ok" : "overwrite:fail");

/* missing key returns null */
out.push(ns.cache.get(cname, "nokey") === null ? "miss:ok" : "miss:fail");

/* unset */
ns.cache.unset(cname, "k1");
out.push(ns.cache.get(cname, "k1") === null ? "unset:ok" : "unset:fail");

/* flush */
ns.cache.set(cname, "f1", "v1");
ns.cache.set(cname, "f2", "v2");
ns.cache.flush(cname);
out.push(ns.cache.get(cname, "f1") === null ? "flush:ok" : "flush:fail");

/* stats */
ns.cache.set(cname, "s1", "v1");
var stats = ns.cache.stats(cname);
out.push(typeof stats === "object" && stats !== null && stats.entries >= 1 ? "stats:ok" : "stats:fail");

ns.conn.write(out.join(","));
