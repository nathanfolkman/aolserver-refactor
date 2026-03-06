/* Tests for ns.html.hrefs() */
var out = [];

/* basic href extraction */
var html = '<a href="http://example.com/foo">link1</a> text <a href="/bar">link2</a>';
var hrefs = ns.html.hrefs(html);
out.push(Array.isArray(hrefs) ? "array:ok" : "array:fail");
out.push(hrefs.length === 2 ? "count:ok" : "count:fail");
out.push(hrefs[0] === "http://example.com/foo" ? "href0:ok" : "href0:fail");
out.push(hrefs[1] === "/bar" ? "href1:ok" : "href1:fail");

/* no hrefs */
var h2 = ns.html.hrefs("<p>no links here</p>");
out.push(h2.length === 0 ? "empty:ok" : "empty:fail");

/* uppercase A tag */
var h3 = ns.html.hrefs('<A HREF="/upper">text</A>');
out.push(h3.length === 1 && h3[0] === "/upper" ? "upper:ok" : "upper:fail");

/* quoted href */
var h4 = ns.html.hrefs("<a href='single-quote'>x</a>");
out.push(h4.length === 1 && h4[0] === "single-quote" ? "single-quote:ok" : "single-quote:fail");

ns.conn.write(out.join(","));
