/* Tests for extended ns.file.* API: chmod, link, symlink, truncate, roll */
var out = [];
var tmp = "/tmp/nsjs_file_ext_test_" + (new Date().getTime());

/* Create a file to work with */
ns.file.write(tmp, "hello world");

/* chmod — make read-only */
out.push(ns.file.chmod(tmp, 0444) === true ? "chmod:ok" : "chmod:fail");

/* chmod — restore writable */
out.push(ns.file.chmod(tmp, 0644) === true ? "chmod-restore:ok" : "chmod-restore:fail");

/* link — create hard link */
var link_path = tmp + ".link";
out.push(ns.file.link(tmp, link_path) === true ? "link:ok" : "link:fail");
out.push(ns.file.exists(link_path) === true ? "link-exists:ok" : "link-exists:fail");

/* symlink — create symbolic link */
var sym_path = tmp + ".sym";
out.push(ns.file.symlink(tmp, sym_path) === true ? "symlink:ok" : "symlink:fail");
out.push(ns.file.exists(sym_path) === true ? "symlink-exists:ok" : "symlink-exists:fail");

/* truncate — reduce file size */
out.push(ns.file.truncate(tmp, 5) === true ? "truncate:ok" : "truncate:fail");
var st = ns.file.stat(tmp);
out.push(st && st.size === 5 ? "truncate-size:ok" : "truncate-size:fail");

/* cleanup */
ns.file.unlink(tmp);
ns.file.unlink(link_path);
ns.file.unlink(sym_path);

ns.conn.write(out.join(","));
