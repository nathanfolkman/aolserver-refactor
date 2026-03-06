/* Tests for ns.dns.* API */
var out = [];

/* addrByHost for localhost */
var ip = ns.dns.addrByHost("localhost");
out.push(ip !== null && typeof ip === "string" ? "addrByHost:ok" : "addrByHost:fail");

/* hostByAddr for 127.0.0.1 */
var host = ns.dns.hostByAddr("127.0.0.1");
out.push(host !== null && typeof host === "string" ? "hostByAddr:ok" : "hostByAddr:fail");

/* addrByHost for invalid */
var bad = ns.dns.addrByHost("invalid.domain.that.does.not.exist.nsjs");
out.push(bad === null ? "addrByHost-miss:ok" : "addrByHost-miss:fail");

ns.conn.write(out.join(","));
