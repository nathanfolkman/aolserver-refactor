/* Tests for ns.atshutdown() and ns.atsignal() */
var out = [];

/* Register a shutdown callback — just verify it doesn't crash */
try {
    ns.atshutdown('ns.log("Notice", "jscp atshutdown test fired");');
    out.push("atshutdown:ok");
} catch(e) {
    out.push("atshutdown:fail");
}

/* Register a signal callback */
try {
    ns.atsignal('ns.log("Notice", "jscp atsignal test fired");');
    out.push("atsignal:ok");
} catch(e) {
    out.push("atsignal:fail");
}

ns.conn.write(out.join(","));
