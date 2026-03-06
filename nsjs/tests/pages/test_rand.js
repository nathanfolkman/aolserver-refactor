/* Tests for ns.rand() */
var out = [];

/* no arg -> float in [0, 1) */
var r = ns.rand();
out.push((typeof r === "number" && r >= 0 && r < 1) ? "float:ok" : "float:fail");

/* with max -> integer in [0, max) */
var results_ok = true;
for (var i = 0; i < 20; i++) {
    var n = ns.rand(100);
    if (typeof n !== "number" || n < 0 || n >= 100 || n !== Math.floor(n)) {
        results_ok = false;
        break;
    }
}
out.push(results_ok ? "range:ok" : "range:fail");

ns.conn.write(out.join(","));
