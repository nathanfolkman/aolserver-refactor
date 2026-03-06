/* Tests for ns.process.kill() */
var out = [];

/* Kill with signal 0 just checks if the process exists — returns true for own pid */
var pid = ns.info.pid();
out.push(typeof pid === "number" && pid > 0 ? "pid-valid:ok" : "pid-valid:fail");

/* Signal 0 to ourselves should succeed */
out.push(ns.process.kill(pid, 0) === true ? "kill-sig0:ok" : "kill-sig0:fail");

/* Killing a non-existent pid should return false */
out.push(ns.process.kill(99999999, 0) === false ? "kill-nonexist:ok" : "kill-nonexist:fail");

ns.conn.write(out.join(","));
