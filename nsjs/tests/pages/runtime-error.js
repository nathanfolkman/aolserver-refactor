ns.conn.write("before");
throw new Error("intentional runtime error");
ns.conn.write("after");
