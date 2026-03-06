/* Tests for ns.config.section() and ns.config.sections() */
var out = [];

/* sections() returns an array */
var sections = ns.config.sections();
out.push(Array.isArray(sections) ? "sections-array:ok" : "sections-array:fail");
out.push(sections.length > 0 ? "sections-nonempty:ok" : "sections-nonempty:fail");

/* section() for known section returns object */
var sec = ns.config.section("ns/server/server1");
out.push(sec !== null ? "section-found:ok" : "section-found:fail");

/* section() for unknown section returns null */
var s2 = ns.config.section("ns/nonexistent/section/zzz");
out.push(s2 === null ? "section-missing:ok" : "section-missing:fail");

ns.conn.write(out.join(","));
