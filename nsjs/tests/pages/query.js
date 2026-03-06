var name = ns.conn.getQuery("name");
var missing = ns.conn.getQuery("nokey");
ns.conn.write(name !== null ? name : "null");
ns.conn.write("|");
ns.conn.write(missing !== null ? missing : "null");
