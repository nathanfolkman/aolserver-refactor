/* Tests for ns.html.* API */
var out = [];

/* quote */
var q = ns.html.quote('<b>Hello & "World"</b>');
out.push(q.indexOf("<b>") === -1 && q.indexOf("&amp;") >= 0 ? "quote:ok" : "quote:fail");

/* guessType */
out.push(ns.html.guessType("index.html").indexOf("html") >= 0 ? "html-type:ok" : "html-type:fail");
out.push(ns.html.guessType("style.css").indexOf("css") >= 0 ? "css-type:ok" : "css-type:fail");
out.push(typeof ns.html.guessType("file.xyz") === "string" ? "unknown-type:ok" : "unknown-type:fail");

ns.conn.write(out.join(","));
