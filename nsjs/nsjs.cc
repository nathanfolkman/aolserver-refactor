/*
 * nsjs.cc --
 *
 *   AOLserver module providing server-side JavaScript execution via V8.
 *
 *   Architecture mirrors nsd/tclinit.c:
 *     - One v8::Isolate per thread (cached in TLS), lazy-created on first request.
 *     - One v8::Context per request (cheap, created/disposed each time).
 *     - Module-scope shared store (js_shared) for cross-thread string data.
 *
 *   Supported file types:
 *     *.js    -- plain JavaScript, executed directly
 *     *.jsadp -- template: static HTML + <% code %> blocks compiled to JS
 *
 *   JavaScript API surface:
 *     ns.conn.*        — request/response (write, getHeader, setHeader, getQuery,
 *                        getMethod, getUrl, getPeerAddr, getHost, getPort, getId,
 *                        getAuthUser, getAuthPasswd, getAllHeaders, getAllQuery,
 *                        getContent, setStatus, setContentType, returnRedirect,
 *                        returnHtml, returnFile, close, return, returnBadRequest,
 *                        returnForbidden, returnNotFound, returnUnauthorized,
 *                        returnNotice, returnError, headers, startContent,
 *                        respond, internalRedirect, parseHeader, authorize)
 *     ns.shared.*      — cross-thread shared vars (set, get, exists, unset, incr,
 *                        append, lappend, names, keys, getAll)
 *     ns.cache.*       — named caches (create, get, set, unset, flush, stats)
 *     ns.config()      — config access; .section(name), .sections(),
 *                        ns.configInt(), ns.configBool()
 *     ns.info.*        — server info (version, uptime, pageroot, log, config,
 *                        hostname, address, pid)
 *     ns.time()        — time functions (now, format, httpTime, parseHttpTime,
 *                        gmtime, localtime)
 *     ns.url.*         — URL utilities (encode, decode, parse, toFile)
 *     ns.html.*        — HTML utilities (quote, guessType, hrefs)
 *     ns.file.*        — file I/O (read, write, exists, stat, mkdir, rmdir,
 *                        unlink, cp, rename, tmpnam, normalizePath, chmod,
 *                        link, symlink, truncate, roll, purge)
 *     ns.image.*       — image utilities (gifSize, jpegSize)
 *     ns.dns.*         — DNS lookups (addrByHost, hostByAddr)
 *     ns.env.*         — environment variables (get, set, unset, names)
 *     ns.process.*     — process operations (kill)
 *     ns.http.*        — HTTP client (get)
 *     ns.sock.*        — socket ops (open, listen, accept, recv, send, close,
 *                        setBlocking, setNonBlocking, nread)
 *     ns.sched.*       — scheduling (after, interval, cancel, daily, weekly,
 *                        pause, resume)
 *     ns.mutex.*       — mutexes (create, lock, unlock, trylock, destroy)
 *     ns.rwlock.*      — rwlocks (create, readLock, writeLock, unlock, destroy)
 *     ns.sema.*        — semaphores (create, wait, post, destroy)
 *     ns.cond.*        — condition vars (create, signal, broadcast, wait,
 *                        timedWait, destroy)
 *     ns.set.*         — in-memory Ns_Set (create, put, get, iget, find, size,
 *                        key, value, delete, update, free, toObject)
 *     ns.thread.*      — thread ops (create, id, yield, setName, getName)
 *     ns.log(level, msg); ns.log.roll()
 *     ns.sleep(ms)
 *     ns.crypt(key, salt)
 *     ns.rand([max])
 *     ns.atshutdown(jsCode) / ns.atsignal(jsCode)
 *     ns.info.serverName
 *     ns.conn.location()
 *
 *   JavaScript control port (jscp):
 *     Configured via ns/server/<server>/module/<module> params:
 *       jscp_address, jscp_port, jscp_users, jscp_log, jscp_max_sessions
 */

/*
 * Include order is critical on macOS:
 *  1. <signal.h> first — establishes pthread_sigmask without _Nullable
 *     before V8 pulls in <pthread.h> (via C++ stdlib) which redeclares it
 *     with _Nullable.  Including signal.h first prevents the conflict.
 *  2. All C++ stdlib and V8 headers before the extern "C" { #include "ns.h" }
 *     block — ns.h -> nsthread.h -> tcl.h, and Tcl's tcl.h does
 *     "#define const" when NO_CONST is defined, corrupting C++ type
 *     qualifiers and iomanip/bitset include guards for any C++ header
 *     included afterwards.
 *  3. #undef const after the ns.h block to restore C++ const semantics
 *     for our own function bodies.
 */
#include <signal.h>

#include <v8.h>
#include <libplatform/libplatform.h>

#include <sys/stat.h>
#include <fstream>
#include <sstream>
#include <string>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <unordered_map>
#include <vector>
#include <cerrno>
#include <ctime>
#include <cstdio>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* AOLserver/Tcl headers last */
extern "C" {
#include "ns.h"
}

/*
 * Tcl's tcl.h does "#define const" (empty) when NO_CONST is set.
 * Restore the const keyword for all C++ code that follows.
 */
#ifdef const
# undef const
#endif

/*
 * Tcl_FindHashEntry and Tcl_CreateHashEntry macros hardcode a
 * "(const char *)" cast in their expansion.  After #undef const the
 * cast produces a genuine const char*, but the underlying function
 * pointers (declared while NO_CONST was active) take plain char*.
 * Redefine the macros here to use a plain (char *) cast instead.
 */
#undef Tcl_FindHashEntry
#define Tcl_FindHashEntry(t, k)      ((*((t)->findProc))((t),  (char *)(k)))
#undef Tcl_CreateHashEntry
#define Tcl_CreateHashEntry(t, k, n) ((*((t)->createProc))((t),(char *)(k),(n)))

/*
 * nc() -- cast away const for AOLserver/Tcl C API calls that take char*
 * instead of const char* (a consequence of Tcl's NO_CONST ABI).
 */
static inline char *nc(const char *s) { return const_cast<char *>(s); }
static inline char *nc(const std::string &s) { return const_cast<char *>(s.c_str()); }

/* -----------------------------------------------------------------------
 * Per-module-instance config (one per Ns_ModuleInit call / virtual server)
 * --------------------------------------------------------------------- */

struct NsMod {
    char *server;
    char *module;
    char *pageRoot;
};

/* -----------------------------------------------------------------------
 * Per-thread V8 state (stored in TLS)
 * --------------------------------------------------------------------- */

struct JsData {
    v8::Isolate                        *isolate;
    v8::ArrayBuffer::Allocator         *allocator;
    v8::Persistent<v8::ObjectTemplate>  globalTmpl;
    NsMod                              *modPtr;
};

/* -----------------------------------------------------------------------
 * Per-request context (stack-allocated in request handlers)
 * --------------------------------------------------------------------- */

struct NsJsContext {
    JsData        *dataPtr;
    Ns_Conn       *conn;
    Ns_DString     output;
    bool           responseSent; /* set when JS calls returnHtml/returnRedirect/etc */
};

/* -----------------------------------------------------------------------
 * Module-scope shared store — array/key -> string value, guarded by mutex
 * --------------------------------------------------------------------- */

struct SharedStore {
    Ns_Mutex      lock;
    Tcl_HashTable arrays;   /* array name -> Tcl_HashTable * (key -> char *) */
};

static SharedStore js_shared;

/* -----------------------------------------------------------------------
 * Module-scope V8 platform (one process-wide instance)
 * --------------------------------------------------------------------- */

static std::unique_ptr<v8::Platform> js_platform;
static Ns_Mutex                      platform_lock;
static int                           platform_initialized = 0;

/* -----------------------------------------------------------------------
 * TLS slot for per-thread JsData
 * --------------------------------------------------------------------- */

static Ns_Tls js_tls;
static Ns_Mutex  tls_lock;
static int       tls_allocated = 0;

/* -----------------------------------------------------------------------
 * Cache registry — name -> Ns_Cache*, guarded by js_cache_lock
 * --------------------------------------------------------------------- */

static Ns_Mutex                                js_cache_lock;
static std::unordered_map<std::string, Ns_Cache*> *js_caches = nullptr;

/* -----------------------------------------------------------------------
 * Mutex registry — id -> Ns_Mutex*, guarded by js_mutex_map_lock
 * --------------------------------------------------------------------- */

static Ns_Mutex                         js_mutex_map_lock;
static int                              js_mutex_next_id = 0;
static std::unordered_map<int, Ns_Mutex*> *js_mutex_map = nullptr;

/* -----------------------------------------------------------------------
 * RWLock registry — id -> Ns_RWLock*, guarded by js_rwlock_map_lock
 * --------------------------------------------------------------------- */

static Ns_Mutex                            js_rwlock_map_lock;
static int                                 js_rwlock_next_id = 0;
static std::unordered_map<int, Ns_RWLock*> *js_rwlock_map = nullptr;

struct SchedRepeatCtx; /* forward declare for sched registry below */

/* -----------------------------------------------------------------------
 * Sched repeat registry — schedule id -> SchedRepeatCtx* for cleanup
 * --------------------------------------------------------------------- */

static Ns_Mutex                                  js_sched_map_lock;
static std::unordered_map<int, SchedRepeatCtx*> *js_sched_map = nullptr;

/* -----------------------------------------------------------------------
 * Sema registry — id -> Ns_Sema*, guarded by js_sema_map_lock
 * --------------------------------------------------------------------- */

static Ns_Mutex                           js_sema_map_lock;
static int                                js_sema_next_id = 0;
static std::unordered_map<int, Ns_Sema*> *js_sema_map = nullptr;

/* -----------------------------------------------------------------------
 * Cond registry — id -> Ns_Cond*, guarded by js_cond_map_lock
 * --------------------------------------------------------------------- */

static Ns_Mutex                           js_cond_map_lock;
static int                                js_cond_next_id = 0;
static std::unordered_map<int, Ns_Cond*> *js_cond_map = nullptr;

/* -----------------------------------------------------------------------
 * Set registry — id -> Ns_Set*, guarded by js_set_map_lock
 * --------------------------------------------------------------------- */

static Ns_Mutex                          js_set_map_lock;
static int                               js_set_next_id = 0;
static std::unordered_map<int, Ns_Set*> *js_set_map = nullptr;

/* -----------------------------------------------------------------------
 * JavaScript control port (jscp) configuration and state
 * --------------------------------------------------------------------- */

struct JsCpUser {
    std::string username;
    std::string password;   /* plaintext; constant-time compared */
};

struct NsJsCpConfig {
    std::string             address;
    int                     port;
    std::vector<JsCpUser>   users;
    bool                    logCommands;
    int                     maxSessions;
    NsMod                  *modPtr;
};

static NsJsCpConfig *js_cp_config    = nullptr;
static Ns_Mutex      js_cp_conn_mx;
static int           js_cp_conn_count = 0;

/* -----------------------------------------------------------------------
 * Custom ArrayBuffer allocator routing through AOLserver's ns_malloc
 * (which maps to tcmalloc when force-loaded by nsd)
 * --------------------------------------------------------------------- */

class NsAllocator final : public v8::ArrayBuffer::Allocator {
public:
    void *Allocate(size_t length) override {
        return ns_calloc(1, length);
    }
    void *AllocateUninitialized(size_t length) override {
        return ns_malloc(length);
    }
    void Free(void *data, size_t /*length*/) override {
        ns_free(data);
    }
};

/* -----------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------- */

static JsData *GetJsData(NsMod *modPtr);
static void    DeleteJsData(void *arg);
static void    BuildGlobalTemplate(v8::Isolate *isolate,
                                   v8::Local<v8::ObjectTemplate> &globalTmpl);
static int     RunScript(NsJsContext *ctx, const std::string &source,
                         const std::string &filename);
static std::string CompileJsAdp(const std::string &source);

static int JsRequest(void *arg, Ns_Conn *conn);
static int JsAdpRequest(void *arg, Ns_Conn *conn);

/* -----------------------------------------------------------------------
 * V8 helper: convert v8::String to std::string (UTF-8)
 * --------------------------------------------------------------------- */

static std::string V8ToString(v8::Isolate *isolate, v8::Local<v8::Value> val) {
    v8::String::Utf8Value utf8(isolate, val);
    return (*utf8 != nullptr) ? std::string(*utf8, utf8.length()) : std::string();
}

/* Convenience: create a V8 string from a C string (never returns empty) */
static v8::Local<v8::String> v8s(v8::Isolate *iso, const char *s) {
    if (s == nullptr) s = "";
    return v8::String::NewFromUtf8(iso, s).ToLocalChecked();
}
static v8::Local<v8::String> v8s(v8::Isolate *iso, const std::string &s) {
    return v8::String::NewFromUtf8(iso, s.c_str(),
                                   v8::NewStringType::kNormal,
                                   static_cast<int>(s.size())).ToLocalChecked();
}

/* -----------------------------------------------------------------------
 * One-time global V8 + shared store initialisation
 * --------------------------------------------------------------------- */

static void EnsurePlatformInit(void) {
    Ns_MutexLock(&platform_lock);
    if (!platform_initialized) {
        js_platform = v8::platform::NewDefaultPlatform();
        v8::V8::InitializePlatform(js_platform.get());
        v8::V8::Initialize();

        Ns_MutexInit(&js_shared.lock);
        Tcl_InitHashTable(&js_shared.arrays, TCL_STRING_KEYS);

        Ns_MutexInit(&js_cache_lock);
        js_caches = new std::unordered_map<std::string, Ns_Cache*>();

        Ns_MutexInit(&js_mutex_map_lock);
        js_mutex_map = new std::unordered_map<int, Ns_Mutex*>();

        Ns_MutexInit(&js_rwlock_map_lock);
        js_rwlock_map = new std::unordered_map<int, Ns_RWLock*>();

        Ns_MutexInit(&js_sched_map_lock);
        js_sched_map = new std::unordered_map<int, SchedRepeatCtx*>();

        Ns_MutexInit(&js_sema_map_lock);
        js_sema_map = new std::unordered_map<int, Ns_Sema*>();

        Ns_MutexInit(&js_cond_map_lock);
        js_cond_map = new std::unordered_map<int, Ns_Cond*>();

        Ns_MutexInit(&js_set_map_lock);
        js_set_map = new std::unordered_map<int, Ns_Set*>();

        platform_initialized = 1;
    }
    Ns_MutexUnlock(&platform_lock);
}

/* -----------------------------------------------------------------------
 * TLS slot allocation — called at most once across all ModInit calls
 * --------------------------------------------------------------------- */

static void EnsureTlsAlloc(void) {
    Ns_MutexLock(&tls_lock);
    if (!tls_allocated) {
        Ns_TlsAlloc(&js_tls, DeleteJsData);
        tls_allocated = 1;
    }
    Ns_MutexUnlock(&tls_lock);
}

/* -----------------------------------------------------------------------
 * GetJsData — lazy per-thread isolate creation
 * --------------------------------------------------------------------- */

static JsData *GetJsData(NsMod *modPtr) {
    JsData *dataPtr = static_cast<JsData *>(Ns_TlsGet(&js_tls));
    if (dataPtr == nullptr) {
        dataPtr = static_cast<JsData *>(ns_calloc(1, sizeof(JsData)));
        dataPtr->modPtr    = modPtr;
        dataPtr->allocator = new NsAllocator();

        v8::Isolate::CreateParams params;
        params.array_buffer_allocator = dataPtr->allocator;
        dataPtr->isolate = v8::Isolate::New(params);

        /* Build the global object template once per thread */
        v8::Isolate::Scope isolate_scope(dataPtr->isolate);
        v8::HandleScope    handle_scope(dataPtr->isolate);
        v8::Local<v8::ObjectTemplate> tmpl =
            v8::ObjectTemplate::New(dataPtr->isolate);
        BuildGlobalTemplate(dataPtr->isolate, tmpl);
        dataPtr->globalTmpl.Reset(dataPtr->isolate, tmpl);

        Ns_TlsSet(&js_tls, dataPtr);
    }
    return dataPtr;
}

/* -----------------------------------------------------------------------
 * DeleteJsData — TLS destructor, called at thread exit
 * --------------------------------------------------------------------- */

static void DeleteJsData(void *arg) {
    JsData *dataPtr = static_cast<JsData *>(arg);
    if (dataPtr == nullptr) return;

    {
        v8::Isolate::Scope isolate_scope(dataPtr->isolate);
        v8::HandleScope    handle_scope(dataPtr->isolate);
        dataPtr->globalTmpl.Reset();
    }
    dataPtr->isolate->Dispose();
    delete dataPtr->allocator;
    ns_free(dataPtr);
}

/* -----------------------------------------------------------------------
 * C++ callbacks for ns.conn.*
 *
 * All callbacks retrieve NsJsContext * from embedder data slot 0.
 * --------------------------------------------------------------------- */

static NsJsContext *GetCtx(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Local<v8::Context> ctx = args.GetIsolate()->GetCurrentContext();
    v8::Local<v8::Value>   ext = ctx->GetEmbedderData(0);
    if (ext.IsEmpty() || !ext->IsExternal()) return nullptr;
    return static_cast<NsJsContext *>(ext.As<v8::External>()->Value());
}

static NsMod *GetMod(const v8::FunctionCallbackInfo<v8::Value> &args) {
    NsJsContext *ctx = GetCtx(args);
    return (ctx != nullptr) ? ctx->dataPtr->modPtr : nullptr;
}

/* ns.conn.write(str) */
static void JsConnWrite(const v8::FunctionCallbackInfo<v8::Value> &args) {
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || args.Length() < 1) return;
    std::string s = V8ToString(args.GetIsolate(), args[0]);
    Ns_DStringNAppend(&ctx->output, nc(s), static_cast<int>(s.size()));
}

/* ns.conn.getHeader(name) */
static void JsConnGetHeader(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || ctx->conn == nullptr || args.Length() < 1) {
        args.GetReturnValue().SetNull();
        return;
    }
    std::string name = V8ToString(iso, args[0]);
    char *val = Ns_SetIGet(ctx->conn->headers, nc(name));
    if (val != nullptr) {
        args.GetReturnValue().Set(
            v8::String::NewFromUtf8(iso, val).ToLocalChecked());
    } else {
        args.GetReturnValue().SetNull();
    }
}

/* ns.conn.setHeader(name, value) */
static void JsConnSetHeader(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || ctx->conn == nullptr || args.Length() < 2) return;
    std::string name  = V8ToString(iso, args[0]);
    std::string value = V8ToString(iso, args[1]);
    Ns_SetUpdate(ctx->conn->outputheaders, nc(name), nc(value));
}

/* ns.conn.getQuery(name) */
static void JsConnGetQuery(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || ctx->conn == nullptr || args.Length() < 1) {
        args.GetReturnValue().SetNull();
        return;
    }
    std::string name  = V8ToString(iso, args[0]);
    Ns_Set     *query = Ns_ConnGetQuery(ctx->conn);
    if (query != nullptr) {
        char *val = Ns_SetIGet(query, nc(name));
        if (val != nullptr) {
            args.GetReturnValue().Set(
                v8::String::NewFromUtf8(iso, val).ToLocalChecked());
            return;
        }
    }
    args.GetReturnValue().SetNull();
}

/* ns.conn.getMethod() */
static void JsConnGetMethod(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || ctx->conn == nullptr || ctx->conn->request == nullptr) {
        args.GetReturnValue().SetNull();
        return;
    }
    args.GetReturnValue().Set(
        v8::String::NewFromUtf8(iso, ctx->conn->request->method)
            .ToLocalChecked());
}

