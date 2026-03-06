/* Tests for ns.info.* API */
var out = [];

out.push(typeof ns.info.version() === "string" ? "version:ok" : "version:fail");
out.push(typeof ns.info.uptime() === "number" ? "uptime:ok" : "uptime:fail");
out.push(typeof ns.info.pageroot() === "string" ? "pageroot:ok" : "pageroot:fail");
out.push(typeof ns.info.log() === "string" ? "log:ok" : "log:fail");
out.push(typeof ns.info.config() === "string" ? "config:ok" : "config:fail");
out.push(typeof ns.info.hostname() === "string" ? "hostname:ok" : "hostname:fail");
out.push(typeof ns.info.address() === "string" ? "address:ok" : "address:fail");
out.push(typeof ns.info.pid() === "number" && ns.info.pid() > 0 ? "pid:ok" : "pid:fail");
out.push(typeof ns.info.serverName() === "string" && ns.info.serverName().length > 0 ? "serverName:ok" : "serverName:fail");

ns.conn.write(out.join(","));
