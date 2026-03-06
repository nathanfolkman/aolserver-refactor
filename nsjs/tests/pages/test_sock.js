/* Tests for ns.sock.* API */
var out = [];

/* open a TCP connection to localhost on the test server port */
var sock = ns.sock.open("127.0.0.1", 8080, 3);
out.push(typeof sock === "number" ? "open:ok" : "open:fail");

if (sock >= 0) {
    /* send an HTTP request */
    var req = "GET /test_hello.js HTTP/1.0\r\nHost: localhost\r\n\r\n";
    var n = ns.sock.send(sock, req, 5);
    out.push(n > 0 ? "send:ok" : "send:fail");

    /* receive response */
    var data = ns.sock.recv(sock, 4096, 5);
    out.push(data !== null && data.length > 0 ? "recv:ok" : "recv:fail");
    out.push(data && data.indexOf("HTTP/1") >= 0 ? "recv-http:ok" : "recv-http:fail");

    /* close */
    ns.sock.close(sock);
    out.push("close:ok");
} else {
    out.push("send:skip");
    out.push("recv:skip");
    out.push("recv-http:skip");
    out.push("close:skip");
}

/* setNonBlocking on invalid fd returns false */
out.push(ns.sock.setNonBlocking(-1) === false ? "nonblocking-invalid:ok" : "nonblocking-invalid:fail");

ns.conn.write(out.join(","));
