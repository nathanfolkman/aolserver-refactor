# nsjs — Server-Side JavaScript for AOLserver

`nsjs` is a V8-backed AOLserver module that executes `.js` and `.jsadp`
files as server-side JavaScript.  It exposes the full AOLserver C API
through a `ns` object available in every script.

---

## Contents

1. [Prerequisites](#prerequisites)
2. [Building](#building)
3. [Server configuration](#server-configuration)
4. [Running](#running)
5. [Testing](#testing)
6. [JavaScript API reference](#javascript-api-reference)
7. [jscp — JavaScript control port](#jscp--javascript-control-port)

---

## Prerequisites

| Requirement | Version | Notes |
|---|---|---|
| CMake | 3.20+ | |
| C++20 compiler | clang 15+ / gcc 12+ | required by V8 14 headers |
| V8 | 14.x | `brew install v8` (macOS) or `libv8-dev` (Linux) |
| Tcl | 8.6+ | built automatically via `NS_BUILD_DEPS=ON` |
| Python 3 | 3.9+ | test runner only |

---

## Building

### macOS (Homebrew)

```sh
# Install V8
brew install v8

# Configure — deps (Tcl, OpenSSL, gperftools) are downloaded and built automatically
cmake -B build \
  -DCMAKE_INSTALL_PREFIX=/tmp/aolserver-test \
  -DNS_BUILD_DEPS=ON \
  -DNS_WITH_V8=ON \
  -DV8_ROOT=$(brew --prefix v8) \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --parallel
cmake --install build
```

`NS_WITH_V8=ON` enables the `nsjs` target.  `V8_ROOT` is optional on
macOS when Homebrew is installed — `cmake/FindV8.cmake` runs
`brew --prefix v8` automatically.

### Linux

```sh
# Debian/Ubuntu
sudo apt install libv8-dev

cmake -B build \
  -DCMAKE_INSTALL_PREFIX=/opt/aolserver \
  -DNS_BUILD_DEPS=ON \
  -DNS_WITH_V8=ON \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --parallel
cmake --install build
```

On Linux `FindV8.cmake` falls back to `pkg-config v8`.  If that is
unavailable, set `-DV8_ROOT=/path/to/v8`.

### Build output

```
<prefix>/bin/nsd          # server binary
<prefix>/bin/nsjs.so      # JavaScript module
<prefix>/lib/libnsd.dylib # (or .so on Linux)
```

---

## Server configuration

Add the module to your `config.tcl`:

```tcl
ns_section "ns/server/myserver/modules"
    ns_param nsjs  nsjs.so

# Optional: jscp control port (see below)
ns_section "ns/server/myserver/module/nsjs"
    ns_param jscp_address      127.0.0.1
    ns_param jscp_port         9090
    ns_param jscp_users        "admin:secret ops:hunter2"
    ns_param jscp_log          false
    ns_param jscp_max_sessions 5
```

Place `.js` and `.jsadp` files in the server's page root.  They are
served in response to `GET`, `HEAD`, and `POST` requests whose URLs
match `/*.js` or `/*.jsadp`.

---

## Running

V8's shared libraries must be visible at startup.

### macOS

```sh
export DYLD_LIBRARY_PATH=/tmp/aolserver-test/lib:$(brew --prefix v8)/lib
/tmp/aolserver-test/bin/nsd -ft config.tcl
```

### Linux

```sh
export LD_LIBRARY_PATH=/opt/aolserver/lib
/opt/aolserver/bin/nsd -ft config.tcl
```

### .jsadp templates

Files with the `.jsadp` extension are template files.  Static HTML
outside `<% ... %>` blocks is emitted verbatim; code inside the blocks
is executed as JavaScript.

```html
<html><body>
<p>Server time: <%= new Date(ns.time() * 1000).toUTCString() %></p>
<p>Page root: <%= ns.info.pageroot() %></p>
</body></html>
```

---

## Testing

The integration test suite starts a real `nsd` instance and issues HTTP
requests against it.

```sh
cd nsjs/tests

# Basic run (uses NSJS_INSTALL env or defaults to /tmp/aolserver-test)
NSJS_INSTALL=/tmp/aolserver-test python3 test_nsjs.py -v

# Override port (useful if 8765 is in use)
NSJS_INSTALL=/tmp/aolserver-test NSJS_PORT=18765 NSJS_JSCP_PORT=19090 \
  python3 test_nsjs.py -v

# If the server is already running, skip lifecycle management
python3 test_nsjs.py --no-server -v
```

| Env variable | Default | Purpose |
|---|---|---|
| `NSJS_INSTALL` | `/tmp/aolserver-test` | AOLserver install prefix |
| `NSJS_PORT` | `8765` | HTTP listen port |
| `NSJS_JSCP_PORT` | `9090` | jscp listen port |
| `NSJS_V8_LIB` | auto-detected | Path to V8 dylib directory |

### CTest

```sh
ctest --test-dir build -L nsjs -V
```

---

## JavaScript API reference

Every script runs with an `ns` object in the global scope.  `conn`
methods that require an active HTTP connection return `null` when
called from a background context (scheduled callbacks, jscp).

### `ns.conn.*` — request / response

| Method | Description |
|---|---|
| `ns.conn.write(str)` | Append to response body |
| `ns.conn.getHeader(name)` | Request header value or `null` |
| `ns.conn.setHeader(name, val)` | Set response header |
| `ns.conn.getQuery(name)` | Query-string parameter or `null` |
| `ns.conn.getAllHeaders()` | All request headers as object |
| `ns.conn.getAllQuery()` | All query params as object |
| `ns.conn.getContent()` | Request body as string |
| `ns.conn.getMethod()` | HTTP method (`"GET"`, `"POST"`, …) |
| `ns.conn.getUrl()` | Request URL path |
| `ns.conn.getPeerAddr()` | Client IP address string |
| `ns.conn.location()` | Origin URL e.g. `"http://host:8080"` |
| `ns.conn.getHost()` | Server hostname |
| `ns.conn.getPort()` | Server port number |
| `ns.conn.getId()` | Connection ID integer |
| `ns.conn.getAuthUser()` | HTTP Basic auth username or `null` |
| `ns.conn.getAuthPasswd()` | HTTP Basic auth password or `null` |
| `ns.conn.setStatus(code)` | Set HTTP status code |
| `ns.conn.setContentType(type)` | Set `Content-Type` |
| `ns.conn.returnHtml(status, html)` | Send HTML response and stop |
| `ns.conn.returnFile(status, type, path)` | Send file response |
| `ns.conn.returnRedirect(url)` | HTTP 302 redirect |
| `ns.conn.return(status, type, body)` | Generic response |
| `ns.conn.returnBadRequest([reason])` | 400 |
| `ns.conn.returnForbidden()` | 403 |
| `ns.conn.returnNotFound()` | 404 |
| `ns.conn.returnUnauthorized()` | 401 |
| `ns.conn.returnNotice(status, title, msg)` | Formatted notice page |
| `ns.conn.returnError(status, title, body)` | Formatted error page |
| `ns.conn.headers(status, type, len)` | Flush headers only |
| `ns.conn.startContent()` | Begin chunked body |
| `ns.conn.respond(status, headers, body)` | Full response from object |
| `ns.conn.internalRedirect(url)` | Internal URL re-dispatch |
| `ns.conn.parseHeader(str)` | Parse a raw header string |
| `ns.conn.authorize(authType, realm)` | Trigger auth challenge |
| `ns.conn.close()` | Close the connection |

### `ns.shared.*` — cross-thread shared store

Named string arrays shared across all threads and requests.

```js
ns.shared.set("counters", "hits", "0");
var n = ns.shared.incr("counters", "hits", 1);
ns.conn.write("hits: " + n);
```

| Method | Description |
|---|---|
| `set(array, key, val)` | Set a key |
| `get(array, key)` | Get a key or `null` |
| `exists(array, key)` | `true` / `false` |
| `unset(array, key)` | Delete a key |
| `incr(array, key, delta)` | Atomic integer increment |
| `append(array, key, str)` | Append string to value |
| `lappend(array, key, item)` | Append space-separated list item |
| `names([pattern])` | Array names matching glob |
| `keys(array)` | All keys in an array |
| `getAll(array)` | All key/value pairs as object |

### `ns.cache.*` — named caches

```js
ns.cache.create("mycache", 1024 * 1024);
ns.cache.set("mycache", "key", "value");
var v = ns.cache.get("mycache", "key");
```

| Method | Description |
|---|---|
| `create(name, maxBytes)` | Create a cache |
| `get(name, key)` | Retrieve or `null` |
| `set(name, key, val)` | Store |
| `unset(name, key)` | Delete |
| `flush(name)` | Clear all entries |
| `stats(name)` | `{name, entries}` object |

### `ns.config` — configuration access

```js
var port = ns.config("ns/server/myserver/module/nssock", "port");
var dbHost = ns.config("ns/db", "host", "localhost");  // with default
var n = ns.configInt("ns/server/myserver", "maxthreads", 10);
var debug = ns.configBool("ns/parameters", "logdebug", false);
```

`ns.config.section(name)` returns all key/value pairs in a section as
an object. `ns.config.sections()` returns an array of section names.

### `ns.info.*` — server information

| Property | Type | Description |
|---|---|---|
| `ns.info.version()` | string | AOLserver version string |
| `ns.info.serverName()` | string | Virtual server name |
| `ns.info.uptime()` | number | Seconds since server start |
| `ns.info.pageroot()` | string | Page root filesystem path |
| `ns.info.log()` | string | Error log path |
| `ns.info.config()` | string | Config file path |
| `ns.info.hostname()` | string | System hostname |
| `ns.info.address()` | string | Server IP address |
| `ns.info.pid()` | number | Server process ID |

### `ns.time` — time utilities

```js
var t = ns.time();                          // epoch seconds (integer)
ns.time.format(t, "%Y-%m-%d %H:%M:%S");    // strftime
ns.time.httpTime(t);                        // RFC 1123 HTTP date
ns.time.parseHttpTime("Fri, 06 Mar 2026 12:00:00 GMT");  // -> epoch
ns.time.gmtime(t);   // {year,month,day,hour,min,sec,wday,yday}
ns.time.localtime(t);
```

### `ns.url.*` — URL utilities

```js
ns.url.encode("hello world");    // "hello%20world"
ns.url.decode("hello%20world");  // "hello world"
ns.url.parse("/foo?a=1&b=2");    // {path, query: {a:"1", b:"2"}}
ns.url.toFile("/index.html");    // filesystem path
```

### `ns.html.*` — HTML utilities

```js
ns.html.quote('<b>Hello & "World"</b>');   // "&lt;b&gt;Hello &amp; ..."
ns.html.guessType("index.html");           // "text/html"
ns.html.hrefs('<a href="/foo">link</a>');   // ["/foo"]
```

> **Note:** `ns.html.strip` was removed — use
> `str.replace(/<[^>]*>/g, '')` instead.

### `ns.file.*` — file I/O

```js
ns.file.write("/tmp/out.txt", "hello");
var content = ns.file.read("/tmp/out.txt");
ns.file.exists("/tmp/out.txt");     // true
ns.file.stat("/tmp/out.txt");       // {size, mtime, isFile, isDir}
ns.file.mkdir("/tmp/dir");
ns.file.unlink("/tmp/out.txt");
ns.file.cp("/src", "/dst");
ns.file.rename("/old", "/new");
ns.file.tmpnam();                   // unique temp path
ns.file.normalizePath("../foo");
ns.file.chmod("/tmp/out.txt", 0o644);
ns.file.link("/src", "/hard");
ns.file.symlink("/src", "/link");
ns.file.truncate("/tmp/out.txt", 0);
ns.file.roll("/var/log/app.log");   // rotate log file
ns.file.purge("/var/log/", "*.log", 7);  // delete logs older than 7 days
```

### `ns.image.*` — image dimensions

```js
var dim = ns.image.gifSize("/path/to/image.gif");   // {width, height}
var dim = ns.image.jpegSize("/path/to/image.jpg");  // {width, height}
```

### `ns.dns.*` — DNS lookups

```js
ns.dns.addrByHost("example.com");   // "93.184.216.34" or null
ns.dns.hostByAddr("93.184.216.34"); // "example.com" or null
```

### `ns.env.*` — environment variables

```js
ns.env.get("HOME");
ns.env.set("MY_VAR", "value");
ns.env.unset("MY_VAR");
ns.env.names();   // array of all variable names
```

### `ns.process.*` — process operations

```js
ns.process.kill(pid);           // SIGTERM
ns.process.kill(pid, 9);        // SIGKILL
```

### `ns.http.*` — HTTP client

```js
var resp = ns.http.get("http://example.com/api");
// resp: {status, headers, body}
```

### `ns.sock.*` — raw sockets

```js
var fd = ns.sock.open("127.0.0.1", 8080);
ns.sock.send(fd, "GET / HTTP/1.0\r\n\r\n");
var data = ns.sock.recv(fd, 4096);
ns.sock.close(fd);

var lfd = ns.sock.listen("127.0.0.1", 9000);
var cfd = ns.sock.accept(lfd);
ns.sock.setNonBlocking(cfd);
var n = ns.sock.nread(cfd);   // bytes available without blocking
```

### `ns.sched.*` — task scheduling

Callbacks receive a fresh V8 isolate (no request context).

```js
// One-shot: run after N seconds
ns.sched.after(5, 'ns.log("Notice", "5s later")');

// Recurring: run every N seconds
var id = ns.sched.interval(60, 'ns.shared.incr("stats","ticks",1)');
ns.sched.cancel(id);

// Daily at HH:MM:SS
ns.sched.daily(2, 30, 0, 'cleanup()');

// Weekly: day 0=Sunday
ns.sched.weekly(0, 3, 0, 0, 'weeklyReport()');

// Pause / resume a recurring schedule
ns.sched.pause(id);
ns.sched.resume(id);
```

### `ns.rand` — random numbers

```js
ns.rand();       // float in [0.0, 1.0)
ns.rand(100);    // integer in [0, 100)  — wraps Ns_DRand()
```

### `ns.atshutdown` / `ns.atsignal`

Register a JS code string to run at server shutdown or on `SIGHUP`.
The callback runs in a fresh isolate with no request context.

```js
ns.atshutdown('ns.log("Notice", "server shutting down")');
ns.atsignal('ns.shared.set("flags","reload","1")');
```

### `ns.log` — logging

```js
ns.log("Notice", "page rendered in " + elapsed + "ms");
ns.log("Warning", "cache miss");
ns.log("Error", "database unreachable");
ns.log.roll();   // rotate the server error log
```

### `ns.sleep` — delay

```js
ns.sleep(250);   // pause current thread for 250 ms
```

### `ns.crypt` — DES crypt

```js
var hash = ns.crypt("password", "ab");   // DES crypt(3)
```

### Synchronisation primitives

All handles are integer IDs; objects persist for the server lifetime.

#### `ns.mutex.*`

```js
var m = ns.mutex.create();
ns.mutex.lock(m);
// ... critical section ...
ns.mutex.unlock(m);
ns.mutex.trylock(m);   // returns boolean
ns.mutex.destroy(m);
```

#### `ns.rwlock.*`

```js
var rw = ns.rwlock.create();
ns.rwlock.readLock(rw);
ns.rwlock.unlock(rw);
ns.rwlock.writeLock(rw);
ns.rwlock.unlock(rw);
ns.rwlock.destroy(rw);
```

#### `ns.sema.*`

```js
var s = ns.sema.create(0);   // initial count
ns.sema.wait(s);
ns.sema.post(s);
ns.sema.destroy(s);
```

#### `ns.cond.*`

```js
var c = ns.cond.create();
var m = ns.mutex.create();
ns.mutex.lock(m);
ns.cond.wait(c, m);                  // releases lock while waiting
ns.cond.timedWait(c, m, 5000);      // ms timeout
ns.cond.signal(c);
ns.cond.broadcast(c);
ns.cond.destroy(c);
```

### `ns.set.*` — Ns_Set key/value store

Wraps AOLserver's `Ns_Set` (an ordered, case-insensitive key/value list).

```js
var s = ns.set.create("mySet");
ns.set.put(s, "color", "blue");
ns.set.put(s, "size", "large");
ns.set.get(s, "color");         // "blue"
ns.set.iget(s, "COLOR");        // "blue" (case-insensitive)
ns.set.find(s, "size");         // index or -1
ns.set.size(s);                 // 2
ns.set.key(s, 0);               // "color"
ns.set.value(s, 0);             // "blue"
ns.set.update(s, "color", "red");
ns.set.delete(s, 0);
ns.set.toObject(s);             // {color:"red", size:"large"}
ns.set.free(s);
```

### `ns.thread.*` — thread operations

```js
var tid = ns.thread.create('ns.sleep(1000); ns.log("Notice","done")');
ns.thread.id();                  // current thread ID string
ns.thread.yield();
ns.thread.setName("worker-1");
ns.thread.getName();
```

---

## jscp — JavaScript control port

jscp is a plain-TCP REPL that lets you evaluate JavaScript interactively
against a running server, similar to the Tcl `nscp` module.

### Enabling

Add to your `config.tcl`:

```tcl
ns_section "ns/server/myserver/module/nsjs"
    ns_param jscp_address      127.0.0.1
    ns_param jscp_port         9090
    ns_param jscp_users        "admin:secret ops:hunter2"
    ns_param jscp_log          false
    ns_param jscp_max_sessions 5
```

| Parameter | Default | Description |
|---|---|---|
| `jscp_address` | — | Bind address (required to enable jscp) |
| `jscp_port` | — | TCP port (required to enable jscp) |
| `jscp_users` | — | Space-separated `user:password` pairs |
| `jscp_log` | `false` | Log each command to the server error log |
| `jscp_max_sessions` | `5` | Maximum concurrent sessions |

### Connecting

```sh
telnet 127.0.0.1 9090
# or
nc 127.0.0.1 9090
```

### Session example

```
Username: admin
Password: secret
Login successful.
jscp 1> ns.info.version()
4.5
jscp 2> ns.info.serverName()
myserver
jscp 3> ns.rand(100)
42
jscp 4> var hits = ns.shared.get("stats", "hits"); hits
1234
jscp 5> ({server: ns.info.serverName(), uptime: ns.info.uptime()})
{"server":"myserver","uptime":3600}
jscp 6> ns.log("Notice", "hello from jscp")
undefined
jscp 7> exit
```

### Protocol details

- **Authentication:** `Username: ` prompt → `Password: ` prompt.
  Incorrect credentials print `Login incorrect.` and close the connection.
- **Prompt format:** `jscp N> ` where N is a monotonically increasing
  command counter for the session.
- **Persistent state:** Each session has its own V8 isolate with a
  single context that persists across commands.  Variables declared with
  `var` survive to subsequent commands.
- **Line continuation:** End a line with `\` to continue on the next
  line.  The continuation prompt is `... `.
- **Result serialisation:** Objects and arrays are JSON-stringified.
  Primitives use `.toString()`.  `undefined` prints as `"undefined"`.
- **Errors:** Compilation and runtime errors print
  `ERROR: <message> (line N)` rather than closing the session.
- **Disconnecting:** Type `exit` or `quit`, or close the TCP connection.
- **`ns.conn.*` in jscp:** All connection methods return `null` or
  do nothing gracefully — there is no HTTP request in a jscp session.

### Security notes

- jscp binds to `127.0.0.1` by default.  Do not expose it externally.
- Passwords are stored and compared in plaintext using a constant-time
  comparison to prevent timing attacks.
- Each authenticated session can run arbitrary JavaScript with full
  access to the `ns` API.  Treat jscp credentials like root passwords.
