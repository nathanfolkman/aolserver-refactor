/* Tests for ns.set.* API */
var out = [];

/* create */
var id = ns.set.create("testset");
out.push(typeof id === "number" && id > 0 ? "create:ok" : "create:fail");

/* put */
var i0 = ns.set.put(id, "key1", "val1");
var i1 = ns.set.put(id, "key2", "val2");
var i2 = ns.set.put(id, "Key1", "val1upper"); /* different case */
out.push(i0 >= 0 && i1 >= 0 && i2 >= 0 ? "put:ok" : "put:fail");

/* size */
out.push(ns.set.size(id) === 3 ? "size:ok" : "size:fail");

/* get */
out.push(ns.set.get(id, "key1") === "val1" ? "get:ok" : "get:fail");
out.push(ns.set.get(id, "missing") === null ? "get-missing:ok" : "get-missing:fail");

/* iget (case-insensitive) */
out.push(ns.set.iget(id, "KEY1") === "val1" ? "iget:ok" : "iget:fail");

/* find */
out.push(ns.set.find(id, "key2") === 1 ? "find:ok" : "find:fail");
out.push(ns.set.find(id, "missing") === -1 ? "find-missing:ok" : "find-missing:fail");

/* key/value by index */
out.push(ns.set.key(id, 0) === "key1" ? "key:ok" : "key:fail");
out.push(ns.set.value(id, 0) === "val1" ? "value:ok" : "value:fail");

/* update */
ns.set.update(id, "key1", "updated");
out.push(ns.set.get(id, "key1") === "updated" ? "update:ok" : "update:fail");

/* toObject */
var obj = ns.set.toObject(id);
out.push(typeof obj === "object" && obj !== null ? "toObject:ok" : "toObject:fail");
out.push(obj["key2"] === "val2" ? "toObject-val:ok" : "toObject-val:fail");

/* delete by index */
ns.set.delete(id, 0);
out.push(ns.set.size(id) === 2 ? "delete:ok" : "delete:fail");

/* free */
ns.set.free(id);
out.push("free:ok");

ns.conn.write(out.join(","));