/* ns.conn.getUrl() */
static void JsConnGetUrl(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || ctx->conn == nullptr || ctx->conn->request == nullptr) {
        args.GetReturnValue().SetNull();
        return;
    }
    args.GetReturnValue().Set(
        v8::String::NewFromUtf8(iso, ctx->conn->request->url)
            .ToLocalChecked());
}

/* ns.log(level, msg) */
static void JsLog(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) return;

    std::string level = V8ToString(iso, args[0]);
    std::string msg   = V8ToString(iso, args[1]);

    Ns_LogSeverity sev = Notice;
    if (level == "Warning") sev = Warning;
    else if (level == "Error") sev = Error;

    Ns_Log(sev, nc("nsjs: %s"), nc(msg));
}

/* -----------------------------------------------------------------------
 * C++ callbacks for ns.shared.*
 *
 * These access js_shared directly — no embedder data needed.
 * --------------------------------------------------------------------- */

/* ns.shared.set(array, key, value) */
static void JsSharedSet(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 3) return;

    std::string arr = V8ToString(iso, args[0]);
    std::string key = V8ToString(iso, args[1]);
    std::string val = V8ToString(iso, args[2]);

    Ns_MutexLock(&js_shared.lock);

    int isNew = 0;
    Tcl_HashEntry *arrEntry = Tcl_CreateHashEntry(
        &js_shared.arrays, nc(arr), &isNew);
    Tcl_HashTable *inner;
    if (isNew) {
        inner = static_cast<Tcl_HashTable *>(ns_malloc(sizeof(Tcl_HashTable)));
        Tcl_InitHashTable(inner, TCL_STRING_KEYS);
        Tcl_SetHashValue(arrEntry, inner);
    } else {
        inner = static_cast<Tcl_HashTable *>(Tcl_GetHashValue(arrEntry));
    }

    int keyIsNew = 0;
    Tcl_HashEntry *keyEntry = Tcl_CreateHashEntry(inner, nc(key), &keyIsNew);
    if (!keyIsNew) {
        char *old = static_cast<char *>(Tcl_GetHashValue(keyEntry));
        ns_free(old);
    }
    Tcl_SetHashValue(keyEntry, ns_strdup(nc(val)));

    Ns_MutexUnlock(&js_shared.lock);
}

/* ns.shared.get(array, key) */
static void JsSharedGet(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) {
        args.GetReturnValue().SetNull();
        return;
    }
    std::string arr = V8ToString(iso, args[0]);
    std::string key = V8ToString(iso, args[1]);

    Ns_MutexLock(&js_shared.lock);

    char *result = nullptr;
    Tcl_HashEntry *arrEntry = Tcl_FindHashEntry(&js_shared.arrays, nc(arr));
    if (arrEntry != nullptr) {
        Tcl_HashTable *inner =
            static_cast<Tcl_HashTable *>(Tcl_GetHashValue(arrEntry));
        Tcl_HashEntry *keyEntry = Tcl_FindHashEntry(inner, nc(key));
        if (keyEntry != nullptr) {
            result = ns_strdup(static_cast<char *>(Tcl_GetHashValue(keyEntry)));
        }
    }

    Ns_MutexUnlock(&js_shared.lock);

    if (result != nullptr) {
        args.GetReturnValue().Set(
            v8::String::NewFromUtf8(iso, result).ToLocalChecked());
        ns_free(result);
    } else {
        args.GetReturnValue().SetNull();
    }
}

/* ns.shared.exists(array, key) */
static void JsSharedExists(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) {
        args.GetReturnValue().Set(false);
        return;
    }
    std::string arr = V8ToString(iso, args[0]);
    std::string key = V8ToString(iso, args[1]);

    Ns_MutexLock(&js_shared.lock);

    bool found = false;
    Tcl_HashEntry *arrEntry = Tcl_FindHashEntry(&js_shared.arrays, nc(arr));
    if (arrEntry != nullptr) {
        Tcl_HashTable *inner =
            static_cast<Tcl_HashTable *>(Tcl_GetHashValue(arrEntry));
        found = (Tcl_FindHashEntry(inner, nc(key)) != nullptr);
    }

    Ns_MutexUnlock(&js_shared.lock);
    args.GetReturnValue().Set(found);
}

/* ns.shared.unset(array, key) */
static void JsSharedUnset(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) return;

    std::string arr = V8ToString(iso, args[0]);
    std::string key = V8ToString(iso, args[1]);

    Ns_MutexLock(&js_shared.lock);

    Tcl_HashEntry *arrEntry = Tcl_FindHashEntry(&js_shared.arrays, nc(arr));
    if (arrEntry != nullptr) {
        Tcl_HashTable *inner =
            static_cast<Tcl_HashTable *>(Tcl_GetHashValue(arrEntry));
        Tcl_HashEntry *keyEntry = Tcl_FindHashEntry(inner, nc(key));
        if (keyEntry != nullptr) {
            ns_free(static_cast<char *>(Tcl_GetHashValue(keyEntry)));
            Tcl_DeleteHashEntry(keyEntry);
        }
    }

    Ns_MutexUnlock(&js_shared.lock);
}

/* ns.shared.incr(array, key, delta) */
static void JsSharedIncr(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 3) {
        args.GetReturnValue().Set(0);
        return;
    }
    std::string arr   = V8ToString(iso, args[0]);
    std::string key   = V8ToString(iso, args[1]);
    long        delta = static_cast<long>(
        args[2]->IntegerValue(iso->GetCurrentContext()).FromMaybe(0));

    Ns_MutexLock(&js_shared.lock);

    int isNew = 0;
    Tcl_HashEntry *arrEntry = Tcl_CreateHashEntry(
        &js_shared.arrays, nc(arr), &isNew);
    Tcl_HashTable *inner;
    if (isNew) {
        inner = static_cast<Tcl_HashTable *>(ns_malloc(sizeof(Tcl_HashTable)));
        Tcl_InitHashTable(inner, TCL_STRING_KEYS);
        Tcl_SetHashValue(arrEntry, inner);
    } else {
        inner = static_cast<Tcl_HashTable *>(Tcl_GetHashValue(arrEntry));
    }

    int keyIsNew = 0;
    Tcl_HashEntry *keyEntry = Tcl_CreateHashEntry(inner, nc(key), &keyIsNew);
    long current = 0;
    if (!keyIsNew) {
        char *old = static_cast<char *>(Tcl_GetHashValue(keyEntry));
        current = strtol(old, nullptr, 10);
        ns_free(old);
    }
    long newVal = current + delta;

    char buf[64];
    snprintf(buf, sizeof(buf), "%ld", newVal);
    Tcl_SetHashValue(keyEntry, ns_strdup(buf));

    Ns_MutexUnlock(&js_shared.lock);

    args.GetReturnValue().Set(static_cast<double>(newVal));
}

/* -----------------------------------------------------------------------
 * Extended ns.conn.* callbacks
 * --------------------------------------------------------------------- */

/* ns.conn.getPeerAddr() */
static void JsConnGetPeerAddr(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || ctx->conn == nullptr) { args.GetReturnValue().SetNull(); return; }
    char *peer = Ns_ConnPeer(ctx->conn);
    args.GetReturnValue().Set(v8s(iso, peer));
}

/* ns.conn.location() — returns "http://host:port" string */
static void JsConnLocation(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || ctx->conn == nullptr) { args.GetReturnValue().SetNull(); return; }
    char *loc = Ns_ConnLocation(ctx->conn);
    args.GetReturnValue().Set(v8s(iso, loc ? loc : ""));
}

/* ns.conn.getHost() */
static void JsConnGetHost(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || ctx->conn == nullptr) { args.GetReturnValue().SetNull(); return; }
    char *host = Ns_ConnHost(ctx->conn);
    args.GetReturnValue().Set(v8s(iso, host));
}

/* ns.conn.getPort() */
static void JsConnGetPort(const v8::FunctionCallbackInfo<v8::Value> &args) {
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || ctx->conn == nullptr) { args.GetReturnValue().Set(0); return; }
    args.GetReturnValue().Set(Ns_ConnPort(ctx->conn));
}

/* ns.conn.getId() */
static void JsConnGetId(const v8::FunctionCallbackInfo<v8::Value> &args) {
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || ctx->conn == nullptr) { args.GetReturnValue().Set(-1); return; }
    args.GetReturnValue().Set(Ns_ConnId(ctx->conn));
}

/* ns.conn.getAuthUser() */
static void JsConnGetAuthUser(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || ctx->conn == nullptr) { args.GetReturnValue().SetNull(); return; }
    char *user = Ns_ConnAuthUser(ctx->conn);
    if (user != nullptr) args.GetReturnValue().Set(v8s(iso, user));
    else args.GetReturnValue().SetNull();
}

/* ns.conn.getAuthPasswd() */
static void JsConnGetAuthPasswd(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || ctx->conn == nullptr) { args.GetReturnValue().SetNull(); return; }
    char *pw = Ns_ConnAuthPasswd(ctx->conn);
    if (pw != nullptr) args.GetReturnValue().Set(v8s(iso, pw));
    else args.GetReturnValue().SetNull();
}

/* ns.conn.getAllHeaders() — returns JS object with all request headers */
static void JsConnGetAllHeaders(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || ctx->conn == nullptr) { args.GetReturnValue().SetNull(); return; }
    v8::Local<v8::Object> obj = v8::Object::New(iso);
    Ns_Set *hdrs = Ns_ConnHeaders(ctx->conn);
    if (hdrs != nullptr) {
        int n = Ns_SetSize(hdrs);
        for (int i = 0; i < n; i++) {
            char *k = Ns_SetKey(hdrs, i);
            char *v = Ns_SetValue(hdrs, i);
            if (k && v) {
                obj->Set(iso->GetCurrentContext(),
                         v8s(iso, k), v8s(iso, v)).Check();
            }
        }
    }
    args.GetReturnValue().Set(obj);
}

/* ns.conn.getAllQuery() — returns JS object with all query parameters */
static void JsConnGetAllQuery(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || ctx->conn == nullptr) { args.GetReturnValue().SetNull(); return; }
    v8::Local<v8::Object> obj = v8::Object::New(iso);
    Ns_Set *q = Ns_ConnGetQuery(ctx->conn);
    if (q != nullptr) {
        int n = Ns_SetSize(q);
        for (int i = 0; i < n; i++) {
            char *k = Ns_SetKey(q, i);
            char *v = Ns_SetValue(q, i);
            if (k && v) {
                obj->Set(iso->GetCurrentContext(),
                         v8s(iso, k), v8s(iso, v)).Check();
            }
        }
    }
    args.GetReturnValue().Set(obj);
}

/* ns.conn.getContent() — returns request body as string */
static void JsConnGetContent(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || ctx->conn == nullptr) { args.GetReturnValue().SetNull(); return; }
    int len = Ns_ConnContentLength(ctx->conn);
    if (len <= 0) { args.GetReturnValue().Set(v8s(iso, "")); return; }
    char *content = Ns_ConnContent(ctx->conn);
    if (content == nullptr) { args.GetReturnValue().SetNull(); return; }
    args.GetReturnValue().Set(
        v8::String::NewFromUtf8(iso, content,
                                v8::NewStringType::kNormal, len).ToLocalChecked());
}

/* ns.conn.setStatus(code) */
static void JsConnSetStatus(const v8::FunctionCallbackInfo<v8::Value> &args) {
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || ctx->conn == nullptr || args.Length() < 1) return;
    int code = static_cast<int>(
        args[0]->Int32Value(args.GetIsolate()->GetCurrentContext()).FromMaybe(200));
    Ns_ConnSetStatus(ctx->conn, code);
}

/* ns.conn.setContentType(type) */
static void JsConnSetContentType(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || ctx->conn == nullptr || args.Length() < 1) return;
    std::string type = V8ToString(iso, args[0]);
    Ns_ConnSetType(ctx->conn, nc(type));
}

/* ns.conn.returnRedirect(url) — sends redirect and marks response sent */
static void JsConnReturnRedirect(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || ctx->conn == nullptr || args.Length() < 1) return;
    std::string url = V8ToString(iso, args[0]);
    Ns_ConnReturnRedirect(ctx->conn, nc(url));
    ctx->responseSent = true;
}

/* ns.conn.returnHtml(status, html) — sends HTML response and marks response sent */
static void JsConnReturnHtml(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || ctx->conn == nullptr || args.Length() < 2) return;
    int code = static_cast<int>(
        args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(200));
    std::string html = V8ToString(iso, args[1]);
    Ns_ConnReturnHtml(ctx->conn, code, nc(html), static_cast<int>(html.size()));
    ctx->responseSent = true;
}

/* ns.conn.returnFile(status, type, path) — sends file response */
static void JsConnReturnFile(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || ctx->conn == nullptr || args.Length() < 3) return;
    int code = static_cast<int>(
        args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(200));
    std::string type = V8ToString(iso, args[1]);
    std::string path = V8ToString(iso, args[2]);
    Ns_ConnReturnFile(ctx->conn, code, nc(type), nc(path));
    ctx->responseSent = true;
}

/* ns.conn.close() */
static void JsConnClose(const v8::FunctionCallbackInfo<v8::Value> &args) {
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || ctx->conn == nullptr) return;
    Ns_ConnClose(ctx->conn);
    ctx->responseSent = true;
}

/* -----------------------------------------------------------------------
 * Extended ns.shared.* callbacks
 * --------------------------------------------------------------------- */

/* ns.shared.append(array, key, value) */
static void JsSharedAppend(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 3) return;
    std::string arr = V8ToString(iso, args[0]);
    std::string key = V8ToString(iso, args[1]);
    std::string app = V8ToString(iso, args[2]);

    Ns_MutexLock(&js_shared.lock);
    int isNew = 0;
    Tcl_HashEntry *arrEntry = Tcl_CreateHashEntry(&js_shared.arrays, nc(arr), &isNew);
    Tcl_HashTable *inner;
    if (isNew) {
        inner = static_cast<Tcl_HashTable *>(ns_malloc(sizeof(Tcl_HashTable)));
        Tcl_InitHashTable(inner, TCL_STRING_KEYS);
        Tcl_SetHashValue(arrEntry, inner);
    } else {
        inner = static_cast<Tcl_HashTable *>(Tcl_GetHashValue(arrEntry));
    }
    int keyIsNew = 0;
    Tcl_HashEntry *keyEntry = Tcl_CreateHashEntry(inner, nc(key), &keyIsNew);
    std::string cur;
    if (!keyIsNew) {
        char *old = static_cast<char *>(Tcl_GetHashValue(keyEntry));
        if (old) cur = old;
        ns_free(old);
    }
    cur += app;
    Tcl_SetHashValue(keyEntry, ns_strdup(nc(cur)));
    Ns_MutexUnlock(&js_shared.lock);
    args.GetReturnValue().Set(v8s(iso, cur));
}

/* ns.shared.lappend(array, key, value) — append as list element */
static void JsSharedLAppend(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 3) return;
    std::string arr = V8ToString(iso, args[0]);
    std::string key = V8ToString(iso, args[1]);
    std::string elm = V8ToString(iso, args[2]);

    Ns_MutexLock(&js_shared.lock);
    int isNew = 0;
    Tcl_HashEntry *arrEntry = Tcl_CreateHashEntry(&js_shared.arrays, nc(arr), &isNew);
    Tcl_HashTable *inner;
    if (isNew) {
        inner = static_cast<Tcl_HashTable *>(ns_malloc(sizeof(Tcl_HashTable)));
        Tcl_InitHashTable(inner, TCL_STRING_KEYS);
        Tcl_SetHashValue(arrEntry, inner);
    } else {
        inner = static_cast<Tcl_HashTable *>(Tcl_GetHashValue(arrEntry));
    }
    int keyIsNew = 0;
    Tcl_HashEntry *keyEntry = Tcl_CreateHashEntry(inner, nc(key), &keyIsNew);
    std::string cur;
    if (!keyIsNew) {
        char *old = static_cast<char *>(Tcl_GetHashValue(keyEntry));
        if (old) cur = old;
        ns_free(old);
    }
    if (!cur.empty()) cur += " ";
    cur += elm;
    Tcl_SetHashValue(keyEntry, ns_strdup(nc(cur)));
    Ns_MutexUnlock(&js_shared.lock);
    args.GetReturnValue().Set(v8s(iso, cur));
}

/* ns.shared.names(pattern) — list array names matching glob pattern */
static void JsSharedNames(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    std::string pat = (args.Length() >= 1) ? V8ToString(iso, args[0]) : std::string("*");

    v8::Local<v8::Array> result = v8::Array::New(iso);
    Ns_MutexLock(&js_shared.lock);
    Tcl_HashSearch search;
    Tcl_HashEntry *e = Tcl_FirstHashEntry(&js_shared.arrays, &search);
    uint32_t idx = 0;
    while (e != nullptr) {
        const char *name = static_cast<const char *>(Tcl_GetHashKey(&js_shared.arrays, e));
        if (fnmatch(pat.c_str(), name, 0) == 0) {
            result->Set(iso->GetCurrentContext(), idx++, v8s(iso, name)).Check();
        }
        e = Tcl_NextHashEntry(&search);
    }
    Ns_MutexUnlock(&js_shared.lock);
    args.GetReturnValue().Set(result);
}

/* ns.shared.keys(array) — list all keys in array */
static void JsSharedKeys(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(v8::Array::New(iso)); return; }
    std::string arr = V8ToString(iso, args[0]);

    v8::Local<v8::Array> result = v8::Array::New(iso);
    Ns_MutexLock(&js_shared.lock);
    Tcl_HashEntry *arrEntry = Tcl_FindHashEntry(&js_shared.arrays, nc(arr));
    uint32_t idx = 0;
    if (arrEntry != nullptr) {
        Tcl_HashTable *inner =
            static_cast<Tcl_HashTable *>(Tcl_GetHashValue(arrEntry));
        Tcl_HashSearch search;
        Tcl_HashEntry *e = Tcl_FirstHashEntry(inner, &search);
        while (e != nullptr) {
            const char *k = static_cast<const char *>(Tcl_GetHashKey(inner, e));
            result->Set(iso->GetCurrentContext(), idx++, v8s(iso, k)).Check();
            e = Tcl_NextHashEntry(&search);
        }
    }
    Ns_MutexUnlock(&js_shared.lock);
    args.GetReturnValue().Set(result);
}

/* ns.shared.getAll(array) — return all key/value pairs as JS object */
static void JsSharedGetAll(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().SetNull(); return; }
    std::string arr = V8ToString(iso, args[0]);

    v8::Local<v8::Object> obj = v8::Object::New(iso);
    Ns_MutexLock(&js_shared.lock);
    Tcl_HashEntry *arrEntry = Tcl_FindHashEntry(&js_shared.arrays, nc(arr));
    if (arrEntry != nullptr) {
        Tcl_HashTable *inner =
            static_cast<Tcl_HashTable *>(Tcl_GetHashValue(arrEntry));
        Tcl_HashSearch search;
        Tcl_HashEntry *e = Tcl_FirstHashEntry(inner, &search);
        while (e != nullptr) {
            const char *k = static_cast<const char *>(Tcl_GetHashKey(inner, e));
            char *v = static_cast<char *>(Tcl_GetHashValue(e));
            obj->Set(iso->GetCurrentContext(),
                     v8s(iso, k), v8s(iso, v ? v : "")).Check();
            e = Tcl_NextHashEntry(&search);
        }
    }
    Ns_MutexUnlock(&js_shared.lock);
    args.GetReturnValue().Set(obj);
}

/* -----------------------------------------------------------------------
 * ns.cache.* callbacks
 * --------------------------------------------------------------------- */

