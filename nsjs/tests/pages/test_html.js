/* Tests for ns.html.* API */
var out = [];

/* quote */
var q = ns.html.quote('<b>Hello & "World"</b>');
out.push(q.indexOf("<b>") === -1 && q.indexOf("&amp;") >= 0 ? "quote:ok" : "quote:fail");

/* strip */
var stripped = ns.html.strip("<b>Hello</b> <i>World</i>");
out.push(stripped === "Hello World" ? "strip:ok" : "strip:fail");

/* strip with nested tags */
var s2 = ns.html.strip("<p class=\"test\">foo</p>");
out.push(s2 === "foo" ? "strip-attr:ok" : "strip-attr:fail");

/* guessType */
out.push(ns.html.guessType("index.html").indexOf("html") >= 0 ? "html-type:ok" : "html-type:fail");
out.push(ns.html.guessType("style.css").indexOf("css") >= 0 ? "css-type:ok" : "css-type:fail");
out.push(typeof ns.html.guessType("file.xyz") === "string" ? "unknown-type:ok" : "unknown-type:fail");

ns.conn.write(out.join(","));
