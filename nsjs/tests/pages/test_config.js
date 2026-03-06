/* Tests for ns.config* API */
var out = [];

/* Read a known config value — server name */
var srvName = ns.config("ns/servers", "server1");
out.push(srvName !== null ? "config:ok" : "config:null");

/* Default value when key missing */
var missing = ns.config("ns/servers", "no_such_key", "default_val");
out.push(missing === "default_val" ? "default:ok" : "default:fail");

/* configInt */
var port = ns.configInt("ns/server/server1/module/nssock", "port", 0);
out.push(typeof port === "number" && port > 0 ? "configInt:ok" : "configInt:fail");

/* configBool */
var logDebug = ns.configBool("ns/parameters", "logdebug", false);
out.push(typeof logDebug === "boolean" ? "configBool:ok" : "configBool:fail");

ns.conn.write(out.join(","));