static Ns_Cache *LookupCache(const std::string &name) {
    Ns_MutexLock(&js_cache_lock);
    auto it = js_caches->find(name);
    Ns_Cache *cache = (it != js_caches->end()) ? it->second : nullptr;
    Ns_MutexUnlock(&js_cache_lock);
    return cache;
}

/* ns.cache.create(name, maxSize) */
static void JsCacheCreate(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) return;
    std::string name = V8ToString(iso, args[0]);
    size_t maxSize = static_cast<size_t>(
        args[1]->IntegerValue(iso->GetCurrentContext()).FromMaybe(0));

    Ns_MutexLock(&js_cache_lock);
    if (js_caches->find(name) == js_caches->end()) {
        Ns_Cache *c = Ns_CacheCreateSz(nc(name), TCL_STRING_KEYS, maxSize, nullptr);
        (*js_caches)[name] = c;
    }
    Ns_MutexUnlock(&js_cache_lock);
}

/* ns.cache.get(name, key) */
static void JsCacheGet(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().SetNull(); return; }
    std::string name = V8ToString(iso, args[0]);
    std::string key  = V8ToString(iso, args[1]);
    Ns_Cache *cache = LookupCache(name);
    if (cache == nullptr) { args.GetReturnValue().SetNull(); return; }

    char *result = nullptr;
    Ns_CacheLock(cache);
    Ns_Entry *entry = Ns_CacheFindEntry(cache, nc(key));
    if (entry != nullptr) {
        char *val = static_cast<char *>(Ns_CacheGetValue(entry));
        if (val) result = ns_strdup(val);
    }
    Ns_CacheUnlock(cache);

    if (result != nullptr) {
        args.GetReturnValue().Set(v8s(iso, result));
        ns_free(result);
    } else {
        args.GetReturnValue().SetNull();
    }
}

/* ns.cache.set(name, key, value) */
static void JsCacheSet(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 3) return;
    std::string name  = V8ToString(iso, args[0]);
    std::string key   = V8ToString(iso, args[1]);
    std::string value = V8ToString(iso, args[2]);
    Ns_Cache *cache = LookupCache(name);
    if (cache == nullptr) return;

    Ns_CacheLock(cache);
    int newPtr = 0;
    Ns_Entry *entry = Ns_CacheCreateEntry(cache, nc(key), &newPtr);
    if (!newPtr) {
        char *old = static_cast<char *>(Ns_CacheGetValue(entry));
        if (old) ns_free(old);
    }
    char *stored = ns_strdup(nc(value));
    Ns_CacheSetValueSz(entry, stored, value.size() + 1);
    Ns_CacheUnlock(cache);
}

/* ns.cache.unset(name, key) */
static void JsCacheUnset(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) return;
    std::string name = V8ToString(iso, args[0]);
    std::string key  = V8ToString(iso, args[1]);
    Ns_Cache *cache = LookupCache(name);
    if (cache == nullptr) return;

    Ns_CacheLock(cache);
    Ns_Entry *entry = Ns_CacheFindEntry(cache, nc(key));
    if (entry != nullptr) {
        char *val = static_cast<char *>(Ns_CacheGetValue(entry));
        if (val) ns_free(val);
        Ns_CacheDeleteEntry(entry);
    }
    Ns_CacheUnlock(cache);
}

/* ns.cache.flush(name) */
static void JsCacheFlush(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) return;
    std::string name = V8ToString(iso, args[0]);
    Ns_Cache *cache = LookupCache(name);
    if (cache == nullptr) return;

    Ns_CacheLock(cache);
    /* Free all values before flushing */
    Ns_CacheSearch search;
    Ns_Entry *entry = Ns_CacheFirstEntry(cache, &search);
    while (entry != nullptr) {
        char *val = static_cast<char *>(Ns_CacheGetValue(entry));
        if (val) ns_free(val);
        entry = Ns_CacheNextEntry(&search);
    }
    Ns_CacheFlush(cache);
    Ns_CacheUnlock(cache);
}

/* ns.cache.stats(name) — returns {name, entries} object */
static void JsCacheStats(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().SetNull(); return; }
    std::string name = V8ToString(iso, args[0]);
    Ns_Cache *cache = LookupCache(name);
    if (cache == nullptr) { args.GetReturnValue().SetNull(); return; }

    int count = 0;
    Ns_CacheLock(cache);
    Ns_CacheSearch search;
    Ns_Entry *e = Ns_CacheFirstEntry(cache, &search);
    while (e != nullptr) { count++; e = Ns_CacheNextEntry(&search); }
    Ns_CacheUnlock(cache);

    v8::Local<v8::Object> obj = v8::Object::New(iso);
    obj->Set(iso->GetCurrentContext(), v8s(iso, "name"),    v8s(iso, name)).Check();
    obj->Set(iso->GetCurrentContext(), v8s(iso, "entries"),
             v8::Integer::New(iso, count)).Check();
    args.GetReturnValue().Set(obj);
}

/* -----------------------------------------------------------------------
 * ns.config* callbacks
 * --------------------------------------------------------------------- */

/* ns.config(section, key [, default]) */
static void JsConfig(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().SetNull(); return; }
    std::string section = V8ToString(iso, args[0]);
    std::string key     = V8ToString(iso, args[1]);
    char *val = Ns_ConfigGetValue(nc(section), nc(key));
    if (val != nullptr) {
        args.GetReturnValue().Set(v8s(iso, val));
    } else if (args.Length() >= 3) {
        args.GetReturnValue().Set(args[2]);
    } else {
        args.GetReturnValue().SetNull();
    }
}

/* ns.configInt(section, key, default) */
static void JsConfigInt(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().SetNull(); return; }
    std::string section = V8ToString(iso, args[0]);
    std::string key     = V8ToString(iso, args[1]);
    int defVal = (args.Length() >= 3)
        ? static_cast<int>(args[2]->Int32Value(iso->GetCurrentContext()).FromMaybe(0))
        : 0;
    int result = defVal;
    Ns_ConfigGetInt(nc(section), nc(key), &result);
    args.GetReturnValue().Set(result);
}

/* ns.configBool(section, key, default) */
static void JsConfigBool(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().SetNull(); return; }
    std::string section = V8ToString(iso, args[0]);
    std::string key     = V8ToString(iso, args[1]);
    int defVal = (args.Length() >= 3) ? (args[2]->BooleanValue(iso) ? 1 : 0) : 0;
    int result = defVal;
    Ns_ConfigGetBool(nc(section), nc(key), &result);
    args.GetReturnValue().Set(static_cast<bool>(result));
}

/* -----------------------------------------------------------------------
 * ns.info.* callbacks
 * --------------------------------------------------------------------- */

static void JsInfoVersion(const v8::FunctionCallbackInfo<v8::Value> &args) {
    args.GetReturnValue().Set(v8s(args.GetIsolate(), NS_VERSION));
}

static void JsInfoUptime(const v8::FunctionCallbackInfo<v8::Value> &args) {
    args.GetReturnValue().Set(Ns_InfoUptime());
}

static void JsInfoPageroot(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsMod *mod = GetMod(args);
    if (mod == nullptr) { args.GetReturnValue().SetNull(); return; }
    char *root = Ns_PageRoot(mod->server);
    args.GetReturnValue().Set(v8s(iso, root ? root : ""));
}

static void JsInfoLog(const v8::FunctionCallbackInfo<v8::Value> &args) {
    args.GetReturnValue().Set(v8s(args.GetIsolate(), Ns_InfoErrorLog()));
}

static void JsInfoConfig(const v8::FunctionCallbackInfo<v8::Value> &args) {
    args.GetReturnValue().Set(v8s(args.GetIsolate(), Ns_InfoConfigFile()));
}

static void JsInfoHostname(const v8::FunctionCallbackInfo<v8::Value> &args) {
    args.GetReturnValue().Set(v8s(args.GetIsolate(), Ns_InfoHostname()));
}

static void JsInfoAddress(const v8::FunctionCallbackInfo<v8::Value> &args) {
    args.GetReturnValue().Set(v8s(args.GetIsolate(), Ns_InfoAddress()));
}

static void JsInfoPid(const v8::FunctionCallbackInfo<v8::Value> &args) {
    args.GetReturnValue().Set(Ns_InfoPid());
}

static void JsInfoServerName(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsMod *mod = GetMod(args);
    if (mod == nullptr) { args.GetReturnValue().SetNull(); return; }
    args.GetReturnValue().Set(v8s(iso, mod->server));
}

/* -----------------------------------------------------------------------
 * ns.time / ns.time.* callbacks
 * --------------------------------------------------------------------- */

/* ns.time() — current epoch seconds */
static void JsTimeNow(const v8::FunctionCallbackInfo<v8::Value> &args) {
    args.GetReturnValue().Set(static_cast<double>(time(nullptr)));
}

/* ns.time.format(t, fmt) — strftime */
static void JsTimeFormat(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().SetNull(); return; }
    time_t t = static_cast<time_t>(
        args[0]->IntegerValue(iso->GetCurrentContext()).FromMaybe(0));
    std::string fmt = V8ToString(iso, args[1]);
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    char buf[512];
    strftime(buf, sizeof(buf), fmt.c_str(), &tm_buf);
    args.GetReturnValue().Set(v8s(iso, buf));
}

/* ns.time.httpTime(t) — HTTP-format date string */
static void JsTimeHttpTime(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    time_t t = (args.Length() >= 1)
        ? static_cast<time_t>(
              args[0]->IntegerValue(iso->GetCurrentContext()).FromMaybe(0))
        : time(nullptr);
    Ns_DString ds;
    Ns_DStringInit(&ds);
    Ns_HttpTime(&ds, &t);
    args.GetReturnValue().Set(v8s(iso, ds.string));
    Ns_DStringFree(&ds);
}

/* ns.time.parseHttpTime(str) — parse HTTP date, returns epoch or null.
 * Tries multiple RFC formats since Ns_ParseHttpTime is unreliable on macOS.
 */
static void JsTimeParseHttpTime(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().SetNull(); return; }
    std::string s = V8ToString(iso, args[0]);
    static const char *formats[] = {
        "%a, %d %b %Y %H:%M:%S %Z",   /* RFC 1123: Fri, 06 Mar 2026 16:55:17 GMT */
        "%A, %d-%b-%y %H:%M:%S %Z",    /* RFC 850:  Friday, 06-Mar-26 16:55:17 GMT */
        "%a %b %d %H:%M:%S %Y",        /* ANSI C:   Fri Mar  6 16:55:17 2026 */
        nullptr
    };
    struct tm tm_buf;
    for (int i = 0; formats[i] != nullptr; i++) {
        memset(&tm_buf, 0, sizeof(tm_buf));
        if (strptime(s.c_str(), formats[i], &tm_buf) != nullptr) {
            time_t t = timegm(&tm_buf);
            args.GetReturnValue().Set(static_cast<double>(t));
            return;
        }
    }
    args.GetReturnValue().SetNull();
}

/* ns.time.gmtime(t) — returns {year,month,day,hour,min,sec,wday,yday} */
static void JsTimeGmtime(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    time_t t = (args.Length() >= 1)
        ? static_cast<time_t>(
              args[0]->IntegerValue(iso->GetCurrentContext()).FromMaybe(0))
        : time(nullptr);
    struct tm tm_buf;
    gmtime_r(&t, &tm_buf);
    v8::Local<v8::Object> obj = v8::Object::New(iso);
    auto ctx = iso->GetCurrentContext();
    obj->Set(ctx, v8s(iso,"year"),  v8::Integer::New(iso, tm_buf.tm_year+1900)).Check();
    obj->Set(ctx, v8s(iso,"month"), v8::Integer::New(iso, tm_buf.tm_mon+1)).Check();
    obj->Set(ctx, v8s(iso,"day"),   v8::Integer::New(iso, tm_buf.tm_mday)).Check();
    obj->Set(ctx, v8s(iso,"hour"),  v8::Integer::New(iso, tm_buf.tm_hour)).Check();
    obj->Set(ctx, v8s(iso,"min"),   v8::Integer::New(iso, tm_buf.tm_min)).Check();
    obj->Set(ctx, v8s(iso,"sec"),   v8::Integer::New(iso, tm_buf.tm_sec)).Check();
    obj->Set(ctx, v8s(iso,"wday"),  v8::Integer::New(iso, tm_buf.tm_wday)).Check();
    obj->Set(ctx, v8s(iso,"yday"),  v8::Integer::New(iso, tm_buf.tm_yday)).Check();
    args.GetReturnValue().Set(obj);
}

/* ns.time.localtime(t) — same structure but local time */
static void JsTimeLocaltime(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    time_t t = (args.Length() >= 1)
        ? static_cast<time_t>(
              args[0]->IntegerValue(iso->GetCurrentContext()).FromMaybe(0))
        : time(nullptr);
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    v8::Local<v8::Object> obj = v8::Object::New(iso);
    auto ctx = iso->GetCurrentContext();
    obj->Set(ctx, v8s(iso,"year"),  v8::Integer::New(iso, tm_buf.tm_year+1900)).Check();
    obj->Set(ctx, v8s(iso,"month"), v8::Integer::New(iso, tm_buf.tm_mon+1)).Check();
    obj->Set(ctx, v8s(iso,"day"),   v8::Integer::New(iso, tm_buf.tm_mday)).Check();
    obj->Set(ctx, v8s(iso,"hour"),  v8::Integer::New(iso, tm_buf.tm_hour)).Check();
    obj->Set(ctx, v8s(iso,"min"),   v8::Integer::New(iso, tm_buf.tm_min)).Check();
    obj->Set(ctx, v8s(iso,"sec"),   v8::Integer::New(iso, tm_buf.tm_sec)).Check();
    obj->Set(ctx, v8s(iso,"wday"),  v8::Integer::New(iso, tm_buf.tm_wday)).Check();
    obj->Set(ctx, v8s(iso,"yday"),  v8::Integer::New(iso, tm_buf.tm_yday)).Check();
    args.GetReturnValue().Set(obj);
}

/* -----------------------------------------------------------------------
 * ns.url.* callbacks
 * --------------------------------------------------------------------- */

/* ns.url.encode(str) */
static void JsUrlEncode(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(v8s(iso,"")); return; }
    std::string s = V8ToString(iso, args[0]);
    Ns_DString ds;
    Ns_DStringInit(&ds);
    Ns_EncodeUrlCharset(&ds, nc(s), nullptr);
    args.GetReturnValue().Set(v8s(iso, ds.string));
    Ns_DStringFree(&ds);
}

/* ns.url.decode(str) */
static void JsUrlDecode(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(v8s(iso,"")); return; }
    std::string s = V8ToString(iso, args[0]);
    Ns_DString ds;
    Ns_DStringInit(&ds);
    Ns_DecodeUrlCharset(&ds, nc(s), nullptr);
    args.GetReturnValue().Set(v8s(iso, ds.string));
    Ns_DStringFree(&ds);
}

/* ns.url.parse(queryStr) — parse query string into JS object */
static void JsUrlParse(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(v8::Object::New(iso)); return; }
    std::string qs = V8ToString(iso, args[0]);
    v8::Local<v8::Object> obj = v8::Object::New(iso);
    auto ctx = iso->GetCurrentContext();

    size_t pos = 0;
    while (pos <= qs.size()) {
        size_t amp = qs.find('&', pos);
        if (amp == std::string::npos) amp = qs.size();
        std::string pair = qs.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        if (!pair.empty()) {
            std::string rawKey   = (eq != std::string::npos) ? pair.substr(0, eq) : pair;
            std::string rawValue = (eq != std::string::npos) ? pair.substr(eq+1) : "";
            /* decode key */
            Ns_DString dsk, dsv;
            Ns_DStringInit(&dsk);
            Ns_DStringInit(&dsv);
            Ns_DecodeUrlCharset(&dsk, nc(rawKey),   nullptr);
            Ns_DecodeUrlCharset(&dsv, nc(rawValue), nullptr);
            obj->Set(ctx, v8s(iso, dsk.string), v8s(iso, dsv.string)).Check();
            Ns_DStringFree(&dsk);
            Ns_DStringFree(&dsv);
        }
        pos = amp + 1;
    }
    args.GetReturnValue().Set(obj);
}

/* ns.url.toFile(url) — map URL to filesystem path */
static void JsUrlToFile(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().SetNull(); return; }
    NsMod *mod = GetMod(args);
    if (mod == nullptr) { args.GetReturnValue().SetNull(); return; }
    std::string url = V8ToString(iso, args[0]);
    Ns_DString ds;
    Ns_DStringInit(&ds);
    if (Ns_UrlToFile(&ds, mod->server, nc(url)) == NS_OK) {
        args.GetReturnValue().Set(v8s(iso, ds.string));
    } else {
        args.GetReturnValue().SetNull();
    }
    Ns_DStringFree(&ds);
}

/* -----------------------------------------------------------------------
 * ns.html.* callbacks
 * --------------------------------------------------------------------- */

/* ns.html.quote(str) — escape HTML entities */
static void JsHtmlQuote(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(v8s(iso,"")); return; }
    std::string s = V8ToString(iso, args[0]);
    Ns_DString ds;
    Ns_DStringInit(&ds);
    Ns_QuoteHtml(&ds, nc(s));
    args.GetReturnValue().Set(v8s(iso, ds.string));
    Ns_DStringFree(&ds);
}

/* ns.html.guessType(filename) — MIME type from extension */
static void JsHtmlGuessType(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().SetNull(); return; }
    std::string fname = V8ToString(iso, args[0]);
    char *mime = Ns_GetMimeType(nc(fname));
    args.GetReturnValue().Set(v8s(iso, mime ? mime : "application/octet-stream"));
}

/* -----------------------------------------------------------------------
 * ns.file.* callbacks — POSIX file operations
 * --------------------------------------------------------------------- */

static bool RecursiveMkdir(const std::string &path) {
    if (mkdir(path.c_str(), 0755) == 0) return true;
    if (errno == EEXIST) return true;
    if (errno == ENOENT) {
        size_t pos = path.rfind('/');
        if (pos == std::string::npos || pos == 0) return false;
        if (!RecursiveMkdir(path.substr(0, pos))) return false;
        return mkdir(path.c_str(), 0755) == 0;
    }
    return false;
}

static bool FileCopy(const std::string &src, const std::string &dst) {
    FILE *fin  = fopen(src.c_str(), "rb");
    if (!fin)  return false;
    FILE *fout = fopen(dst.c_str(), "wb");
    if (!fout) { fclose(fin); return false; }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
        fwrite(buf, 1, n, fout);
    }
    fclose(fin);
    fclose(fout);
    return true;
}

/* ns.file.read(path) */
static void JsFileRead(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().SetNull(); return; }
    std::string path = V8ToString(iso, args[0]);
    std::ifstream ifs(path);
    if (!ifs) { args.GetReturnValue().SetNull(); return; }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    args.GetReturnValue().Set(v8s(iso, ss.str()));
}

/* ns.file.write(path, content) */
static void JsFileWrite(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().Set(false); return; }
    std::string path    = V8ToString(iso, args[0]);
    std::string content = V8ToString(iso, args[1]);
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) { args.GetReturnValue().Set(false); return; }
    fwrite(content.c_str(), 1, content.size(), f);
    fclose(f);
    args.GetReturnValue().Set(true);
}

/* ns.file.exists(path) */
static void JsFileExists(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(false); return; }
    std::string path = V8ToString(iso, args[0]);
    struct stat st;
    args.GetReturnValue().Set(stat(path.c_str(), &st) == 0);
}

