/* Tests for ns.sched.* API.
 * after() and interval() schedule JS execution on a background thread.
 * We verify the IDs are returned and cancel works without crashing.
 */
var out = [];

/* after — should return a valid schedule ID */
var sid = ns.sched.after(3600, "ns.shared.set('sched_test','fired','yes');");
out.push(typeof sid === "number" ? "after:ok" : "after:fail");

/* cancel the scheduled job immediately (no side effects) */
ns.sched.cancel(sid);
out.push("cancel:ok");

/* interval — recurring schedule */
var iid = ns.sched.interval(3600, "ns.shared.incr('sched_test','ticks',1);");
out.push(typeof iid === "number" ? "interval:ok" : "interval:fail");

/* cancel interval */
ns.sched.cancel(iid);
out.push("cancel-interval:ok");

ns.conn.write(out.join(","));
