/* Tests for ns.mutex.* API */
var out = [];

/* create */
var id = ns.mutex.create();
out.push(typeof id === "number" && id > 0 ? "create:ok" : "create:fail");

/* trylock (should succeed when unlocked) */
out.push(ns.mutex.trylock(id) === true ? "trylock:ok" : "trylock:fail");

/* unlock */
ns.mutex.unlock(id);
out.push("unlock:ok");

/* lock + unlock */
ns.mutex.lock(id);
ns.mutex.unlock(id);
out.push("lock-unlock:ok");

/* trylock after unlock (should succeed again) */
out.push(ns.mutex.trylock(id) === true ? "trylock2:ok" : "trylock2:fail");
ns.mutex.unlock(id);

/* destroy */
ns.mutex.destroy(id);
out.push("destroy:ok");

ns.conn.write(out.join(","));