/* ns.file.stat(path) — returns {size, mtime, isFile, isDir} or null */
static void JsFileStat(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().SetNull(); return; }
    std::string path = V8ToString(iso, args[0]);
    struct stat st;
    if (stat(path.c_str(), &st) != 0) { args.GetReturnValue().SetNull(); return; }
    v8::Local<v8::Object> obj = v8::Object::New(iso);
    auto ctx = iso->GetCurrentContext();
    obj->Set(ctx, v8s(iso,"size"),   v8::Number::New(iso, static_cast<double>(st.st_size))).Check();
    obj->Set(ctx, v8s(iso,"mtime"),  v8::Number::New(iso, static_cast<double>(st.st_mtime))).Check();
    obj->Set(ctx, v8s(iso,"isFile"), v8::Boolean::New(iso, S_ISREG(st.st_mode))).Check();
    obj->Set(ctx, v8s(iso,"isDir"),  v8::Boolean::New(iso, S_ISDIR(st.st_mode))).Check();
    args.GetReturnValue().Set(obj);
}

/* ns.file.mkdir(path) */
static void JsFileMkdir(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(false); return; }
    std::string path = V8ToString(iso, args[0]);
    args.GetReturnValue().Set(RecursiveMkdir(path));
}

/* ns.file.rmdir(path) */
static void JsFileRmdir(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(false); return; }
    std::string path = V8ToString(iso, args[0]);
    args.GetReturnValue().Set(rmdir(path.c_str()) == 0);
}

/* ns.file.unlink(path) */
static void JsFileUnlink(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(false); return; }
    std::string path = V8ToString(iso, args[0]);
    args.GetReturnValue().Set(unlink(path.c_str()) == 0);
}

/* ns.file.cp(src, dst) */
static void JsFileCp(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().Set(false); return; }
    args.GetReturnValue().Set(
        FileCopy(V8ToString(iso, args[0]), V8ToString(iso, args[1])));
}

/* ns.file.rename(src, dst) */
static void JsFileRename(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().Set(false); return; }
    std::string src = V8ToString(iso, args[0]);
    std::string dst = V8ToString(iso, args[1]);
    args.GetReturnValue().Set(rename(src.c_str(), dst.c_str()) == 0);
}

/* ns.file.tmpnam() — returns a unique temp file path */
static void JsFileTmpnam(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    char tmpl[] = "/tmp/nsjs_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) close(fd);
    args.GetReturnValue().Set(v8s(iso, tmpl));
}

/* ns.file.normalizePath(path) */
static void JsFileNormalizePath(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().SetNull(); return; }
    std::string path = V8ToString(iso, args[0]);
    Ns_DString ds;
    Ns_DStringInit(&ds);
    Ns_NormalizePath(&ds, nc(path));
    args.GetReturnValue().Set(v8s(iso, ds.string));
    Ns_DStringFree(&ds);
}

/* -----------------------------------------------------------------------
 * ns.dns.* callbacks
 * --------------------------------------------------------------------- */

/* ns.dns.addrByHost(hostname) — returns IP string or null */
static void JsDnsAddrByHost(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().SetNull(); return; }
    std::string host = V8ToString(iso, args[0]);
    Ns_DString ds;
    Ns_DStringInit(&ds);
    if (Ns_GetAddrByHost(&ds, nc(host)) == NS_TRUE) {
        args.GetReturnValue().Set(v8s(iso, ds.string));
    } else {
        args.GetReturnValue().SetNull();
    }
    Ns_DStringFree(&ds);
}

/* ns.dns.hostByAddr(addr) — returns hostname string or null */
static void JsDnsHostByAddr(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().SetNull(); return; }
    std::string addr = V8ToString(iso, args[0]);
    Ns_DString ds;
    Ns_DStringInit(&ds);
    if (Ns_GetHostByAddr(&ds, nc(addr)) == NS_TRUE) {
        args.GetReturnValue().Set(v8s(iso, ds.string));
    } else {
        args.GetReturnValue().SetNull();
    }
    Ns_DStringFree(&ds);
}

/* -----------------------------------------------------------------------
 * ns.sched.* — scheduling with JS execution on scheduler thread
 *
 * The JS code string is executed in a fresh V8 isolate; conn APIs return
 * null since there is no active request.
 * --------------------------------------------------------------------- */

struct SchedOnceCtx {
    NsMod       *modPtr;
    std::string  jsCode;
};

static void SchedRunJs(const std::string &jsCode) {
    if (!platform_initialized) return;
    NsAllocator *alloc = new NsAllocator();
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = alloc;
    v8::Isolate *iso = v8::Isolate::New(params);
    {
        v8::Isolate::Scope isc(iso);
        v8::HandleScope   hs(iso);
        v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(iso);
        BuildGlobalTemplate(iso, tmpl);
        v8::Local<v8::Context> v8ctx = v8::Context::New(iso, nullptr, tmpl);
        v8::Context::Scope cs(v8ctx);
        v8::TryCatch tc(iso);
        v8::Local<v8::String> src =
            v8::String::NewFromUtf8(iso, jsCode.c_str()).ToLocalChecked();
        v8::Local<v8::Script> script;
        if (v8::Script::Compile(v8ctx, src).ToLocal(&script)) {
            v8::Local<v8::Value> result;
            if (!script->Run(v8ctx).ToLocal(&result)) {
                v8::String::Utf8Value err(iso, tc.Exception());
                Ns_Log(Error, nc("nsjs sched: %s"), *err ? *err : "unknown error");
            }
        } else {
            v8::String::Utf8Value err(iso, tc.Exception());
            Ns_Log(Error, nc("nsjs sched compile: %s"), *err ? *err : "unknown error");
        }
    }
    iso->Dispose();
    delete alloc;
}

static void SchedOnceFire(void *arg) {
    SchedOnceCtx *ctx = static_cast<SchedOnceCtx *>(arg);
    SchedRunJs(ctx->jsCode);
    /* SchedOnceDelete will free ctx */
}

static void SchedOnceDelete(void *arg) {
    delete static_cast<SchedOnceCtx *>(arg);
}

struct SchedRepeatCtx {
    NsMod       *modPtr;
    std::string  jsCode;
};

static void SchedRepeatFire(void *arg) {
    SchedRepeatCtx *ctx = static_cast<SchedRepeatCtx *>(arg);
    SchedRunJs(ctx->jsCode);
}

/* ns.sched.after(seconds, jsCode) — one-shot, returns schedule ID */
static void JsSchedAfter(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().Set(-1); return; }
    int seconds = static_cast<int>(
        args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(0));
    std::string jsCode = V8ToString(iso, args[1]);

    SchedOnceCtx *ctx = new SchedOnceCtx();
    ctx->modPtr = GetMod(args);
    ctx->jsCode = jsCode;

    int id = Ns_After(seconds, SchedOnceFire, ctx, SchedOnceDelete);
    args.GetReturnValue().Set(id);
}

/* ns.sched.interval(seconds, jsCode) — recurring, returns schedule ID */
static void JsSchedInterval(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().Set(-1); return; }
    int seconds = static_cast<int>(
        args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(0));
    std::string jsCode = V8ToString(iso, args[1]);

    SchedRepeatCtx *ctx = new SchedRepeatCtx();
    ctx->modPtr = GetMod(args);
    ctx->jsCode = jsCode;

    int id = Ns_ScheduleProc(SchedRepeatFire, ctx, 1, seconds);
    if (id >= 0) {
        Ns_MutexLock(&js_sched_map_lock);
        (*js_sched_map)[id] = ctx;
        Ns_MutexUnlock(&js_sched_map_lock);
    } else {
        delete ctx;
    }
    args.GetReturnValue().Set(id);
}

/* ns.sched.cancel(id) */
static void JsSchedCancel(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) return;
    int id = static_cast<int>(
        args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    if (id < 0) return;
    Ns_UnscheduleProc(id);
    /* Free the repeat ctx if present */
    Ns_MutexLock(&js_sched_map_lock);
    auto it = js_sched_map->find(id);
    if (it != js_sched_map->end()) {
        delete it->second;
        js_sched_map->erase(it);
    }
    Ns_MutexUnlock(&js_sched_map_lock);
}

/* -----------------------------------------------------------------------
 * ns.mutex.* callbacks — integer handle -> Ns_Mutex*
 * --------------------------------------------------------------------- */

/* ns.mutex.create() — returns integer handle */
static void JsMutexCreate(const v8::FunctionCallbackInfo<v8::Value> &args) {
    Ns_MutexLock(&js_mutex_map_lock);
    int id = ++js_mutex_next_id;
    Ns_Mutex *m = static_cast<Ns_Mutex *>(ns_calloc(1, sizeof(Ns_Mutex)));
    Ns_MutexInit(m);
    (*js_mutex_map)[id] = m;
    Ns_MutexUnlock(&js_mutex_map_lock);
    args.GetReturnValue().Set(id);
}

static Ns_Mutex *LookupMutex(int id) {
    Ns_MutexLock(&js_mutex_map_lock);
    auto it = js_mutex_map->find(id);
    Ns_Mutex *m = (it != js_mutex_map->end()) ? it->second : nullptr;
    Ns_MutexUnlock(&js_mutex_map_lock);
    return m;
}

/* ns.mutex.lock(id) */
static void JsMutexLock(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) return;
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_Mutex *m = LookupMutex(id);
    if (m) Ns_MutexLock(m);
}

/* ns.mutex.unlock(id) */
static void JsMutexUnlock(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) return;
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_Mutex *m = LookupMutex(id);
    if (m) Ns_MutexUnlock(m);
}

/* ns.mutex.trylock(id) — returns boolean */
static void JsMutexTryLock(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(false); return; }
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_Mutex *m = LookupMutex(id);
    if (m == nullptr) { args.GetReturnValue().Set(false); return; }
    args.GetReturnValue().Set(Ns_MutexTryLock(m) == NS_OK);
}

/* ns.mutex.destroy(id) */
static void JsMutexDestroy(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) return;
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_MutexLock(&js_mutex_map_lock);
    auto it = js_mutex_map->find(id);
    if (it != js_mutex_map->end()) {
        Ns_MutexDestroy(it->second);
        ns_free(it->second);
        js_mutex_map->erase(it);
    }
    Ns_MutexUnlock(&js_mutex_map_lock);
}

/* -----------------------------------------------------------------------
 * ns.rwlock.* callbacks — integer handle -> Ns_RWLock*
 * --------------------------------------------------------------------- */

/* ns.rwlock.create() — returns integer handle */
static void JsRWLockCreate(const v8::FunctionCallbackInfo<v8::Value> &args) {
    Ns_MutexLock(&js_rwlock_map_lock);
    int id = ++js_rwlock_next_id;
    Ns_RWLock *rw = static_cast<Ns_RWLock *>(ns_calloc(1, sizeof(Ns_RWLock)));
    Ns_RWLockInit(rw);
    (*js_rwlock_map)[id] = rw;
    Ns_MutexUnlock(&js_rwlock_map_lock);
    args.GetReturnValue().Set(id);
}

static Ns_RWLock *LookupRWLock(int id) {
    Ns_MutexLock(&js_rwlock_map_lock);
    auto it = js_rwlock_map->find(id);
    Ns_RWLock *rw = (it != js_rwlock_map->end()) ? it->second : nullptr;
    Ns_MutexUnlock(&js_rwlock_map_lock);
    return rw;
}

/* ns.rwlock.readLock(id) */
static void JsRWLockReadLock(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) return;
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_RWLock *rw = LookupRWLock(id);
    if (rw) Ns_RWLockRdLock(rw);
}

/* ns.rwlock.writeLock(id) */
static void JsRWLockWriteLock(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) return;
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_RWLock *rw = LookupRWLock(id);
    if (rw) Ns_RWLockWrLock(rw);
}

/* ns.rwlock.unlock(id) */
static void JsRWLockUnlock(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) return;
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_RWLock *rw = LookupRWLock(id);
    if (rw) Ns_RWLockUnlock(rw);
}

/* ns.rwlock.destroy(id) */
static void JsRWLockDestroy(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) return;
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_MutexLock(&js_rwlock_map_lock);
    auto it = js_rwlock_map->find(id);
    if (it != js_rwlock_map->end()) {
        Ns_RWLockDestroy(it->second);
        ns_free(it->second);
        js_rwlock_map->erase(it);
    }
    Ns_MutexUnlock(&js_rwlock_map_lock);
}

/* -----------------------------------------------------------------------
 * ns.sleep(ms) — sleep milliseconds
 * --------------------------------------------------------------------- */
static void JsSleep(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) return;
    int ms = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(0));
    if (ms > 0) usleep(static_cast<useconds_t>(ms) * 1000u);
}

/* -----------------------------------------------------------------------
 * ns.crypt(key, salt) — DES crypt
 * --------------------------------------------------------------------- */
static void JsCrypt(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().SetNull(); return; }
    std::string key  = V8ToString(iso, args[0]);
    std::string salt = V8ToString(iso, args[1]);
    char buf[NS_ENCRYPT_BUFSIZE];
    char *result = Ns_Encrypt(nc(key.c_str()), nc(salt.c_str()), buf);
    if (result) args.GetReturnValue().Set(v8s(iso, result));
    else        args.GetReturnValue().SetNull();
}

/* -----------------------------------------------------------------------
 * ns.rand([max]) — wraps Ns_DRand()
 *   no arg  -> raw double in [0.0, 1.0)
 *   max arg -> integer in [0, max)
 * --------------------------------------------------------------------- */
static void JsRand(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    double r = Ns_DRand();
    if (args.Length() >= 1 && !args[0]->IsUndefined() && !args[0]->IsNull()) {
        double max = args[0]->NumberValue(iso->GetCurrentContext()).FromMaybe(0.0);
        args.GetReturnValue().Set(static_cast<int32_t>(r * max));
    } else {
        args.GetReturnValue().Set(r);
    }
}

/* -----------------------------------------------------------------------
 * ns.atshutdown(jsCode) / ns.atsignal(jsCode)
 * --------------------------------------------------------------------- */

struct JsAtCtx {
    std::string jsCode;
};

static void JsAtShutdownProc(void *arg) {
    JsAtCtx *ctx = static_cast<JsAtCtx *>(arg);
    SchedRunJs(ctx->jsCode);
    delete ctx;
}

static void JsAtSignalProc(void *arg) {
    JsAtCtx *ctx = static_cast<JsAtCtx *>(arg);
    SchedRunJs(ctx->jsCode);
    delete ctx;
}

static void JsAtShutdown(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) return;
    std::string jsCode = V8ToString(iso, args[0]);
    JsAtCtx *ctx = new JsAtCtx();
    ctx->jsCode = jsCode;
    Ns_RegisterAtShutdown(JsAtShutdownProc, ctx);
}

static void JsAtSignal(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) return;
    std::string jsCode = V8ToString(iso, args[0]);
    JsAtCtx *ctx = new JsAtCtx();
    ctx->jsCode = jsCode;
    Ns_RegisterAtSignal(JsAtSignalProc, ctx);
}

/* -----------------------------------------------------------------------
 * ns.env.get/set/unset/names — environment variable access
 * --------------------------------------------------------------------- */
static void JsEnvGet(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().SetNull(); return; }
    std::string name = V8ToString(iso, args[0]);
    const char *val = getenv(name.c_str());
    if (val) args.GetReturnValue().Set(v8s(iso, val));
    else     args.GetReturnValue().SetNull();
}

static void JsEnvSet(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().Set(false); return; }
    std::string name = V8ToString(iso, args[0]);
    std::string val  = V8ToString(iso, args[1]);
    args.GetReturnValue().Set(setenv(name.c_str(), val.c_str(), 1) == 0);
}

static void JsEnvUnset(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(false); return; }
    std::string name = V8ToString(iso, args[0]);
    args.GetReturnValue().Set(unsetenv(name.c_str()) == 0);
}

extern char **environ;
static void JsEnvNames(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::Array> arr = v8::Array::New(iso);
    uint32_t idx = 0;
    for (char **ep = environ; ep && *ep; ++ep) {
        const char *eq = strchr(*ep, '=');
        if (eq) {
            std::string name(*ep, static_cast<size_t>(eq - *ep));
            arr->Set(ctx, idx++, v8s(iso, name)).Check();
        }
    }
    args.GetReturnValue().Set(arr);
}

/* -----------------------------------------------------------------------
 * ns.file extensions: chmod, link, symlink, truncate, roll, purge
 * --------------------------------------------------------------------- */
static void JsFileChmod(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().Set(false); return; }
    std::string path = V8ToString(iso, args[0]);
    int mode = static_cast<int>(args[1]->Int32Value(iso->GetCurrentContext()).FromMaybe(0));
    args.GetReturnValue().Set(chmod(path.c_str(), static_cast<mode_t>(mode)) == 0);
}

static void JsFileLink(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().Set(false); return; }
    std::string src = V8ToString(iso, args[0]);
    std::string dst = V8ToString(iso, args[1]);
    args.GetReturnValue().Set(link(src.c_str(), dst.c_str()) == 0);
}

static void JsFileSymlink(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().Set(false); return; }
    std::string src = V8ToString(iso, args[0]);
    std::string dst = V8ToString(iso, args[1]);
    args.GetReturnValue().Set(symlink(src.c_str(), dst.c_str()) == 0);
}

static void JsFileTruncate(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(false); return; }
    std::string path = V8ToString(iso, args[0]);
    off_t len = 0;
    if (args.Length() >= 2)
        len = static_cast<off_t>(args[1]->NumberValue(iso->GetCurrentContext()).FromMaybe(0.0));
    args.GetReturnValue().Set(truncate(path.c_str(), len) == 0);
}

static void JsFileRoll(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(false); return; }
    std::string path = V8ToString(iso, args[0]);
    int max = 10;
    if (args.Length() >= 2)
        max = static_cast<int>(args[1]->Int32Value(iso->GetCurrentContext()).FromMaybe(10));
    args.GetReturnValue().Set(Ns_RollFile(nc(path.c_str()), max) == NS_OK);
}

static void JsFilePurge(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(false); return; }
    std::string path = V8ToString(iso, args[0]);
    int max = 10;
    if (args.Length() >= 2)
        max = static_cast<int>(args[1]->Int32Value(iso->GetCurrentContext()).FromMaybe(10));
    args.GetReturnValue().Set(Ns_PurgeFiles(nc(path.c_str()), max) == NS_OK);
}

/* -----------------------------------------------------------------------
 * ns.image.gifSize(path) -> {width, height} or null
 * Reads the 6-byte GIF header + logical screen descriptor width/height.
 * --------------------------------------------------------------------- */
static void JsImageGifSize(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().SetNull(); return; }
    std::string path = V8ToString(iso, args[0]);
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) { args.GetReturnValue().SetNull(); return; }
    unsigned char hdr[10];
    if (fread(hdr, 1, 10, fp) < 10) { fclose(fp); args.GetReturnValue().SetNull(); return; }
    fclose(fp);
    if (hdr[0] != 'G' || hdr[1] != 'I' || hdr[2] != 'F') {
        args.GetReturnValue().SetNull(); return;
    }
    int w = hdr[6] | (hdr[7] << 8);
    int h = hdr[8] | (hdr[9] << 8);
    v8::Local<v8::Object> obj = v8::Object::New(iso);
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    obj->Set(ctx, v8s(iso, "width"),  v8::Integer::New(iso, w)).Check();
    obj->Set(ctx, v8s(iso, "height"), v8::Integer::New(iso, h)).Check();
    args.GetReturnValue().Set(obj);
}

/* -----------------------------------------------------------------------
 * ns.image.jpegSize(path) -> {width, height} or null
 * Scans JPEG SOF markers to find image dimensions.
 * --------------------------------------------------------------------- */
