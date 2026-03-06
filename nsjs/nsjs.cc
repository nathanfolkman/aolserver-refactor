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
 *                        returnHtml, returnFile, close)
 *     ns.shared.*      — cross-thread shared vars (set, get, exists, unset, incr,
 *                        append, lappend, names, keys, getAll)
 *     ns.cache.*       — named caches (create, get, set, unset, flush, stats)
 *     ns.config()      — config access (config, configInt, configBool)
 *     ns.info.*        — server info (version, uptime, pageroot, log, config,
 *                        hostname, address, pid)
 *     ns.time()        — time functions (now, format, httpTime, parseHttpTime,
 *                        gmtime, localtime)
 *     ns.url.*         — URL utilities (encode, decode, parse, toFile)
 *     ns.html.*        — HTML utilities (quote, strip, guessType)
 *     ns.file.*        — file I/O (read, write, exists, stat, mkdir, rmdir,
 *                        unlink, cp, rename, tmpnam, normalizePath)
 *     ns.dns.*         — DNS lookups (addrByHost, hostByAddr)
 *     ns.sched.*       — scheduling (after, interval, cancel)
 *     ns.mutex.*       — mutexes (create, lock, unlock, trylock, destroy)
 *     ns.rwlock.*      — rwlocks (create, readLock, writeLock, unlock, destroy)
 *     ns.log(level, msg)
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
#include <cerrno>
#include <ctime>
#include <cstdio>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/types.h>

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
    if (ctx == nullptr || args.Length() < 1) {
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
    if (ctx == nullptr || args.Length() < 2) return;
    std::string name  = V8ToString(iso, args[0]);
    std::string value = V8ToString(iso, args[1]);
    Ns_SetUpdate(ctx->conn->outputheaders, nc(name), nc(value));
}

/* ns.conn.getQuery(name) */
static void JsConnGetQuery(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || args.Length() < 1) {
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
    if (ctx == nullptr || ctx->conn->request == nullptr) {
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
    if (ctx == nullptr || ctx->conn->request == nullptr) {
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
    if (ctx == nullptr) { args.GetReturnValue().SetNull(); return; }
    char *peer = Ns_ConnPeer(ctx->conn);
    args.GetReturnValue().Set(v8s(iso, peer));
}

/* ns.conn.getHost() */
static void JsConnGetHost(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr) { args.GetReturnValue().SetNull(); return; }
    char *host = Ns_ConnHost(ctx->conn);
    args.GetReturnValue().Set(v8s(iso, host));
}

/* ns.conn.getPort() */
static void JsConnGetPort(const v8::FunctionCallbackInfo<v8::Value> &args) {
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr) { args.GetReturnValue().Set(0); return; }
    args.GetReturnValue().Set(Ns_ConnPort(ctx->conn));
}

/* ns.conn.getId() */
static void JsConnGetId(const v8::FunctionCallbackInfo<v8::Value> &args) {
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr) { args.GetReturnValue().Set(-1); return; }
    args.GetReturnValue().Set(Ns_ConnId(ctx->conn));
}

/* ns.conn.getAuthUser() */
static void JsConnGetAuthUser(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr) { args.GetReturnValue().SetNull(); return; }
    char *user = Ns_ConnAuthUser(ctx->conn);
    if (user != nullptr) args.GetReturnValue().Set(v8s(iso, user));
    else args.GetReturnValue().SetNull();
}

/* ns.conn.getAuthPasswd() */
static void JsConnGetAuthPasswd(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr) { args.GetReturnValue().SetNull(); return; }
    char *pw = Ns_ConnAuthPasswd(ctx->conn);
    if (pw != nullptr) args.GetReturnValue().Set(v8s(iso, pw));
    else args.GetReturnValue().SetNull();
}

