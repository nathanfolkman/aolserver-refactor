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
 *     ns.conn.write(str)
 *     ns.conn.getHeader(name)
 *     ns.conn.setHeader(name, value)
 *     ns.conn.getQuery(name)
 *     ns.conn.getMethod()
 *     ns.conn.getUrl()
 *     ns.log(level, msg)
 *     ns.shared.set(array, key, value)
 *     ns.shared.get(array, key)
 *     ns.shared.exists(array, key)
 *     ns.shared.unset(array, key)
 *     ns.shared.incr(array, key, delta)
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
 * BuildGlobalTemplate — construct the `ns` object hierarchy once per thread
 * --------------------------------------------------------------------- */

static void BuildGlobalTemplate(v8::Isolate *isolate,
                                v8::Local<v8::ObjectTemplate> &globalTmpl) {
    /* ns.conn object */
    v8::Local<v8::ObjectTemplate> connObj =
        v8::ObjectTemplate::New(isolate);
    connObj->Set(isolate, "write",
        v8::FunctionTemplate::New(isolate, JsConnWrite));
    connObj->Set(isolate, "getHeader",
        v8::FunctionTemplate::New(isolate, JsConnGetHeader));
    connObj->Set(isolate, "setHeader",
        v8::FunctionTemplate::New(isolate, JsConnSetHeader));
    connObj->Set(isolate, "getQuery",
        v8::FunctionTemplate::New(isolate, JsConnGetQuery));
    connObj->Set(isolate, "getMethod",
        v8::FunctionTemplate::New(isolate, JsConnGetMethod));
    connObj->Set(isolate, "getUrl",
        v8::FunctionTemplate::New(isolate, JsConnGetUrl));

    /* ns.shared object */
    v8::Local<v8::ObjectTemplate> sharedObj =
        v8::ObjectTemplate::New(isolate);
    sharedObj->Set(isolate, "set",
        v8::FunctionTemplate::New(isolate, JsSharedSet));
    sharedObj->Set(isolate, "get",
        v8::FunctionTemplate::New(isolate, JsSharedGet));
    sharedObj->Set(isolate, "exists",
        v8::FunctionTemplate::New(isolate, JsSharedExists));
    sharedObj->Set(isolate, "unset",
        v8::FunctionTemplate::New(isolate, JsSharedUnset));
    sharedObj->Set(isolate, "incr",
        v8::FunctionTemplate::New(isolate, JsSharedIncr));

    /* ns top-level object */
    v8::Local<v8::ObjectTemplate> nsObj =
        v8::ObjectTemplate::New(isolate);
    nsObj->Set(isolate, "conn",   connObj);
    nsObj->Set(isolate, "shared", sharedObj);
    nsObj->Set(isolate, "log",
        v8::FunctionTemplate::New(isolate, JsLog));

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
        Ns_ConnReturnInternalError(ctx->conn);
        return NS_ERROR;
    }

    v8::Local<v8::Value> result;
    if (!script->Run(v8ctx).ToLocal(&result)) {
        v8::String::Utf8Value err(iso, trycatch.Exception());
        Ns_Log(Error, nc("nsjs: runtime error in %s: %s"),
               filename.c_str(), *err ? *err : "(unknown)");
        Ns_ConnReturnInternalError(ctx->conn);
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
    ctx.dataPtr = dataPtr;
    ctx.conn    = conn;
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

    if (rc == NS_OK) {
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
    ctx.dataPtr = dataPtr;
    ctx.conn    = conn;
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

    if (rc == NS_OK) {
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