static void JsImageJpegSize(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().SetNull(); return; }
    std::string path = V8ToString(iso, args[0]);
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) { args.GetReturnValue().SetNull(); return; }
    unsigned char c;
    /* Check SOI marker */
    if (fgetc(fp) != 0xFF || fgetc(fp) != 0xD8) { fclose(fp); args.GetReturnValue().SetNull(); return; }
    while (!feof(fp)) {
        /* Find next marker */
        while ((c = static_cast<unsigned char>(fgetc(fp))) != 0xFF && !feof(fp)) {}
        while ((c = static_cast<unsigned char>(fgetc(fp))) == 0xFF && !feof(fp)) {}
        if (feof(fp)) break;
        unsigned char marker = c;
        /* Read segment length */
        int len = (fgetc(fp) << 8) | fgetc(fp);
        /* SOF markers: C0-C3, C5-C7, C9-CB, CD-CF */
        if ((marker >= 0xC0 && marker <= 0xC3) ||
            (marker >= 0xC5 && marker <= 0xC7) ||
            (marker >= 0xC9 && marker <= 0xCB) ||
            (marker >= 0xCD && marker <= 0xCF)) {
            fgetc(fp); /* precision */
            int h = (fgetc(fp) << 8) | fgetc(fp);
            int w = (fgetc(fp) << 8) | fgetc(fp);
            fclose(fp);
            v8::Local<v8::Object> obj = v8::Object::New(iso);
            v8::Local<v8::Context> ctx = iso->GetCurrentContext();
            obj->Set(ctx, v8s(iso, "width"),  v8::Integer::New(iso, w)).Check();
            obj->Set(ctx, v8s(iso, "height"), v8::Integer::New(iso, h)).Check();
            args.GetReturnValue().Set(obj);
            return;
        }
        /* Skip segment */
        fseek(fp, len - 2, SEEK_CUR);
    }
    fclose(fp);
    args.GetReturnValue().SetNull();
}

/* -----------------------------------------------------------------------
 * ns.html.hrefs(html) -> array of href values
 * --------------------------------------------------------------------- */
static void JsHtmlHrefs(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(v8::Array::New(iso)); return; }
    std::string html = V8ToString(iso, args[0]);
    v8::Local<v8::Array> arr = v8::Array::New(iso);
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    uint32_t idx = 0;
    /* Work on a mutable copy */
    std::vector<char> buf(html.begin(), html.end());
    buf.push_back('\0');
    char *p = buf.data();
    while (true) {
        char *s = strchr(p, '<');
        if (!s) break;
        char *e = strchr(s, '>');
        if (!e) break;
        ++s;
        *e = '\0';
        while (*s && isspace(static_cast<unsigned char>(*s))) ++s;
        if ((*s == 'a' || *s == 'A') && isspace(static_cast<unsigned char>(s[1]))) {
            ++s;
            while (*s) {
                if (strncasecmp(s, "href", 4) == 0) {
                    s += 4;
                    while (*s && isspace(static_cast<unsigned char>(*s))) ++s;
                    if (*s == '=') {
                        ++s;
                        while (*s && isspace(static_cast<unsigned char>(*s))) ++s;
                        char *he = nullptr;
                        if (*s == '\'' || *s == '"') {
                            he = strchr(s + 1, *s);
                            ++s;
                        }
                        if (!he) {
                            he = s;
                            while (*he && !isspace(static_cast<unsigned char>(*he))) ++he;
                        }
                        char save = *he;
                        *he = '\0';
                        arr->Set(ctx, idx++, v8s(iso, s)).Check();
                        *he = save;
                        break;
                    }
                }
                ++s;
            }
        }
        *e = '>';
        p = e + 1;
    }
    args.GetReturnValue().Set(arr);
}

/* -----------------------------------------------------------------------
 * ns.process.kill(pid, sig) — send signal to process
 * --------------------------------------------------------------------- */
static void JsProcessKill(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(false); return; }
    int pid = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(0));
    int sig = SIGTERM;
    if (args.Length() >= 2)
        sig = static_cast<int>(args[1]->Int32Value(iso->GetCurrentContext()).FromMaybe(SIGTERM));
    args.GetReturnValue().Set(kill(static_cast<pid_t>(pid), sig) == 0);
}

/* -----------------------------------------------------------------------
 * ns.log.roll() — roll the server log file
 * --------------------------------------------------------------------- */
static void JsLogRoll(const v8::FunctionCallbackInfo<v8::Value> &/*args*/) {
    Ns_LogRoll();
}

/* -----------------------------------------------------------------------
 * ns.config.section(name) -> JS object of key/value pairs, or null
 * ns.config.sections()    -> array of section name strings
 * --------------------------------------------------------------------- */
static void JsConfigSection(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().SetNull(); return; }
    std::string name = V8ToString(iso, args[0]);
    Ns_Set *set = Ns_ConfigGetSection(nc(name.c_str()));
    if (!set) { args.GetReturnValue().SetNull(); return; }
    v8::Local<v8::Object> obj = v8::Object::New(iso);
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    for (int i = 0; i < Ns_SetSize(set); ++i) {
        const char *k = Ns_SetKey(set, i);
        const char *v = Ns_SetValue(set, i);
        if (k) obj->Set(ctx, v8s(iso, k), v8s(iso, v ? v : "")).Check();
    }
    args.GetReturnValue().Set(obj);
}

static void JsConfigSections(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    v8::Local<v8::Array> arr = v8::Array::New(iso);
    Ns_Set **sets = Ns_ConfigGetSections();
    uint32_t idx = 0;
    if (sets) {
        for (int i = 0; sets[i] != nullptr; ++i) {
            const char *name = sets[i]->name;
            if (name) arr->Set(ctx, idx++, v8s(iso, name)).Check();
        }
    }
    args.GetReturnValue().Set(arr);
}

/* -----------------------------------------------------------------------
 * Extended ns.conn.* response callbacks
 * --------------------------------------------------------------------- */

/* ns.conn.return(status, type, body) */
static void JsConnReturn(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (!ctx || !ctx->conn || args.Length() < 3) return;
    int status = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(200));
    std::string type = V8ToString(iso, args[1]);
    std::string body = V8ToString(iso, args[2]);
    ctx->responseSent = true;
    Ns_ConnReturnCharData(ctx->conn, status, nc(body.c_str()),
                          static_cast<int>(body.size()), nc(type.c_str()));
}

/* ns.conn.returnBadRequest(reason) */
static void JsConnReturnBadRequest(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (!ctx || !ctx->conn) return;
    std::string reason;
    if (args.Length() >= 1) reason = V8ToString(iso, args[0]);
    ctx->responseSent = true;
    Ns_ConnReturnBadRequest(ctx->conn, reason.empty() ? nullptr : nc(reason.c_str()));
}

/* ns.conn.returnForbidden() */
static void JsConnReturnForbidden(const v8::FunctionCallbackInfo<v8::Value> &args) {
    NsJsContext *ctx = GetCtx(args);
    if (!ctx || !ctx->conn) return;
    ctx->responseSent = true;
    Ns_ConnReturnForbidden(ctx->conn);
}

/* ns.conn.returnNotFound() */
static void JsConnReturnNotFound(const v8::FunctionCallbackInfo<v8::Value> &args) {
    NsJsContext *ctx = GetCtx(args);
    if (!ctx || !ctx->conn) return;
    ctx->responseSent = true;
    Ns_ConnReturnNotFound(ctx->conn);
}

/* ns.conn.returnUnauthorized() */
static void JsConnReturnUnauthorized(const v8::FunctionCallbackInfo<v8::Value> &args) {
    NsJsContext *ctx = GetCtx(args);
    if (!ctx || !ctx->conn) return;
    ctx->responseSent = true;
    Ns_ConnReturnUnauthorized(ctx->conn);
}

/* ns.conn.returnNotice(status, title, msg) */
static void JsConnReturnNotice(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (!ctx || !ctx->conn || args.Length() < 3) return;
    int status = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(200));
    std::string title = V8ToString(iso, args[1]);
    std::string msg   = V8ToString(iso, args[2]);
    ctx->responseSent = true;
    Ns_ConnReturnNotice(ctx->conn, status, nc(title.c_str()), nc(msg.c_str()));
}

/* ns.conn.returnError(status, title, body) */
static void JsConnReturnError(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (!ctx || !ctx->conn || args.Length() < 3) return;
    int status = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(500));
    std::string title = V8ToString(iso, args[1]);
    std::string body  = V8ToString(iso, args[2]);
    ctx->responseSent = true;
    Ns_ConnReturnAdminNotice(ctx->conn, status, nc(title.c_str()), nc(body.c_str()));
}

/* ns.conn.headers(status, type, len) — flush response headers only */
static void JsConnHeaders(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (!ctx || !ctx->conn) return;
    int status = 200;
    const char *type = nullptr;
    int len = 0;
    if (args.Length() >= 1)
        status = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(200));
    std::string typeStr;
    if (args.Length() >= 2) { typeStr = V8ToString(iso, args[1]); type = typeStr.c_str(); }
    if (args.Length() >= 3)
        len = static_cast<int>(args[2]->Int32Value(iso->GetCurrentContext()).FromMaybe(0));
    Ns_ConnSetRequiredHeaders(ctx->conn, nc(type), len);
    Ns_ConnFlushHeaders(ctx->conn, status);
}

/* ns.conn.startContent() — begin the content body (writes blank line) */
static void JsConnStartContent(const v8::FunctionCallbackInfo<v8::Value> &/*args*/) {
    /* In AOLserver, headers+content are managed by the return functions.
     * This is a no-op shim kept for API compatibility. */
}

/* ns.conn.respond(status, type, body) — alias for return */
static void JsConnRespond(const v8::FunctionCallbackInfo<v8::Value> &args) {
    JsConnReturn(args);
}

/* ns.conn.internalRedirect(url) — internal redirect to another URL */
static void JsConnInternalRedirect(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (!ctx || !ctx->conn || args.Length() < 1) return;
    std::string url = V8ToString(iso, args[0]);
    ctx->responseSent = true;
    Ns_ConnRedirect(ctx->conn, nc(url.c_str()));
}

/* ns.conn.parseHeader(headerLine) -> {key, value} or null
 * Parses a single "Key: Value" HTTP header line. */
static void JsConnParseHeader(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().SetNull(); return; }
    std::string line = V8ToString(iso, args[0]);
    Ns_Set *set = Ns_SetCreate(nullptr);
    /* Ns_ParseHeader modifies the string in-place, so work on a copy */
    std::vector<char> buf(line.begin(), line.end());
    buf.push_back('\0');
    int rc = Ns_ParseHeader(set, buf.data(), Preserve);
    if (rc == NS_OK && Ns_SetSize(set) > 0) {
        v8::Local<v8::Object> obj = v8::Object::New(iso);
        v8::Local<v8::Context> ctx = iso->GetCurrentContext();
        obj->Set(ctx, v8s(iso, "key"),   v8s(iso, Ns_SetKey(set, 0))).Check();
        obj->Set(ctx, v8s(iso, "value"), v8s(iso, Ns_SetValue(set, 0) ? Ns_SetValue(set, 0) : "")).Check();
        Ns_SetFree(set);
        args.GetReturnValue().Set(obj);
    } else {
        Ns_SetFree(set);
        args.GetReturnValue().SetNull();
    }
}

/* ns.conn.authorize(method, url, user, passwd, peer) -> bool */
static void JsConnAuthorize(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *jsctx = GetCtx(args);
    if (!jsctx || args.Length() < 5) { args.GetReturnValue().Set(false); return; }
    std::string method = V8ToString(iso, args[0]);
    std::string url    = V8ToString(iso, args[1]);
    std::string user   = V8ToString(iso, args[2]);
    std::string passwd = V8ToString(iso, args[3]);
    std::string peer   = V8ToString(iso, args[4]);
    const char *server = jsctx->dataPtr->modPtr->server;
    int result = Ns_AuthorizeRequest(nc(server),
                                     nc(method.c_str()), nc(url.c_str()),
                                     nc(user.c_str()), nc(passwd.c_str()),
                                     nc(peer.c_str()));
    args.GetReturnValue().Set(result == NS_OK);
}

/* -----------------------------------------------------------------------
 * ns.sema.* — semaphore operations
 * --------------------------------------------------------------------- */
static void JsSemaCreate(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    int initCount = 0;
    if (args.Length() >= 1)
        initCount = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(0));
    Ns_Sema *s = static_cast<Ns_Sema *>(ns_calloc(1, sizeof(Ns_Sema)));
    Ns_SemaInit(s, initCount);
    Ns_MutexLock(&js_sema_map_lock);
    int id = ++js_sema_next_id;
    (*js_sema_map)[id] = s;
    Ns_MutexUnlock(&js_sema_map_lock);
    args.GetReturnValue().Set(id);
}

static void JsSemaWait(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) return;
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_MutexLock(&js_sema_map_lock);
    auto it = js_sema_map->find(id);
    Ns_Sema *s = (it != js_sema_map->end()) ? it->second : nullptr;
    Ns_MutexUnlock(&js_sema_map_lock);
    if (s) Ns_SemaWait(s);
}

static void JsSemaPost(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) return;
    int id    = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    int count = 1;
    if (args.Length() >= 2)
        count = static_cast<int>(args[1]->Int32Value(iso->GetCurrentContext()).FromMaybe(1));
    Ns_MutexLock(&js_sema_map_lock);
    auto it = js_sema_map->find(id);
    Ns_Sema *s = (it != js_sema_map->end()) ? it->second : nullptr;
    Ns_MutexUnlock(&js_sema_map_lock);
    if (s) Ns_SemaPost(s, count);
}

static void JsSemaDestroy(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) return;
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_MutexLock(&js_sema_map_lock);
    auto it = js_sema_map->find(id);
    if (it != js_sema_map->end()) {
        Ns_SemaDestroy(it->second);
        ns_free(it->second);
        js_sema_map->erase(it);
    }
    Ns_MutexUnlock(&js_sema_map_lock);
}

/* -----------------------------------------------------------------------
 * ns.cond.* — condition variable operations
 * --------------------------------------------------------------------- */
static void JsCondCreate(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    Ns_Cond *c = static_cast<Ns_Cond *>(ns_calloc(1, sizeof(Ns_Cond)));
    Ns_CondInit(c);
    Ns_MutexLock(&js_cond_map_lock);
    int id = ++js_cond_next_id;
    (*js_cond_map)[id] = c;
    Ns_MutexUnlock(&js_cond_map_lock);
    args.GetReturnValue().Set(id);
    (void)iso;
}

static void JsCondSignal(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) return;
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_MutexLock(&js_cond_map_lock);
    auto it = js_cond_map->find(id);
    Ns_Cond *c = (it != js_cond_map->end()) ? it->second : nullptr;
    Ns_MutexUnlock(&js_cond_map_lock);
    if (c) Ns_CondSignal(c);
}

static void JsCondBroadcast(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) return;
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_MutexLock(&js_cond_map_lock);
    auto it = js_cond_map->find(id);
    Ns_Cond *c = (it != js_cond_map->end()) ? it->second : nullptr;
    Ns_MutexUnlock(&js_cond_map_lock);
    if (c) Ns_CondBroadcast(c);
}

/* ns.cond.wait(condId, mutexId) */
static void JsCondWait(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) return;
    int cid = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    int mid = static_cast<int>(args[1]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_Cond  *c = nullptr;
    Ns_Mutex *m = nullptr;
    Ns_MutexLock(&js_cond_map_lock);
    { auto it = js_cond_map->find(cid); if (it != js_cond_map->end()) c = it->second; }
    Ns_MutexUnlock(&js_cond_map_lock);
    Ns_MutexLock(&js_mutex_map_lock);
    { auto it = js_mutex_map->find(mid); if (it != js_mutex_map->end()) m = it->second; }
    Ns_MutexUnlock(&js_mutex_map_lock);
    if (c && m) Ns_CondWait(c, m);
}

/* ns.cond.timedWait(condId, mutexId, timeoutSec) -> bool (true=signalled) */
static void JsCondTimedWait(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 3) { args.GetReturnValue().Set(false); return; }
    int cid = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    int mid = static_cast<int>(args[1]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    int sec = static_cast<int>(args[2]->Int32Value(iso->GetCurrentContext()).FromMaybe(0));
    Ns_Cond  *c = nullptr;
    Ns_Mutex *m = nullptr;
    Ns_MutexLock(&js_cond_map_lock);
    { auto it = js_cond_map->find(cid); if (it != js_cond_map->end()) c = it->second; }
    Ns_MutexUnlock(&js_cond_map_lock);
    Ns_MutexLock(&js_mutex_map_lock);
    { auto it = js_mutex_map->find(mid); if (it != js_mutex_map->end()) m = it->second; }
    Ns_MutexUnlock(&js_mutex_map_lock);
    if (c && m) {
        Ns_Time timeout;
        timeout.sec  = sec;
        timeout.usec = 0;
        int rc = Ns_CondTimedWait(c, m, &timeout);
        args.GetReturnValue().Set(rc == NS_OK);
    } else {
        args.GetReturnValue().Set(false);
    }
}

static void JsCondDestroy(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) return;
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_MutexLock(&js_cond_map_lock);
    auto it = js_cond_map->find(id);
    if (it != js_cond_map->end()) {
        Ns_CondDestroy(it->second);
        ns_free(it->second);
        js_cond_map->erase(it);
    }
    Ns_MutexUnlock(&js_cond_map_lock);
}

/* -----------------------------------------------------------------------
 * ns.sched extensions: daily, weekly, pause, resume
 * --------------------------------------------------------------------- */

/* Fire proc matching Ns_SchedProc signature for daily/weekly */
static void SchedDailyWeeklyFire(void *arg, int /*id*/) {
    SchedRepeatCtx *ctx = static_cast<SchedRepeatCtx *>(arg);
    SchedRunJs(ctx->jsCode);
}

/* ns.sched.daily(hour, min, jsCode) — returns schedule ID */
static void JsSchedDaily(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 3) { args.GetReturnValue().Set(-1); return; }
    int hour   = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(0));
    int minute = static_cast<int>(args[1]->Int32Value(iso->GetCurrentContext()).FromMaybe(0));
    std::string jsCode = V8ToString(iso, args[2]);
    SchedRepeatCtx *ctx = new SchedRepeatCtx();
    ctx->modPtr = GetMod(args);
    ctx->jsCode = jsCode;
    int id = Ns_ScheduleDaily(SchedDailyWeeklyFire, ctx, 0, hour, minute, nullptr);
    if (id >= 0) {
        Ns_MutexLock(&js_sched_map_lock);
        (*js_sched_map)[id] = ctx;
        Ns_MutexUnlock(&js_sched_map_lock);
    } else {
        delete ctx;
    }
    args.GetReturnValue().Set(id);
}

/* ns.sched.weekly(day, hour, min, jsCode) — returns schedule ID */
static void JsSchedWeekly(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 4) { args.GetReturnValue().Set(-1); return; }
    int day    = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(0));
    int hour   = static_cast<int>(args[1]->Int32Value(iso->GetCurrentContext()).FromMaybe(0));
    int minute = static_cast<int>(args[2]->Int32Value(iso->GetCurrentContext()).FromMaybe(0));
    std::string jsCode = V8ToString(iso, args[3]);
    SchedRepeatCtx *ctx = new SchedRepeatCtx();
    ctx->modPtr = GetMod(args);
    ctx->jsCode = jsCode;
    int id = Ns_ScheduleWeekly(SchedDailyWeeklyFire, ctx, 0, day, hour, minute, nullptr);
    if (id >= 0) {
        Ns_MutexLock(&js_sched_map_lock);
        (*js_sched_map)[id] = ctx;
        Ns_MutexUnlock(&js_sched_map_lock);
    } else {
        delete ctx;
    }
    args.GetReturnValue().Set(id);
}

