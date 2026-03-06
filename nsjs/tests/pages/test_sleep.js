/* Tests for ns.sleep() */
var out = [];

/* sleep 0 ms — should not crash */
ns.sleep(0);
out.push("sleep0:ok");

/* sleep a small amount */
ns.sleep(5);
out.push("sleep5:ok");

ns.conn.write(out.join(","));