/* ns.conn.getAllHeaders() — returns JS object with all request headers */
static void JsConnGetAllHeaders(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr) { args.GetReturnValue().SetNull(); return; }
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
    if (ctx == nullptr) { args.GetReturnValue().SetNull(); return; }
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
    if (ctx == nullptr) { args.GetReturnValue().SetNull(); return; }
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
    if (ctx == nullptr || args.Length() < 1) return;
    int code = static_cast<int>(
        args[0]->Int32Value(args.GetIsolate()->GetCurrentContext()).FromMaybe(200));
    Ns_ConnSetStatus(ctx->conn, code);
}

/* ns.conn.setContentType(type) */
static void JsConnSetContentType(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || args.Length() < 1) return;
    std::string type = V8ToString(iso, args[0]);
    Ns_ConnSetType(ctx->conn, nc(type));
}

/* ns.conn.returnRedirect(url) — sends redirect and marks response sent */
static void JsConnReturnRedirect(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || args.Length() < 1) return;
    std::string url = V8ToString(iso, args[0]);
    Ns_ConnReturnRedirect(ctx->conn, nc(url));
    ctx->responseSent = true;
}

/* ns.conn.returnHtml(status, html) — sends HTML response and marks response sent */
static void JsConnReturnHtml(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    NsJsContext *ctx = GetCtx(args);
    if (ctx == nullptr || args.Length() < 2) return;
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
    if (ctx == nullptr || args.Length() < 3) return;
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
    if (ctx == nullptr) return;
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

/* ns.html.strip(str) — strip HTML tags */
static void JsHtmlStrip(const v8::FunctionCallbackInfo<v8::Value> &args) {
    v8::Isolate *iso = args.GetIsolate();
    if (args.Length() < 1) { args.GetReturnValue().Set(v8s(iso,"")); return; }
    std::string s = V8ToString(iso, args[0]);
    std::string result;
    result.reserve(s.size());
    bool inTag = false;
    for (char c : s) {
        if      (c == '<') inTag = true;
        else if (c == '>') inTag = false;
        else if (!inTag)   result += c;
    }
    args.GetReturnValue().Set(v8s(iso, result));
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
    connObj->Set(isolate, "close",          v8::FunctionTemplate::New(isolate, JsConnClose));

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
    infoObj->Set(isolate, "pid",      v8::FunctionTemplate::New(isolate, JsInfoPid));

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
    htmlObj->Set(isolate, "strip",     v8::FunctionTemplate::New(isolate, JsHtmlStrip));
    htmlObj->Set(isolate, "guessType", v8::FunctionTemplate::New(isolate, JsHtmlGuessType));

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

    /* ---- ns.dns ---- */
    v8::Local<v8::ObjectTemplate> dnsObj = v8::ObjectTemplate::New(isolate);
    dnsObj->Set(isolate, "addrByHost", v8::FunctionTemplate::New(isolate, JsDnsAddrByHost));
    dnsObj->Set(isolate, "hostByAddr", v8::FunctionTemplate::New(isolate, JsDnsHostByAddr));

    /* ---- ns.sched ---- */
    v8::Local<v8::ObjectTemplate> schedObj = v8::ObjectTemplate::New(isolate);
    schedObj->Set(isolate, "after",    v8::FunctionTemplate::New(isolate, JsSchedAfter));
    schedObj->Set(isolate, "interval", v8::FunctionTemplate::New(isolate, JsSchedInterval));
    schedObj->Set(isolate, "cancel",   v8::FunctionTemplate::New(isolate, JsSchedCancel));

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

    /* ---- ns top-level (callable config + sub-objects) ---- */
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
    nsObj->Set(isolate, "log",        v8::FunctionTemplate::New(isolate, JsLog));
    nsObj->Set(isolate, "time",       timeFn);
    nsObj->Set(isolate, "config",     v8::FunctionTemplate::New(isolate, JsConfig));
    nsObj->Set(isolate, "configInt",  v8::FunctionTemplate::New(isolate, JsConfigInt));
    nsObj->Set(isolate, "configBool", v8::FunctionTemplate::New(isolate, JsConfigBool));

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

    Ns_Log(Notice, nc("nsjs: module loaded for server '%s'"), server);
    return NS_OK;
}