/* ns.sched.pause(id) -> bool */
static void JsSchedPause(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(false); return; }
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    args.GetReturnValue().Set(id >= 0 && Ns_Pause(id) != 0);
}

/* ns.sched.resume(id) -> bool */
static void JsSchedResume(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(false); return; }
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    args.GetReturnValue().Set(id >= 0 && Ns_Resume(id) != 0);
}

/* -----------------------------------------------------------------------
 * ns.set.* — in-memory Ns_Set key/value operations
 * Integer handle -> Ns_Set* stored in js_set_map.
 * --------------------------------------------------------------------- */
static void JsSetCreate(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    std::string name;
    if (args.Length() >= 1) name = V8ToString(iso, args[0]);
    Ns_Set *set = Ns_SetCreate(name.empty() ? nullptr : nc(name.c_str()));
    Ns_MutexLock(&js_set_map_lock);
    int id = ++js_set_next_id;
    (*js_set_map)[id] = set;
    Ns_MutexUnlock(&js_set_map_lock);
    args.GetReturnValue().Set(id);
}

static Ns_Set *JsSetLookup(int id) {
    Ns_MutexLock(&js_set_map_lock);
    auto it = js_set_map->find(id);
    Ns_Set *s = (it != js_set_map->end()) ? it->second : nullptr;
    Ns_MutexUnlock(&js_set_map_lock);
    return s;
}

static void JsSetPut(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 3) { args.GetReturnValue().Set(-1); return; }
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_Set *s = JsSetLookup(id);
    if (!s) { args.GetReturnValue().Set(-1); return; }
    std::string key = V8ToString(iso, args[1]);
    std::string val = V8ToString(iso, args[2]);
    int idx = Ns_SetPut(s, nc(key.c_str()), nc(val.c_str()));
    args.GetReturnValue().Set(idx);
}

static void JsSetGet(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().SetNull(); return; }
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_Set *s = JsSetLookup(id);
    if (!s) { args.GetReturnValue().SetNull(); return; }
    std::string key = V8ToString(iso, args[1]);
    const char *val = Ns_SetGet(s, nc(key.c_str()));
    if (val) args.GetReturnValue().Set(v8s(iso, val));
    else     args.GetReturnValue().SetNull();
}

static void JsSetIGet(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().SetNull(); return; }
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_Set *s = JsSetLookup(id);
    if (!s) { args.GetReturnValue().SetNull(); return; }
    std::string key = V8ToString(iso, args[1]);
    const char *val = Ns_SetIGet(s, nc(key.c_str()));
    if (val) args.GetReturnValue().Set(v8s(iso, val));
    else     args.GetReturnValue().SetNull();
}

static void JsSetFind(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().Set(-1); return; }
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_Set *s = JsSetLookup(id);
    if (!s) { args.GetReturnValue().Set(-1); return; }
    std::string key = V8ToString(iso, args[1]);
    args.GetReturnValue().Set(Ns_SetFind(s, nc(key.c_str())));
}

static void JsSetSize(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(0); return; }
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_Set *s = JsSetLookup(id);
    args.GetReturnValue().Set(s ? Ns_SetSize(s) : 0);
}

static void JsSetKey(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().SetNull(); return; }
    int id  = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    int idx = static_cast<int>(args[1]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_Set *s = JsSetLookup(id);
    if (!s || idx < 0 || idx >= Ns_SetSize(s)) { args.GetReturnValue().SetNull(); return; }
    const char *k = Ns_SetKey(s, idx);
    args.GetReturnValue().Set(v8s(iso, k ? k : ""));
}

static void JsSetValue(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().SetNull(); return; }
    int id  = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    int idx = static_cast<int>(args[1]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_Set *s = JsSetLookup(id);
    if (!s || idx < 0 || idx >= Ns_SetSize(s)) { args.GetReturnValue().SetNull(); return; }
    const char *v = Ns_SetValue(s, idx);
    args.GetReturnValue().Set(v8s(iso, v ? v : ""));
}

static void JsSetDelete(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) return;
    int id  = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    int idx = static_cast<int>(args[1]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_Set *s = JsSetLookup(id);
    if (s && idx >= 0 && idx < Ns_SetSize(s)) Ns_SetDelete(s, idx);
}

static void JsSetUpdate(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 3) return;
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_Set *s = JsSetLookup(id);
    if (!s) return;
    std::string key = V8ToString(iso, args[1]);
    std::string val = V8ToString(iso, args[2]);
    Ns_SetUpdate(s, nc(key.c_str()), nc(val.c_str()));
}

static void JsSetFree(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) return;
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_MutexLock(&js_set_map_lock);
    auto it = js_set_map->find(id);
    if (it != js_set_map->end()) {
        Ns_SetFree(it->second);
        js_set_map->erase(it);
    }
    Ns_MutexUnlock(&js_set_map_lock);
}

/* ns.set.toObject(id) — return all fields as a JS plain object */
static void JsSetToObject(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().SetNull(); return; }
    int id = static_cast<int>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    Ns_Set *s = JsSetLookup(id);
    if (!s) { args.GetReturnValue().SetNull(); return; }
    v8::Local<v8::Object> obj = v8::Object::New(iso);
    v8::Local<v8::Context> ctx = iso->GetCurrentContext();
    for (int i = 0; i < Ns_SetSize(s); ++i) {
        const char *k = Ns_SetKey(s, i);
        const char *v = Ns_SetValue(s, i);
        if (k) obj->Set(ctx, v8s(iso, k), v8s(iso, v ? v : "")).Check();
    }
    args.GetReturnValue().Set(obj);
}

/* -----------------------------------------------------------------------
 * ns.http.get(url) -> {body, headers} or null
 * Uses Ns_FetchURL for external URLs and Ns_FetchPage for internal paths.
 * --------------------------------------------------------------------- */
static void JsHttpGet(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().SetNull(); return; }
    std::string url = V8ToString(iso, args[0]);
    NsJsContext *jsctx = GetCtx(args);

    Ns_DString ds;
    Ns_DStringInit(&ds);

    int status;
    if (url[0] == '/') {
        /* Internal page fetch */
        const char *server = jsctx ? jsctx->dataPtr->modPtr->server : nullptr;
        status = server ? Ns_FetchPage(&ds, nc(url.c_str()), nc(server)) : NS_ERROR;
        if (status == NS_OK) {
            v8::Local<v8::Object> obj = v8::Object::New(iso);
            v8::Local<v8::Context> ctx = iso->GetCurrentContext();
            obj->Set(ctx, v8s(iso, "body"), v8s(iso, ds.string ? ds.string : "")).Check();
            obj->Set(ctx, v8s(iso, "headers"), v8::Object::New(iso)).Check();
            Ns_DStringFree(&ds);
            args.GetReturnValue().Set(obj);
        } else {
            Ns_DStringFree(&ds);
            args.GetReturnValue().SetNull();
        }
    } else {
        /* External URL fetch */
        Ns_Set *hdrs = Ns_SetCreate(nullptr);
        status = Ns_FetchURL(&ds, nc(url.c_str()), hdrs);
        if (status == NS_OK) {
            v8::Local<v8::Object> obj = v8::Object::New(iso);
            v8::Local<v8::Context> ctx = iso->GetCurrentContext();
            obj->Set(ctx, v8s(iso, "body"), v8s(iso, ds.string ? ds.string : "")).Check();
            v8::Local<v8::Object> hdrObj = v8::Object::New(iso);
            for (int i = 0; i < Ns_SetSize(hdrs); ++i) {
                const char *k = Ns_SetKey(hdrs, i);
                const char *v = Ns_SetValue(hdrs, i);
                if (k) hdrObj->Set(ctx, v8s(iso, k), v8s(iso, v ? v : "")).Check();
            }
            obj->Set(ctx, v8s(iso, "headers"), hdrObj).Check();
            Ns_SetFree(hdrs);
            Ns_DStringFree(&ds);
            args.GetReturnValue().Set(obj);
        } else {
            Ns_SetFree(hdrs);
            Ns_DStringFree(&ds);
            args.GetReturnValue().SetNull();
        }
    }
}

/* -----------------------------------------------------------------------
 * ns.sock.* — socket operations (SOCKET = int on Unix)
 * --------------------------------------------------------------------- */

/* ns.sock.open(host, port, timeout) -> fd or -1 */
static void JsSockOpen(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().Set(-1); return; }
    std::string host = V8ToString(iso, args[0]);
    int port    = static_cast<int>(args[1]->Int32Value(iso->GetCurrentContext()).FromMaybe(80));
    int timeout = 10;
    if (args.Length() >= 3)
        timeout = static_cast<int>(args[2]->Int32Value(iso->GetCurrentContext()).FromMaybe(10));
    SOCKET sock = Ns_SockTimedConnect(nc(host.c_str()), port, timeout);
    args.GetReturnValue().Set(static_cast<int>(sock));
}

/* ns.sock.listen(addr, port) -> fd or -1 */
static void JsSockListen(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().Set(-1); return; }
    std::string addr = V8ToString(iso, args[0]);
    int port = static_cast<int>(args[1]->Int32Value(iso->GetCurrentContext()).FromMaybe(0));
    /* Pass nullptr for INADDR_ANY */
    const char *a = addr.empty() ? nullptr : addr.c_str();
    SOCKET sock = Ns_SockListen(nc(a), port);
    args.GetReturnValue().Set(static_cast<int>(sock));
}

/* ns.sock.accept(listenFd) -> fd or -1 */
static void JsSockAccept(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(-1); return; }
    SOCKET listenSock = static_cast<SOCKET>(
        args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    SOCKET clientSock = Ns_SockAccept(listenSock, nullptr, nullptr);
    args.GetReturnValue().Set(static_cast<int>(clientSock));
}

/* ns.sock.recv(fd, maxBytes, timeout) -> string or null */
static void JsSockRecv(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().SetNull(); return; }
    SOCKET sock = static_cast<SOCKET>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    int maxBytes = 4096;
    if (args.Length() >= 2)
        maxBytes = static_cast<int>(args[1]->Int32Value(iso->GetCurrentContext()).FromMaybe(4096));
    int timeout = -1;
    if (args.Length() >= 3)
        timeout = static_cast<int>(args[2]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    if (sock < 0 || maxBytes <= 0) { args.GetReturnValue().SetNull(); return; }
    char *buf = static_cast<char *>(ns_malloc(static_cast<size_t>(maxBytes)));
    int n = Ns_SockRecv(sock, buf, maxBytes, timeout);
    if (n > 0) {
        v8::Local<v8::String> rv = v8::String::NewFromUtf8(
            iso, buf, v8::NewStringType::kNormal, n).ToLocalChecked();
        ns_free(buf);
        args.GetReturnValue().Set(rv);
    } else {
        ns_free(buf);
        args.GetReturnValue().SetNull();
    }
}

/* ns.sock.send(fd, data, timeout) -> bytes sent or -1 */
static void JsSockSend(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 2) { args.GetReturnValue().Set(-1); return; }
    SOCKET sock = static_cast<SOCKET>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    std::string data = V8ToString(iso, args[1]);
    int timeout = -1;
    if (args.Length() >= 3)
        timeout = static_cast<int>(args[2]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    if (sock < 0) { args.GetReturnValue().Set(-1); return; }
    int n = Ns_SockSend(sock, nc(data.c_str()), static_cast<int>(data.size()), timeout);
    args.GetReturnValue().Set(n);
}

/* ns.sock.close(fd) */
static void JsSockClose(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) return;
    SOCKET sock = static_cast<SOCKET>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    if (sock >= 0) ns_sockclose(sock);
}

/* ns.sock.setBlocking(fd) -> bool */
static void JsSockSetBlocking(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(false); return; }
    SOCKET sock = static_cast<SOCKET>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    args.GetReturnValue().Set(Ns_SockSetBlocking(sock) == NS_OK);
}

/* ns.sock.setNonBlocking(fd) -> bool */
static void JsSockSetNonBlocking(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(false); return; }
    SOCKET sock = static_cast<SOCKET>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    args.GetReturnValue().Set(Ns_SockSetNonBlocking(sock) == NS_OK);
}

/* ns.sock.nread(fd) -> bytes available or -1 */
static void JsSockNRead(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(-1); return; }
    SOCKET sock = static_cast<SOCKET>(args[0]->Int32Value(iso->GetCurrentContext()).FromMaybe(-1));
    if (sock < 0) { args.GetReturnValue().Set(-1); return; }
    int n = 0;
    if (ioctl(sock, FIONREAD, &n) == 0) args.GetReturnValue().Set(n);
    else                                 args.GetReturnValue().Set(-1);
}

/* -----------------------------------------------------------------------
 * ns.thread.* — thread operations
 * --------------------------------------------------------------------- */

struct JsThreadCtx {
    std::string jsCode;
};

static void JsThreadProc(void *arg) {
    JsThreadCtx *tctx = static_cast<JsThreadCtx *>(arg);
    SchedRunJs(tctx->jsCode);
    delete tctx;
}

/* ns.thread.create(jsCode) — fire-and-forget background thread */
static void JsThreadCreate(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(false); return; }
    std::string code = V8ToString(iso, args[0]);
    JsThreadCtx *tctx = new JsThreadCtx();
    tctx->jsCode = code;
    Ns_Thread thread;
    Ns_ThreadCreate(JsThreadProc, tctx, 0, &thread);
    args.GetReturnValue().Set(true);
}

/* ns.thread.id() -> current thread numeric ID */
static void JsThreadId(const v8::FunctionCallbackInfo<v8::Value> &args) {
    args.GetReturnValue().Set(Ns_ThreadId());
}

/* ns.thread.yield() — yield the current thread */
static void JsThreadYield(const v8::FunctionCallbackInfo<v8::Value> &/*args*/) {
    Ns_ThreadYield();
}

/* ns.thread.setName(name) */
static void JsThreadSetName(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) return;
    std::string name = V8ToString(iso, args[0]);
    Ns_ThreadSetName(nc(name.c_str()));
}

/* ns.thread.getName() -> string */
static void JsThreadGetName(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    const char *name = Ns_ThreadGetName();
    args.GetReturnValue().Set(v8s(iso, name ? name : ""));
}

/* -----------------------------------------------------------------------
 * JavaScript control port (jscp) implementation
 *
 * Each session runs on its own Ns thread with a dedicated V8 isolate and
 * a persistent v8::Context so that REPL state (variables etc.) survives
 * across commands.  conn APIs return null/empty (ctx->conn == nullptr).
 * --------------------------------------------------------------------- */

/* Constant-time string compare to avoid timing side-channels on passwords */
static bool JsCpConstEq(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

/* Read one line from fd (blocking); returns "" on EOF/error, strips \r\n */
static std::string JsCpReadLine(int fd) {
    std::string line;
    char c;
    while (true) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return "";
        if (c == '\n') break;
        if (c != '\r') line += c;
    }
    return line;
}

/* Write all bytes to fd; returns false on error */
static bool JsCpWrite(int fd, const std::string &s) {
    const char *p = s.c_str();
    size_t left = s.size();
    while (left > 0) {
        ssize_t n = send(fd, p, left, 0);
        if (n <= 0) return false;
        p    += n;
        left -= static_cast<size_t>(n);
    }
    return true;
}

/* Authenticate: prompt for username/password, return true if valid */
static bool JsCpAuthenticate(int fd, NsJsCpConfig *cfg) {
    if (!JsCpWrite(fd, "Username: ")) return false;
    std::string user = JsCpReadLine(fd);
    if (user.empty()) return false;

    if (!JsCpWrite(fd, "Password: ")) return false;
    std::string pass = JsCpReadLine(fd);
    if (pass.empty()) return false;

    for (const JsCpUser &u : cfg->users) {
        if (u.username == user && JsCpConstEq(u.password, pass)) {
            JsCpWrite(fd, "Login successful.\r\n");
            return true;
        }
    }
    JsCpWrite(fd, "Login incorrect.\r\n");
    return false;
}

/* Read a complete command (handle \ line continuation) */
static std::string JsCpReadCommand(int fd, int cmdNum) {
    std::string prompt = std::string("jscp ") + std::to_string(cmdNum) + "> ";
    if (!JsCpWrite(fd, prompt)) return "";

    std::string line = JsCpReadLine(fd);
    if (line.empty()) return "";

    std::string code;
    while (!line.empty() && line.back() == '\\') {
        line.pop_back();   /* strip the backslash */
        code += line + "\n";
        if (!JsCpWrite(fd, "... ")) return "";
        line = JsCpReadLine(fd);
        if (line.empty()) return code;  /* send what we have */
    }
    code += line;
    return code;
}

/* Run JS code string in the persistent context; return result as string */
static std::string JsCpRunCommand(v8::Isolate *iso,
                                  v8::Local<v8::Context> &v8ctx,
                                  const std::string &code) {
    v8::Context::Scope cs(v8ctx);
    v8::TryCatch tc(iso);
    v8::Local<v8::String> src = v8::String::NewFromUtf8(iso, code.c_str()).ToLocalChecked();
    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(v8ctx, src).ToLocal(&script)) {
        v8::String::Utf8Value err(iso, tc.Exception());
        v8::Local<v8::Message> msg = tc.Message();
        int line = msg.IsEmpty() ? -1 : msg->GetLineNumber(v8ctx).FromMaybe(-1);
        std::string errStr = *err ? *err : "compile error";
        if (line >= 0)
            return "ERROR: " + errStr + " (line " + std::to_string(line) + ")";
        return "ERROR: " + errStr;
    }
    v8::Local<v8::Value> result;
    if (!script->Run(v8ctx).ToLocal(&result)) {
        v8::String::Utf8Value err(iso, tc.Exception());
        v8::Local<v8::Message> msg = tc.Message();
        int line = msg.IsEmpty() ? -1 : msg->GetLineNumber(v8ctx).FromMaybe(-1);
        std::string errStr = *err ? *err : "runtime error";
        if (line >= 0)
            return "ERROR: " + errStr + " (line " + std::to_string(line) + ")";
        return "ERROR: " + errStr;
    }
    if (result->IsUndefined()) return "undefined";
    if (result->IsNull())      return "null";
    /* For objects/arrays use JSON.stringify; for primitives use toString */
    if (result->IsObject() && !result->IsFunction()) {
        v8::Local<v8::Object> jsonObj = v8ctx->Global()
            ->Get(v8ctx, v8::String::NewFromUtf8(iso, "JSON").ToLocalChecked())
            .ToLocalChecked().As<v8::Object>();
        v8::Local<v8::Value> strFn = jsonObj
            ->Get(v8ctx, v8::String::NewFromUtf8(iso, "stringify").ToLocalChecked())
            .ToLocalChecked();
        if (strFn->IsFunction()) {
            v8::Local<v8::Value> argv[] = { result };
            v8::Local<v8::Value> json = strFn.As<v8::Function>()
                ->Call(v8ctx, jsonObj, 1, argv)
                .ToLocalChecked();
            v8::String::Utf8Value s(iso, json);
            return *s ? *s : "null";
        }
    }
    v8::String::Utf8Value s(iso, result);
    return *s ? *s : "";
}

struct JsCpSessionArg {
    int             fd;
    NsJsCpConfig   *cfg;
};

