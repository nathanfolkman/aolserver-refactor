// JS globals must NOT persist across requests (fresh context per request).
// This variable is declared and incremented; if context leaked between
// requests the value would grow.  It must always be 1.
var counter = 0;
counter++;
ns.conn.write(String(counter));
