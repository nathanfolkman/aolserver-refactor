/* Tests for ns.cond.* API */
var out = [];

/* create cond + mutex */
var cid = ns.cond.create();
out.push(typeof cid === "number" && cid > 0 ? "create:ok" : "create:fail");

var mid = ns.mutex.create();
out.push(typeof mid === "number" && mid > 0 ? "mutex-create:ok" : "mutex-create:fail");

/* signal (no waiters — should not crash) */
ns.cond.signal(cid);
out.push("signal:ok");

/* broadcast (no waiters) */
ns.cond.broadcast(cid);
out.push("broadcast:ok");

/* timedWait with locked mutex — should timeout and return false */
ns.mutex.lock(mid);
var ok = ns.cond.timedWait(cid, mid, 0); /* 0 second timeout */
ns.mutex.unlock(mid);
out.push(typeof ok === "boolean" ? "timedWait:ok" : "timedWait:fail");

/* destroy */
ns.cond.destroy(cid);
ns.mutex.destroy(mid);
out.push("destroy:ok");

ns.conn.write(out.join(","));
