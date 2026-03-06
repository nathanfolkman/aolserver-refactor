/* Tests for ns.rwlock.* API */
var out = [];

/* create */
var id = ns.rwlock.create();
out.push(typeof id === "number" && id > 0 ? "create:ok" : "create:fail");

/* readLock + unlock */
ns.rwlock.readLock(id);
ns.rwlock.unlock(id);
out.push("readLock:ok");

/* writeLock + unlock */
ns.rwlock.writeLock(id);
ns.rwlock.unlock(id);
out.push("writeLock:ok");

/* multiple readers */
ns.rwlock.readLock(id);
ns.rwlock.unlock(id);
out.push("multiRead:ok");

/* destroy */
ns.rwlock.destroy(id);
out.push("destroy:ok");

ns.conn.write(out.join(","));