static void JsCpSessionThread(void *arg) {
    JsCpSessionArg *sa = static_cast<JsCpSessionArg *>(arg);
    int              fd  = sa->fd;
    NsJsCpConfig    *cfg = sa->cfg;
    delete sa;

    /* Authenticate */
    if (!JsCpAuthenticate(fd, cfg)) {
        close(fd);
        Ns_MutexLock(&js_cp_conn_mx);
        js_cp_conn_count--;
        Ns_MutexUnlock(&js_cp_conn_mx);
        return;
    }

    /* Create a dedicated V8 isolate for this session */
    NsAllocator *alloc = new NsAllocator();
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = alloc;
    v8::Isolate *iso = v8::Isolate::New(params);

    /* Build a null-conn JsData so GetMod() works for ns.info etc. */
    JsData jdata;
    memset(&jdata, 0, sizeof(jdata));
    jdata.isolate = iso;
    jdata.allocator = alloc;
    jdata.modPtr = cfg->modPtr;

    NsJsContext jctx;
    memset(&jctx, 0, sizeof(jctx));
    jctx.dataPtr = &jdata;
    jctx.conn    = nullptr;
    Ns_DStringInit(&jctx.output);

    {
        v8::Isolate::Scope isc(iso);
        v8::HandleScope   hs(iso);

        /* Build global template and create the persistent session context */
        v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(iso);
        tmpl->SetInternalFieldCount(0);
        BuildGlobalTemplate(iso, tmpl);

        v8::Local<v8::Context> v8ctx = v8::Context::New(iso, nullptr, tmpl);
        v8ctx->SetEmbedderData(0, v8::External::New(iso, &jctx));

        /* REPL loop */
        int cmdNum = 1;
        while (true) {
            std::string code = JsCpReadCommand(fd, cmdNum);
            if (code.empty()) break;

            /* Trim whitespace */
            size_t st = code.find_first_not_of(" \t\r\n");
            if (st == std::string::npos) {
                cmdNum++;
                continue;
            }
            std::string trimmed = code.substr(st);
            if (trimmed == "exit" || trimmed == "quit") break;

            if (cfg->logCommands)
                Ns_Log(Notice, nc("nsjs jscp: %s"), nc(trimmed));

            std::string result = JsCpRunCommand(iso, v8ctx, trimmed);
            JsCpWrite(fd, result + "\r\n");
            cmdNum++;
        }
    }

    Ns_DStringFree(&jctx.output);
    iso->Dispose();
    delete alloc;
    close(fd);

    Ns_MutexLock(&js_cp_conn_mx);
    js_cp_conn_count--;
    Ns_MutexUnlock(&js_cp_conn_mx);
}

static void JsCpListenerThread(void *arg) {
    NsJsCpConfig *cfg = static_cast<NsJsCpConfig *>(arg);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(cfg->port));
    inet_pton(AF_INET, cfg->address.c_str(), &addr.sin_addr);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        Ns_Log(Error, nc("nsjs jscp: socket failed: %s"), strerror(errno));
        return;
    }
    int on = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (bind(lfd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        Ns_Log(Error, nc("nsjs jscp: bind %s:%d failed: %s"),
               nc(cfg->address), cfg->port, strerror(errno));
        close(lfd);
        return;
    }
    if (listen(lfd, 5) < 0) {
        Ns_Log(Error, nc("nsjs jscp: listen failed: %s"), strerror(errno));
        close(lfd);
        return;
    }
    Ns_Log(Notice, nc("nsjs jscp: listening on %s:%d"),
           nc(cfg->address), cfg->port);

    while (true) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(lfd, reinterpret_cast<struct sockaddr *>(&caddr), &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        Ns_MutexLock(&js_cp_conn_mx);
        int count = js_cp_conn_count;
        if (count < cfg->maxSessions) {
            js_cp_conn_count++;
        }
        Ns_MutexUnlock(&js_cp_conn_mx);

        if (count >= cfg->maxSessions) {
            send(cfd, "ERROR: max sessions reached\r\n", 29, 0);
            close(cfd);
            continue;
        }

        JsCpSessionArg *sa = new JsCpSessionArg();
        sa->fd  = cfd;
        sa->cfg = cfg;

        Ns_ThreadCreate(JsCpSessionThread, sa, 0, nullptr);
    }
    close(lfd);
}

static void JsCpInit(NsJsCpConfig *cfg) {
    Ns_MutexInit(&js_cp_conn_mx);
    Ns_ThreadCreate(JsCpListenerThread, cfg, 0, nullptr);
}

/* -----------------------------------------------------------------------
 * BuildGlobalTemplate — construct the `ns` object hierarchy once per thread
 * --------------------------------------------------------------------- */

static void BuildGlobalTemplate(v8::Isolate *isolate,
                                v8::Local<v8::ObjectTemplate> &globalTmpl) {
    /* ---- ns.conn ---- */
    v8::Local<v8::ObjectTemplate> connObj = v8::ObjectTemplate::New(isolate);
    connObj->Set(isolate, "write",          v8::FunctionTemplate::New(isolate, JsConnWrite));
    connObj->Set(isolate, "getHeader",      v8::FunctionTemplate::New(isolate, JsConnGetHeader));
    connObj->Set(isolate, "setHeader",      v8::FunctionTemplate::New(isolate, JsConnSetHeader));
    connObj->Set(isolate, "getQuery",       v8::FunctionTemplate::New(isolate, JsConnGetQuery));
    connObj->Set(isolate, "getMethod",      v8::FunctionTemplate::New(isolate, JsConnGetMethod));
    connObj->Set(isolate, "getUrl",         v8::FunctionTemplate::New(isolate, JsConnGetUrl));
    connObj->Set(isolate, "getPeerAddr",    v8::FunctionTemplate::New(isolate, JsConnGetPeerAddr));
    connObj->Set(isolate, "location",       v8::FunctionTemplate::New(isolate, JsConnLocation));
    connObj->Set(isolate, "getHost",        v8::FunctionTemplate::New(isolate, JsConnGetHost));
    connObj->Set(isolate, "getPort",        v8::FunctionTemplate::New(isolate, JsConnGetPort));
    connObj->Set(isolate, "getId",          v8::FunctionTemplate::New(isolate, JsConnGetId));
    connObj->Set(isolate, "getAuthUser",    v8::FunctionTemplate::New(isolate, JsConnGetAuthUser));
    connObj->Set(isolate, "getAuthPasswd",  v8::FunctionTemplate::New(isolate, JsConnGetAuthPasswd));
    connObj->Set(isolate, "getAllHeaders",  v8::FunctionTemplate::New(isolate, JsConnGetAllHeaders));
    connObj->Set(isolate, "getAllQuery",    v8::FunctionTemplate::New(isolate, JsConnGetAllQuery));
    connObj->Set(isolate, "getContent",     v8::FunctionTemplate::New(isolate, JsConnGetContent));
    connObj->Set(isolate, "setStatus",      v8::FunctionTemplate::New(isolate, JsConnSetStatus));
    connObj->Set(isolate, "setContentType", v8::FunctionTemplate::New(isolate, JsConnSetContentType));
    connObj->Set(isolate, "returnRedirect", v8::FunctionTemplate::New(isolate, JsConnReturnRedirect));
    connObj->Set(isolate, "returnHtml",     v8::FunctionTemplate::New(isolate, JsConnReturnHtml));
    connObj->Set(isolate, "returnFile",     v8::FunctionTemplate::New(isolate, JsConnReturnFile));
    connObj->Set(isolate, "close",               v8::FunctionTemplate::New(isolate, JsConnClose));
    connObj->Set(isolate, "return",              v8::FunctionTemplate::New(isolate, JsConnReturn));
    connObj->Set(isolate, "returnBadRequest",    v8::FunctionTemplate::New(isolate, JsConnReturnBadRequest));
    connObj->Set(isolate, "returnForbidden",     v8::FunctionTemplate::New(isolate, JsConnReturnForbidden));
    connObj->Set(isolate, "returnNotFound",      v8::FunctionTemplate::New(isolate, JsConnReturnNotFound));
    connObj->Set(isolate, "returnUnauthorized",  v8::FunctionTemplate::New(isolate, JsConnReturnUnauthorized));
    connObj->Set(isolate, "returnNotice",        v8::FunctionTemplate::New(isolate, JsConnReturnNotice));
    connObj->Set(isolate, "returnError",         v8::FunctionTemplate::New(isolate, JsConnReturnError));
    connObj->Set(isolate, "headers",             v8::FunctionTemplate::New(isolate, JsConnHeaders));
    connObj->Set(isolate, "startContent",        v8::FunctionTemplate::New(isolate, JsConnStartContent));
    connObj->Set(isolate, "respond",             v8::FunctionTemplate::New(isolate, JsConnRespond));
    connObj->Set(isolate, "internalRedirect",    v8::FunctionTemplate::New(isolate, JsConnInternalRedirect));
    connObj->Set(isolate, "parseHeader",         v8::FunctionTemplate::New(isolate, JsConnParseHeader));
    connObj->Set(isolate, "authorize",           v8::FunctionTemplate::New(isolate, JsConnAuthorize));

    /* ---- ns.shared ---- */
    v8::Local<v8::ObjectTemplate> sharedObj = v8::ObjectTemplate::New(isolate);
    sharedObj->Set(isolate, "set",      v8::FunctionTemplate::New(isolate, JsSharedSet));
    sharedObj->Set(isolate, "get",      v8::FunctionTemplate::New(isolate, JsSharedGet));
    sharedObj->Set(isolate, "exists",   v8::FunctionTemplate::New(isolate, JsSharedExists));
    sharedObj->Set(isolate, "unset",    v8::FunctionTemplate::New(isolate, JsSharedUnset));
    sharedObj->Set(isolate, "incr",     v8::FunctionTemplate::New(isolate, JsSharedIncr));
    sharedObj->Set(isolate, "append",   v8::FunctionTemplate::New(isolate, JsSharedAppend));
    sharedObj->Set(isolate, "lappend",  v8::FunctionTemplate::New(isolate, JsSharedLAppend));
    sharedObj->Set(isolate, "names",    v8::FunctionTemplate::New(isolate, JsSharedNames));
    sharedObj->Set(isolate, "keys",     v8::FunctionTemplate::New(isolate, JsSharedKeys));
    sharedObj->Set(isolate, "getAll",   v8::FunctionTemplate::New(isolate, JsSharedGetAll));

    /* ---- ns.cache ---- */
    v8::Local<v8::ObjectTemplate> cacheObj = v8::ObjectTemplate::New(isolate);
    cacheObj->Set(isolate, "create",  v8::FunctionTemplate::New(isolate, JsCacheCreate));
    cacheObj->Set(isolate, "get",     v8::FunctionTemplate::New(isolate, JsCacheGet));
    cacheObj->Set(isolate, "set",     v8::FunctionTemplate::New(isolate, JsCacheSet));
    cacheObj->Set(isolate, "unset",   v8::FunctionTemplate::New(isolate, JsCacheUnset));
    cacheObj->Set(isolate, "flush",   v8::FunctionTemplate::New(isolate, JsCacheFlush));
    cacheObj->Set(isolate, "stats",   v8::FunctionTemplate::New(isolate, JsCacheStats));

    /* ---- ns.info ---- */
    v8::Local<v8::ObjectTemplate> infoObj = v8::ObjectTemplate::New(isolate);
    infoObj->Set(isolate, "version",  v8::FunctionTemplate::New(isolate, JsInfoVersion));
    infoObj->Set(isolate, "uptime",   v8::FunctionTemplate::New(isolate, JsInfoUptime));
    infoObj->Set(isolate, "pageroot", v8::FunctionTemplate::New(isolate, JsInfoPageroot));
    infoObj->Set(isolate, "log",      v8::FunctionTemplate::New(isolate, JsInfoLog));
    infoObj->Set(isolate, "config",   v8::FunctionTemplate::New(isolate, JsInfoConfig));
    infoObj->Set(isolate, "hostname", v8::FunctionTemplate::New(isolate, JsInfoHostname));
    infoObj->Set(isolate, "address",  v8::FunctionTemplate::New(isolate, JsInfoAddress));
    infoObj->Set(isolate, "pid",        v8::FunctionTemplate::New(isolate, JsInfoPid));
    infoObj->Set(isolate, "serverName", v8::FunctionTemplate::New(isolate, JsInfoServerName));

    /* ---- ns.time (callable + sub-properties) ---- */
    v8::Local<v8::FunctionTemplate> timeFn =
        v8::FunctionTemplate::New(isolate, JsTimeNow);
    timeFn->Set(isolate, "format",        v8::FunctionTemplate::New(isolate, JsTimeFormat));
    timeFn->Set(isolate, "httpTime",      v8::FunctionTemplate::New(isolate, JsTimeHttpTime));
    timeFn->Set(isolate, "parseHttpTime", v8::FunctionTemplate::New(isolate, JsTimeParseHttpTime));
    timeFn->Set(isolate, "gmtime",        v8::FunctionTemplate::New(isolate, JsTimeGmtime));
    timeFn->Set(isolate, "localtime",     v8::FunctionTemplate::New(isolate, JsTimeLocaltime));

    /* ---- ns.url ---- */
    v8::Local<v8::ObjectTemplate> urlObj = v8::ObjectTemplate::New(isolate);
    urlObj->Set(isolate, "encode",  v8::FunctionTemplate::New(isolate, JsUrlEncode));
    urlObj->Set(isolate, "decode",  v8::FunctionTemplate::New(isolate, JsUrlDecode));
    urlObj->Set(isolate, "parse",   v8::FunctionTemplate::New(isolate, JsUrlParse));
    urlObj->Set(isolate, "toFile",  v8::FunctionTemplate::New(isolate, JsUrlToFile));

    /* ---- ns.html ---- */
    v8::Local<v8::ObjectTemplate> htmlObj = v8::ObjectTemplate::New(isolate);
    htmlObj->Set(isolate, "quote",     v8::FunctionTemplate::New(isolate, JsHtmlQuote));
    htmlObj->Set(isolate, "guessType", v8::FunctionTemplate::New(isolate, JsHtmlGuessType));
    htmlObj->Set(isolate, "hrefs",     v8::FunctionTemplate::New(isolate, JsHtmlHrefs));

    /* ---- ns.file ---- */
    v8::Local<v8::ObjectTemplate> fileObj = v8::ObjectTemplate::New(isolate);
    fileObj->Set(isolate, "read",          v8::FunctionTemplate::New(isolate, JsFileRead));
    fileObj->Set(isolate, "write",         v8::FunctionTemplate::New(isolate, JsFileWrite));
    fileObj->Set(isolate, "exists",        v8::FunctionTemplate::New(isolate, JsFileExists));
    fileObj->Set(isolate, "stat",          v8::FunctionTemplate::New(isolate, JsFileStat));
    fileObj->Set(isolate, "mkdir",         v8::FunctionTemplate::New(isolate, JsFileMkdir));
    fileObj->Set(isolate, "rmdir",         v8::FunctionTemplate::New(isolate, JsFileRmdir));
    fileObj->Set(isolate, "unlink",        v8::FunctionTemplate::New(isolate, JsFileUnlink));
    fileObj->Set(isolate, "cp",            v8::FunctionTemplate::New(isolate, JsFileCp));
    fileObj->Set(isolate, "rename",        v8::FunctionTemplate::New(isolate, JsFileRename));
    fileObj->Set(isolate, "tmpnam",        v8::FunctionTemplate::New(isolate, JsFileTmpnam));
    fileObj->Set(isolate, "normalizePath", v8::FunctionTemplate::New(isolate, JsFileNormalizePath));
    fileObj->Set(isolate, "chmod",         v8::FunctionTemplate::New(isolate, JsFileChmod));
    fileObj->Set(isolate, "link",          v8::FunctionTemplate::New(isolate, JsFileLink));
    fileObj->Set(isolate, "symlink",       v8::FunctionTemplate::New(isolate, JsFileSymlink));
    fileObj->Set(isolate, "truncate",      v8::FunctionTemplate::New(isolate, JsFileTruncate));
    fileObj->Set(isolate, "roll",          v8::FunctionTemplate::New(isolate, JsFileRoll));
    fileObj->Set(isolate, "purge",         v8::FunctionTemplate::New(isolate, JsFilePurge));

    /* ---- ns.dns ---- */
    v8::Local<v8::ObjectTemplate> dnsObj = v8::ObjectTemplate::New(isolate);
    dnsObj->Set(isolate, "addrByHost", v8::FunctionTemplate::New(isolate, JsDnsAddrByHost));
    dnsObj->Set(isolate, "hostByAddr", v8::FunctionTemplate::New(isolate, JsDnsHostByAddr));

    /* ---- ns.sched ---- */
    v8::Local<v8::ObjectTemplate> schedObj = v8::ObjectTemplate::New(isolate);
    schedObj->Set(isolate, "after",    v8::FunctionTemplate::New(isolate, JsSchedAfter));
    schedObj->Set(isolate, "interval", v8::FunctionTemplate::New(isolate, JsSchedInterval));
    schedObj->Set(isolate, "cancel",   v8::FunctionTemplate::New(isolate, JsSchedCancel));
    schedObj->Set(isolate, "daily",    v8::FunctionTemplate::New(isolate, JsSchedDaily));
    schedObj->Set(isolate, "weekly",   v8::FunctionTemplate::New(isolate, JsSchedWeekly));
    schedObj->Set(isolate, "pause",    v8::FunctionTemplate::New(isolate, JsSchedPause));
    schedObj->Set(isolate, "resume",   v8::FunctionTemplate::New(isolate, JsSchedResume));

    /* ---- ns.mutex ---- */
    v8::Local<v8::ObjectTemplate> mutexObj = v8::ObjectTemplate::New(isolate);
    mutexObj->Set(isolate, "create",  v8::FunctionTemplate::New(isolate, JsMutexCreate));
    mutexObj->Set(isolate, "lock",    v8::FunctionTemplate::New(isolate, JsMutexLock));
    mutexObj->Set(isolate, "unlock",  v8::FunctionTemplate::New(isolate, JsMutexUnlock));
    mutexObj->Set(isolate, "trylock", v8::FunctionTemplate::New(isolate, JsMutexTryLock));
    mutexObj->Set(isolate, "destroy", v8::FunctionTemplate::New(isolate, JsMutexDestroy));

    /* ---- ns.rwlock ---- */
    v8::Local<v8::ObjectTemplate> rwlockObj = v8::ObjectTemplate::New(isolate);
    rwlockObj->Set(isolate, "create",    v8::FunctionTemplate::New(isolate, JsRWLockCreate));
    rwlockObj->Set(isolate, "readLock",  v8::FunctionTemplate::New(isolate, JsRWLockReadLock));
    rwlockObj->Set(isolate, "writeLock", v8::FunctionTemplate::New(isolate, JsRWLockWriteLock));
    rwlockObj->Set(isolate, "unlock",    v8::FunctionTemplate::New(isolate, JsRWLockUnlock));
    rwlockObj->Set(isolate, "destroy",   v8::FunctionTemplate::New(isolate, JsRWLockDestroy));

    /* ---- ns.image ---- */
    v8::Local<v8::ObjectTemplate> imageObj = v8::ObjectTemplate::New(isolate);
    imageObj->Set(isolate, "gifSize",  v8::FunctionTemplate::New(isolate, JsImageGifSize));
    imageObj->Set(isolate, "jpegSize", v8::FunctionTemplate::New(isolate, JsImageJpegSize));

    /* ---- ns.env ---- */
    v8::Local<v8::ObjectTemplate> envObj = v8::ObjectTemplate::New(isolate);
    envObj->Set(isolate, "get",   v8::FunctionTemplate::New(isolate, JsEnvGet));
    envObj->Set(isolate, "set",   v8::FunctionTemplate::New(isolate, JsEnvSet));
    envObj->Set(isolate, "unset", v8::FunctionTemplate::New(isolate, JsEnvUnset));
    envObj->Set(isolate, "names", v8::FunctionTemplate::New(isolate, JsEnvNames));

    /* ---- ns.process ---- */
    v8::Local<v8::ObjectTemplate> processObj = v8::ObjectTemplate::New(isolate);
    processObj->Set(isolate, "kill", v8::FunctionTemplate::New(isolate, JsProcessKill));

    /* ---- ns.http ---- */
    v8::Local<v8::ObjectTemplate> httpObj = v8::ObjectTemplate::New(isolate);
    httpObj->Set(isolate, "get", v8::FunctionTemplate::New(isolate, JsHttpGet));

    /* ---- ns.sock ---- */
    v8::Local<v8::ObjectTemplate> sockObj = v8::ObjectTemplate::New(isolate);
    sockObj->Set(isolate, "open",           v8::FunctionTemplate::New(isolate, JsSockOpen));
    sockObj->Set(isolate, "listen",         v8::FunctionTemplate::New(isolate, JsSockListen));
    sockObj->Set(isolate, "accept",         v8::FunctionTemplate::New(isolate, JsSockAccept));
    sockObj->Set(isolate, "recv",           v8::FunctionTemplate::New(isolate, JsSockRecv));
    sockObj->Set(isolate, "send",           v8::FunctionTemplate::New(isolate, JsSockSend));
    sockObj->Set(isolate, "close",          v8::FunctionTemplate::New(isolate, JsSockClose));
    sockObj->Set(isolate, "setBlocking",    v8::FunctionTemplate::New(isolate, JsSockSetBlocking));
    sockObj->Set(isolate, "setNonBlocking", v8::FunctionTemplate::New(isolate, JsSockSetNonBlocking));
    sockObj->Set(isolate, "nread",          v8::FunctionTemplate::New(isolate, JsSockNRead));

    /* ---- ns.thread ---- */
    v8::Local<v8::ObjectTemplate> threadObj = v8::ObjectTemplate::New(isolate);
    threadObj->Set(isolate, "create",  v8::FunctionTemplate::New(isolate, JsThreadCreate));
    threadObj->Set(isolate, "id",      v8::FunctionTemplate::New(isolate, JsThreadId));
    threadObj->Set(isolate, "yield",   v8::FunctionTemplate::New(isolate, JsThreadYield));
    threadObj->Set(isolate, "setName", v8::FunctionTemplate::New(isolate, JsThreadSetName));
    threadObj->Set(isolate, "getName", v8::FunctionTemplate::New(isolate, JsThreadGetName));

    /* ---- ns.sema ---- */
    v8::Local<v8::ObjectTemplate> semaObj = v8::ObjectTemplate::New(isolate);
    semaObj->Set(isolate, "create",  v8::FunctionTemplate::New(isolate, JsSemaCreate));
    semaObj->Set(isolate, "wait",    v8::FunctionTemplate::New(isolate, JsSemaWait));
    semaObj->Set(isolate, "post",    v8::FunctionTemplate::New(isolate, JsSemaPost));
    semaObj->Set(isolate, "destroy", v8::FunctionTemplate::New(isolate, JsSemaDestroy));

    /* ---- ns.cond ---- */
    v8::Local<v8::ObjectTemplate> condObj = v8::ObjectTemplate::New(isolate);
    condObj->Set(isolate, "create",     v8::FunctionTemplate::New(isolate, JsCondCreate));
    condObj->Set(isolate, "signal",     v8::FunctionTemplate::New(isolate, JsCondSignal));
    condObj->Set(isolate, "broadcast",  v8::FunctionTemplate::New(isolate, JsCondBroadcast));
    condObj->Set(isolate, "wait",       v8::FunctionTemplate::New(isolate, JsCondWait));
    condObj->Set(isolate, "timedWait",  v8::FunctionTemplate::New(isolate, JsCondTimedWait));
    condObj->Set(isolate, "destroy",    v8::FunctionTemplate::New(isolate, JsCondDestroy));

    /* ---- ns.set ---- */
    v8::Local<v8::ObjectTemplate> setObj = v8::ObjectTemplate::New(isolate);
    setObj->Set(isolate, "create",   v8::FunctionTemplate::New(isolate, JsSetCreate));
    setObj->Set(isolate, "put",      v8::FunctionTemplate::New(isolate, JsSetPut));
    setObj->Set(isolate, "get",      v8::FunctionTemplate::New(isolate, JsSetGet));
    setObj->Set(isolate, "iget",     v8::FunctionTemplate::New(isolate, JsSetIGet));
    setObj->Set(isolate, "find",     v8::FunctionTemplate::New(isolate, JsSetFind));
    setObj->Set(isolate, "size",     v8::FunctionTemplate::New(isolate, JsSetSize));
    setObj->Set(isolate, "key",      v8::FunctionTemplate::New(isolate, JsSetKey));
    setObj->Set(isolate, "value",    v8::FunctionTemplate::New(isolate, JsSetValue));
    setObj->Set(isolate, "delete",   v8::FunctionTemplate::New(isolate, JsSetDelete));
    setObj->Set(isolate, "update",   v8::FunctionTemplate::New(isolate, JsSetUpdate));
    setObj->Set(isolate, "free",     v8::FunctionTemplate::New(isolate, JsSetFree));
    setObj->Set(isolate, "toObject", v8::FunctionTemplate::New(isolate, JsSetToObject));

    /* ---- ns.log (callable fn with sub-properties) ---- */
    v8::Local<v8::FunctionTemplate> logFn = v8::FunctionTemplate::New(isolate, JsLog);
    logFn->Set(isolate, "roll", v8::FunctionTemplate::New(isolate, JsLogRoll));

    /* ---- ns.config (callable fn with sub-properties) ---- */
    v8::Local<v8::FunctionTemplate> configFn = v8::FunctionTemplate::New(isolate, JsConfig);
    configFn->Set(isolate, "section",  v8::FunctionTemplate::New(isolate, JsConfigSection));
    configFn->Set(isolate, "sections", v8::FunctionTemplate::New(isolate, JsConfigSections));

    /* ---- ns top-level ---- */
    v8::Local<v8::ObjectTemplate> nsObj = v8::ObjectTemplate::New(isolate);
    nsObj->Set(isolate, "conn",       connObj);
    nsObj->Set(isolate, "shared",     sharedObj);
    nsObj->Set(isolate, "cache",      cacheObj);
    nsObj->Set(isolate, "info",       infoObj);
    nsObj->Set(isolate, "url",        urlObj);
    nsObj->Set(isolate, "html",       htmlObj);
    nsObj->Set(isolate, "file",       fileObj);
    nsObj->Set(isolate, "dns",        dnsObj);
    nsObj->Set(isolate, "sched",      schedObj);
    nsObj->Set(isolate, "mutex",      mutexObj);
    nsObj->Set(isolate, "rwlock",     rwlockObj);
    nsObj->Set(isolate, "image",      imageObj);
    nsObj->Set(isolate, "env",        envObj);
    nsObj->Set(isolate, "process",    processObj);
    nsObj->Set(isolate, "http",       httpObj);
    nsObj->Set(isolate, "sock",       sockObj);
    nsObj->Set(isolate, "thread",     threadObj);
    nsObj->Set(isolate, "sema",       semaObj);
    nsObj->Set(isolate, "cond",       condObj);
    nsObj->Set(isolate, "set",        setObj);
    nsObj->Set(isolate, "log",        logFn);
    nsObj->Set(isolate, "time",       timeFn);
    nsObj->Set(isolate, "config",     configFn);
    nsObj->Set(isolate, "configInt",  v8::FunctionTemplate::New(isolate, JsConfigInt));
    nsObj->Set(isolate, "configBool", v8::FunctionTemplate::New(isolate, JsConfigBool));
    nsObj->Set(isolate, "sleep",      v8::FunctionTemplate::New(isolate, JsSleep));
    nsObj->Set(isolate, "crypt",      v8::FunctionTemplate::New(isolate, JsCrypt));
    nsObj->Set(isolate, "rand",       v8::FunctionTemplate::New(isolate, JsRand));
    nsObj->Set(isolate, "atshutdown", v8::FunctionTemplate::New(isolate, JsAtShutdown));
    nsObj->Set(isolate, "atsignal",   v8::FunctionTemplate::New(isolate, JsAtSignal));

    globalTmpl->Set(isolate, "ns", nsObj);
}

