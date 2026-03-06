/* Tests for ns.file.* API (operates in /tmp) */
var out = [];

/* tmpnam */
var tmp = ns.file.tmpnam();
out.push(typeof tmp === "string" && tmp.length > 0 ? "tmpnam:ok" : "tmpnam:fail");

/* write + exists + read */
var path = tmp + "_test.txt";
out.push(ns.file.write(path, "hello nsjs") === true ? "write:ok" : "write:fail");
out.push(ns.file.exists(path) === true ? "exists:ok" : "exists:fail");
out.push(ns.file.read(path) === "hello nsjs" ? "read:ok" : "read:fail");

/* stat */
var st = ns.file.stat(path);
out.push(st !== null && st.isFile === true && st.size === 10 ? "stat:ok" : "stat:fail");

/* cp */
var path2 = tmp + "_copy.txt";
out.push(ns.file.cp(path, path2) === true ? "cp:ok" : "cp:fail");
out.push(ns.file.read(path2) === "hello nsjs" ? "cp-read:ok" : "cp-read:fail");

/* rename */
var path3 = tmp + "_renamed.txt";
out.push(ns.file.rename(path2, path3) === true ? "rename:ok" : "rename:fail");
out.push(ns.file.exists(path3) === true ? "rename-exists:ok" : "rename-exists:fail");

/* mkdir + rmdir */
var dir = tmp + "_dir";
out.push(ns.file.mkdir(dir) === true ? "mkdir:ok" : "mkdir:fail");
out.push(ns.file.stat(dir) !== null && ns.file.stat(dir).isDir ? "mkdir-isDir:ok" : "mkdir-isDir:fail");
out.push(ns.file.rmdir(dir) === true ? "rmdir:ok" : "rmdir:fail");

/* unlink */
out.push(ns.file.unlink(path) === true ? "unlink:ok" : "unlink:fail");
ns.file.unlink(path3);

/* normalizePath */
var np = ns.file.normalizePath("/tmp/../tmp/foo");
out.push(typeof np === "string" && np.indexOf("..") === -1 ? "normPath:ok" : "normPath:fail");

/* exists on missing file */
out.push(ns.file.exists("/tmp/no_such_file_nsjs_xyz") === false ? "notexists:ok" : "notexists:fail");

ns.conn.write(out.join(","));
