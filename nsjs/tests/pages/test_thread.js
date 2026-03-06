/* Tests for ns.thread.* API */
var out = [];

/* id — returns a positive integer */
var tid = ns.thread.id();
out.push(typeof tid === "number" && tid > 0 ? "id:ok" : "id:fail");

/* setName / getName */
ns.thread.setName("jstest-thread");
var name = ns.thread.getName();
out.push(typeof name === "string" ? "getName:ok" : "getName:fail");

/* yield — should not crash */
ns.thread.yield();
out.push("yield:ok");

/* create — fire-and-forget background thread */
ns.shared.set("thread_test", "ran", "no");
var ok = ns.thread.create("ns.sleep(10); ns.shared.set('thread_test','ran','yes');");
out.push(ok === true ? "create:ok" : "create:fail");

/* Brief sleep to allow background thread to start */
ns.sleep(100);

ns.conn.write(out.join(","));