/* -----------------------------------------------------------------------
 * RunScript — compile and execute a JavaScript string inside ctx
 * --------------------------------------------------------------------- */

static int RunScript(NsJsContext *ctx, const std::string &source,
                     const std::string &filename) {
    v8::Isolate *iso = ctx->dataPtr->isolate;

    v8::TryCatch   trycatch(iso);
    v8::Local<v8::Context> v8ctx = iso->GetCurrentContext();

    v8::Local<v8::String> src =
        v8::String::NewFromUtf8(iso, source.c_str(),
                                v8::NewStringType::kNormal,
                                static_cast<int>(source.size()))
            .ToLocalChecked();

    v8::Local<v8::String> fname =
        v8::String::NewFromUtf8(iso, filename.c_str()).ToLocalChecked();
    v8::ScriptOrigin origin(fname);  /* V8 14+ dropped the Isolate* arg */

    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(v8ctx, src, &origin).ToLocal(&script)) {
        v8::String::Utf8Value err(iso, trycatch.Exception());
        Ns_Log(Error, nc("nsjs: compile error in %s: %s"),
               filename.c_str(), *err ? *err : "(unknown)");
        if (!ctx->responseSent) Ns_ConnReturnInternalError(ctx->conn);
        return NS_ERROR;
    }

    v8::Local<v8::Value> result;
    if (!script->Run(v8ctx).ToLocal(&result)) {
        v8::String::Utf8Value err(iso, trycatch.Exception());
        Ns_Log(Error, nc("nsjs: runtime error in %s: %s"),
               filename.c_str(), *err ? *err : "(unknown)");
        if (!ctx->responseSent) Ns_ConnReturnInternalError(ctx->conn);
        return NS_ERROR;
    }

    return NS_OK;
}

/* -----------------------------------------------------------------------
 * JsRequest — handler for *.js requests
 * --------------------------------------------------------------------- */

static int JsRequest(void *arg, Ns_Conn *conn) {
    NsMod  *modPtr = static_cast<NsMod *>(arg);
    JsData *dataPtr = GetJsData(modPtr);

    /* Resolve URL to filesystem path */
    Ns_DString path;
    Ns_DStringInit(&path);
    if (Ns_UrlToFile(&path, modPtr->server,
                     conn->request->url) != NS_OK) {
        Ns_DStringFree(&path);
        return Ns_ConnReturnNotFound(conn);
    }

    struct stat st;
    if (stat(path.string, &st) != 0 || !S_ISREG(st.st_mode)) {
        Ns_DStringFree(&path);
        return Ns_ConnReturnNotFound(conn);
    }

    /* Read file */
    std::ifstream ifs(path.string);
    Ns_DStringFree(&path);
    if (!ifs) {
        return Ns_ConnReturnInternalError(conn);
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    std::string source = ss.str();

    /* Set up per-request context */
    NsJsContext ctx;
    ctx.dataPtr      = dataPtr;
    ctx.conn         = conn;
    ctx.responseSent = false;
    Ns_DStringInit(&ctx.output);

    v8::Isolate        *iso = dataPtr->isolate;
    v8::Isolate::Scope  isolate_scope(iso);
    v8::HandleScope     handle_scope(iso);

    v8::Local<v8::ObjectTemplate> tmpl =
        v8::Local<v8::ObjectTemplate>::New(iso, dataPtr->globalTmpl);
    v8::Local<v8::Context> v8ctx =
        v8::Context::New(iso, nullptr, tmpl);

    /* Store NsJsContext pointer in embedder data slot 0 */
    v8ctx->SetEmbedderData(0, v8::External::New(iso, &ctx));

    v8::Context::Scope ctx_scope(v8ctx);

    std::string filename(conn->request->url);
    int rc = RunScript(&ctx, source, filename);

    if (rc == NS_OK && !ctx.responseSent) {
        Ns_ConnReturnHtml(conn, 200,
                          ctx.output.string,
                          ctx.output.length);
    }

    Ns_DStringFree(&ctx.output);
    return rc;
}

/* -----------------------------------------------------------------------
 * CompileJsAdp — transform .jsadp source into executable JavaScript
 *
 * Static HTML segments are emitted as ns.conn.write("...") calls.
 * <% code %> blocks are injected verbatim.
 * --------------------------------------------------------------------- */

static std::string CompileJsAdp(const std::string &source) {
    std::string out;
    out.reserve(source.size() * 2);

    size_t pos = 0;
    size_t len = source.size();

    while (pos < len) {
        size_t tag = source.find("<%", pos);
        if (tag == std::string::npos) {
            /* Remaining text is all static HTML */
            std::string seg = source.substr(pos);
            if (!seg.empty()) {
                out += "ns.conn.write(\"";
                for (char c : seg) {
                    if      (c == '\\') out += "\\\\";
                    else if (c == '"')  out += "\\\"";
                    else if (c == '\n') out += "\\n";
                    else if (c == '\r') out += "\\r";
                    else                out += c;
                }
                out += "\");\n";
            }
            break;
        }

        /* Emit static HTML before the tag */
        if (tag > pos) {
            std::string seg = source.substr(pos, tag - pos);
            out += "ns.conn.write(\"";
            for (char c : seg) {
                if      (c == '\\') out += "\\\\";
                else if (c == '"')  out += "\\\"";
                else if (c == '\n') out += "\\n";
                else if (c == '\r') out += "\\r";
                else                out += c;
            }
            out += "\");\n";
        }

        /* Find closing %> */
        size_t end = source.find("%>", tag + 2);
        if (end == std::string::npos) {
            /* Unclosed tag — emit remainder as code */
            out += source.substr(tag + 2);
            break;
        }

        /* Emit raw code block */
        out += source.substr(tag + 2, end - (tag + 2));
        out += "\n";

        pos = end + 2;
    }

    return out;
}

/* -----------------------------------------------------------------------
 * JsAdpRequest — handler for *.jsadp requests
 * --------------------------------------------------------------------- */

static int JsAdpRequest(void *arg, Ns_Conn *conn) {
    NsMod  *modPtr  = static_cast<NsMod *>(arg);
    JsData *dataPtr = GetJsData(modPtr);

    /* Resolve URL to filesystem path */
    Ns_DString path;
    Ns_DStringInit(&path);
    if (Ns_UrlToFile(&path, modPtr->server,
                     conn->request->url) != NS_OK) {
        Ns_DStringFree(&path);
        return Ns_ConnReturnNotFound(conn);
    }

    struct stat st;
    if (stat(path.string, &st) != 0 || !S_ISREG(st.st_mode)) {
        Ns_DStringFree(&path);
        return Ns_ConnReturnNotFound(conn);
    }

    std::ifstream ifs(path.string);
    Ns_DStringFree(&path);
    if (!ifs) {
        return Ns_ConnReturnInternalError(conn);
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    std::string adpSource = ss.str();

    std::string jsSource = CompileJsAdp(adpSource);

    NsJsContext ctx;
    ctx.dataPtr      = dataPtr;
    ctx.conn         = conn;
    ctx.responseSent = false;
    Ns_DStringInit(&ctx.output);

    v8::Isolate        *iso = dataPtr->isolate;
    v8::Isolate::Scope  isolate_scope(iso);
    v8::HandleScope     handle_scope(iso);

    v8::Local<v8::ObjectTemplate> tmpl =
        v8::Local<v8::ObjectTemplate>::New(iso, dataPtr->globalTmpl);
    v8::Local<v8::Context> v8ctx =
        v8::Context::New(iso, nullptr, tmpl);

    v8ctx->SetEmbedderData(0, v8::External::New(iso, &ctx));
    v8::Context::Scope ctx_scope(v8ctx);

    std::string filename = std::string(conn->request->url) + " [jsadp]";
    int rc = RunScript(&ctx, jsSource, filename);

    if (rc == NS_OK && !ctx.responseSent) {
        Ns_ConnReturnHtml(conn, 200,
                          ctx.output.string,
                          ctx.output.length);
    }

    Ns_DStringFree(&ctx.output);
    return rc;
}

/* -----------------------------------------------------------------------
 * NsJs_ModInit — module entry point, called once per virtual server
 * --------------------------------------------------------------------- */

extern "C" int NsJs_ModInit(char *server, char *module) {
    EnsurePlatformInit();
    EnsureTlsAlloc();

    NsMod *modPtr = static_cast<NsMod *>(ns_calloc(1, sizeof(NsMod)));
    modPtr->server   = ns_strdup(server);
    modPtr->module   = ns_strdup(module);

    /* Resolve page root — default to server's page root */
    Ns_DString ds;
    Ns_DStringInit(&ds);
    Ns_UrlToFile(&ds, server, nc("/"));
    modPtr->pageRoot = ns_strdup(ds.string);
    Ns_DStringFree(&ds);

    static const char *methods[] = { "GET", "HEAD", "POST", nullptr };

    for (int i = 0; methods[i] != nullptr; i++) {
        Ns_RegisterRequest(server, nc(methods[i]), nc("/*.js"),
                           JsRequest,
                           static_cast<Ns_Callback *>(nullptr),
                           modPtr, 0);
        Ns_RegisterRequest(server, nc(methods[i]), nc("/*.jsadp"),
                           JsAdpRequest,
                           static_cast<Ns_Callback *>(nullptr),
                           modPtr, 0);
    }

    /* Optional jscp configuration */
    {
        char section[256];
        snprintf(section, sizeof(section),
                 "ns/server/%s/module/%s", server, module);
        char *addr    = Ns_ConfigGetValue(section, nc("jscp_address"));
        int   port    = 0;
        Ns_ConfigGetInt(section, nc("jscp_port"), &port);
        if (addr != nullptr && port > 0) {
            NsJsCpConfig *cfg = new NsJsCpConfig();
            cfg->address     = addr;
            cfg->port        = port;
            cfg->modPtr      = modPtr;
            cfg->logCommands = false;
            cfg->maxSessions = 5;

            int logCmd = 0;
            Ns_ConfigGetBool(section, nc("jscp_log"), &logCmd);
            cfg->logCommands = (logCmd != 0);

            int maxs = 0;
            if (Ns_ConfigGetInt(section, nc("jscp_max_sessions"), &maxs) && maxs > 0)
                cfg->maxSessions = maxs;

            /* Parse "user1:pass1 user2:pass2" users string */
            char *usersStr = Ns_ConfigGetValue(section, nc("jscp_users"));
            if (usersStr != nullptr) {
                std::string us(usersStr);
                size_t pos = 0;
                while (pos < us.size()) {
                    size_t sp = us.find(' ', pos);
                    std::string tok = (sp == std::string::npos)
                        ? us.substr(pos)
                        : us.substr(pos, sp - pos);
                    pos = (sp == std::string::npos) ? us.size() : sp + 1;
                    if (tok.empty()) continue;
                    size_t col = tok.find(':');
                    if (col != std::string::npos) {
                        JsCpUser u;
                        u.username = tok.substr(0, col);
                        u.password = tok.substr(col + 1);
                        cfg->users.push_back(u);
                    }
                }
            }

            js_cp_config = cfg;
            JsCpInit(cfg);
            Ns_Log(Notice, nc("nsjs jscp: configured on %s:%d"), addr, port);
        }
    }

    Ns_Log(Notice, nc("nsjs: module loaded for server '%s'"), server);
    return NS_OK;
}
