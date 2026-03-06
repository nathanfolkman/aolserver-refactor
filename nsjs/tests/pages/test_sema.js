/* Tests for ns.sema.* API */
var out = [];

/* create with initial count 0 */
var id = ns.sema.create(0);
out.push(typeof id === "number" && id > 0 ? "create:ok" : "create:fail");

/* post(1) then wait — should not block */
ns.sema.post(id, 1);
ns.sema.wait(id);
out.push("post-wait:ok");

/* post multiple */
ns.sema.post(id, 3);
ns.sema.wait(id);
ns.sema.wait(id);
ns.sema.wait(id);
out.push("post3-wait3:ok");

/* create with initial count 1 — wait immediately */
var id2 = ns.sema.create(1);
ns.sema.wait(id2);
out.push("create1-wait:ok");

/* destroy */
ns.sema.destroy(id);
ns.sema.destroy(id2);
out.push("destroy:ok");

ns.conn.write(out.join(","));
