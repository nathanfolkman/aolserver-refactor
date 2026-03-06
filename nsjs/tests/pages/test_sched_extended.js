/* Tests for extended ns.sched.* API: daily, weekly, pause, resume */
var out = [];

/* daily — schedule at 3am tomorrow */
var did = ns.sched.daily(3, 0, "ns.shared.set('sched_ext','daily','fired');");
out.push(typeof did === "number" ? "daily:ok" : "daily:fail");

/* weekly — schedule Sunday 3am */
var wid = ns.sched.weekly(0, 3, 0, "ns.shared.set('sched_ext','weekly','fired');");
out.push(typeof wid === "number" ? "weekly:ok" : "weekly:fail");

/* pause the daily job */
var paused = ns.sched.pause(did);
out.push(paused === true ? "pause:ok" : "pause:fail");

/* resume it */
var resumed = ns.sched.resume(did);
out.push(resumed === true ? "resume:ok" : "resume:fail");

/* cancel both */
ns.sched.cancel(did);
ns.sched.cancel(wid);
out.push("cancel:ok");

ns.conn.write(out.join(","));
