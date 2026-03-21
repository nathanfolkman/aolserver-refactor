/*
 * http2.c -- HTTP/2 (RFC 7540) over TLS using nghttp2.
 */

#include "nsd.h"

#if HAVE_NGHTTP2

#include <nghttp2/nghttp2.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdatomic.h>

/* Set to 1 while debugging h2spec failures (stderr, line-buffered by libc on TTY). */
#ifndef AOLSERVER_H2_TRACE
#define AOLSERVER_H2_TRACE 0
#endif

#if AOLSERVER_H2_TRACE
static void
H2Trace(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vdprintf(2, fmt, ap);
    va_end(ap);
    dprintf(2, "\n");
}
#else
#define H2Trace(...) ((void) 0)
#endif

/*
 * When AOLSERVER_H2_DEBUG is set (non-empty, not "0"), log NsHttp2Feed results
 * to stderr. Complements driver.c SockClose logging for the same env var.
 */
static int
H2DebugEnv(void)
{
    static int cache = -1;

    if (cache < 0) {
	const char *e = getenv("AOLSERVER_H2_DEBUG");

	if (e == NULL || e[0] == '\0' || strcmp(e, "0") == 0) {
	    cache = 0;
	} else {
	    cache = 1;
	}
    }
    return cache;
}

static void
H2DebugFeedMsg(const char *msg, unsigned int sockId, size_t datalen, int rv,
	       const char *extra)
{
    if (!H2DebugEnv()) {
	return;
    }
    if (extra != NULL) {
	fprintf(stderr,
		"AOLSERVER_H2_DEBUG NsHttp2Feed id=%u datalen=%lu rv=%d %s (%s)\n",
		sockId, (unsigned long) datalen, rv, msg, extra);
    } else {
	fprintf(stderr,
		"AOLSERVER_H2_DEBUG NsHttp2Feed id=%u datalen=%lu rv=%d %s\n",
		sockId, (unsigned long) datalen, rv, msg);
    }
    fflush(stderr);
}

/* Match driver.c ReadErr ordinals */
enum {
    H2_E_OK = 0,
    H2_E_HINVAL = 9
};

static _Atomic unsigned long long h2_stat_feed_ok;
static _Atomic unsigned long long h2_stat_feed_mem_recv_err;
static _Atomic unsigned long long h2_stat_trysend_recoveries;
static _Atomic unsigned long long h2_stat_sessions_created;
static _Atomic unsigned long long h2_stat_sessions_destroyed;
static _Atomic unsigned long long h2_stat_streams_dispatched;
static _Atomic unsigned long long h2_stat_rst_stream_sent;
static _Atomic unsigned long long h2_stat_session_send_fail;
static _Atomic unsigned long long h2_stat_bytes_sent;
static _Atomic unsigned long long h2_stat_ping_recv;
static _Atomic unsigned long long h2_stat_goaway_recv;
static _Atomic unsigned long long h2_stat_rst_stream_recv;
static _Atomic unsigned long long h2_stat_goaway_sent;
static _Atomic unsigned long long h2_stat_ping_sent;
static _Atomic unsigned long long h2_stat_ping_ack_sent;
static _Atomic unsigned long long h2_stat_defer_appends;
static _Atomic unsigned long long h2_stat_defer_max_depth;
static _Atomic unsigned long long h2_stat_trysend_drain_reads;
static _Atomic unsigned long long h2_stat_bytes_fed;

#define H2_STAT_INC(v) \
    (void) atomic_fetch_add_explicit(&(v), 1ULL, memory_order_relaxed)
#define H2_STAT_ADD(v, n) \
    (void) atomic_fetch_add_explicit(&(v), (unsigned long long) (n), \
	    memory_order_relaxed)

/*
 * Mirror each counter into the owning Driver for ns_driver query / per-driver views.
 */
#define H2_DUAL_ADD(drv, field, n)                                                      \
    do {                                                                                \
	H2_STAT_ADD(h2_stat_##field, (unsigned long long) (n));                         \
	if ((drv) != NULL) {                                                            \
	    (void) atomic_fetch_add_explicit(&(drv)->h2.field,                          \
		    (unsigned long long) (n), memory_order_relaxed);                    \
	}                                                                               \
    } while (0)
#define H2_DUAL_INC(drv, field) H2_DUAL_ADD(drv, field, 1ULL)

static void
H2DeferUpdateMaxDepth(Driver *drv, unsigned long long depth)
{
    _Atomic unsigned long long *cells[2];
    int i, n = 0;

    cells[n++] = &h2_stat_defer_max_depth;
    if (drv != NULL) {
	cells[n++] = &drv->h2.defer_max_depth;
    }
    for (i = 0; i < n; i++) {
	unsigned long long cur;
	_Atomic unsigned long long *a = cells[i];

	do {
	    cur = atomic_load_explicit(a, memory_order_relaxed);
	    if (depth <= cur) {
		break;
	    }
	} while (!atomic_compare_exchange_weak_explicit(a, &cur, depth,
		memory_order_relaxed, memory_order_relaxed));
    }
}

typedef struct H2Stream {
    int32_t stream_id;
    Sock *sock;
    char *method;
    char *path;
    char *scheme;
    char *authority;
    Ns_Set *hdrs;
    Ns_DString body;
    int dispatched;
    int defer_pend; /* Set while stream is on sock's h2 defer queue (at most once). */
    /*
     * nghttp2 may call on_stream_close while the stream is still in the defer
     * queue (e.g. RST_STREAM / protocol errors during h2spec). Do not free in
     * the close callback in that case; DispatchH2Stream drops the work.
     */
    int closed_before_dispatch;
} H2Stream;

static void H2StreamDestroy(H2Stream *s);
static void H2ClearStreamUserData(Sock *sock, int32_t stream_id);
static void H2FailStream(Sock *sock, int32_t stream_id, uint32_t err);
static int DispatchH2Stream(H2Stream *s);
static int H2DispatchDeferred(Sock *sockPtr);

typedef struct H2Defer {
    H2Stream *s;
    struct H2Defer *next;
} H2Defer;

static void H2DeferAppend(Sock *sock, H2Stream *s)
{
    H2Defer *d;
    H2Defer *p;
    unsigned long long depth = 0u;

    if (s->defer_pend != 0) {
	return;
    }
    s->defer_pend = 1;
    d = (H2Defer *) ns_malloc(sizeof(H2Defer));
    d->s = s;
    d->next = NULL;
    if (sock->h2DeferLast == NULL) {
	sock->h2DeferFirst = sock->h2DeferLast = d;
    } else {
	((H2Defer *) sock->h2DeferLast)->next = d;
	sock->h2DeferLast = d;
    }
    for (p = (H2Defer *) sock->h2DeferFirst; p != NULL; p = p->next) {
	depth++;
    }
    H2_DUAL_INC(sock->drvPtr, defer_appends);
    H2DeferUpdateMaxDepth(sock->drvPtr, depth);
}

static H2Defer *
H2DeferGrabAll(Sock *sock)
{
    H2Defer *head = (H2Defer *) sock->h2DeferFirst;

    sock->h2DeferFirst = sock->h2DeferLast = NULL;
    return head;
}

static void H2DeferAbort(Sock *sock)
{
    H2Defer *d = (H2Defer *) sock->h2DeferFirst;

    while (d != NULL) {
	H2Defer *next = d->next;

	H2StreamDestroy(d->s);
	ns_free(d);
	d = next;
    }
    sock->h2DeferFirst = sock->h2DeferLast = NULL;
}

static void H2StreamDestroy(H2Stream *s)
{
    if (s == NULL) {
        return;
    }
    ns_free(s->method);
    ns_free(s->path);
    ns_free(s->scheme);
    ns_free(s->authority);
    if (s->hdrs != NULL) {
        Ns_SetFree(s->hdrs);
    }
    Ns_DStringFree(&s->body);
    ns_free(s);
}

static ssize_t H2SendCb(nghttp2_session *session, const uint8_t *data, size_t length,
                        int flags, void *user_data)
{
    Sock *sock = (Sock *) user_data;
    struct iovec iov;

    (void) session;
    (void) flags;
    iov.iov_base = (void *) data;
    iov.iov_len = length;
    {
        int n = (*sock->drvPtr->proc)(DriverSend, (Ns_Sock *) sock, &iov, 1);

        if (n < 0) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        /*
         * Non-blocking TLS may write 0 bytes (WANT_READ/WANT_WRITE). nghttp2
         * requires NGHTTP2_ERR_WOULDBLOCK — returning 0 corrupts its serializer.
         */
        if (n == 0) {
            return (ssize_t) NGHTTP2_ERR_WOULDBLOCK;
        }
	H2_DUAL_ADD(sock->drvPtr, bytes_sent, (unsigned long long) n);
        return (ssize_t) n;
    }
}

static int H2OnBeginHeaders(nghttp2_session *session, const nghttp2_frame *frame,
                            void *user_data)
{
    Sock *sock = (Sock *) user_data;
    H2Stream *s;

    (void) session;
    if (frame->hd.type != NGHTTP2_HEADERS
        || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }
    s = (H2Stream *) ns_calloc(1, sizeof(H2Stream));
    s->stream_id = frame->hd.stream_id;
    s->sock = sock;
    s->hdrs = Ns_SetCreate(NULL);
    nghttp2_session_set_stream_user_data(session, frame->hd.stream_id, s);
    return 0;
}

static int SavePseudo(H2Stream *s, const char *name, const char *value)
{
    if (strcmp(name, ":method") == 0) {
        ns_free(s->method);
        s->method = ns_strdup(value);
    } else if (strcmp(name, ":path") == 0) {
        ns_free(s->path);
        s->path = ns_strdup(value);
    } else if (strcmp(name, ":scheme") == 0) {
        ns_free(s->scheme);
        s->scheme = ns_strdup(value);
    } else if (strcmp(name, ":authority") == 0) {
        ns_free(s->authority);
        s->authority = ns_strdup(value);
    }
    return 0;
}

static int H2OnHeader(nghttp2_session *session, const nghttp2_frame *frame,
                      const uint8_t *name, size_t namelen,
                      const uint8_t *value, size_t valuelen,
                      uint8_t flags, void *user_data)
{
    H2Stream *s;
    char *nbuf, *vbuf;

    (void) user_data;
    (void) flags;
    s = (H2Stream *) nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    if (s == NULL) {
        return 0;
    }
    nbuf = ns_malloc(namelen + 1u);
    memcpy(nbuf, name, namelen);
    nbuf[namelen] = '\0';
    vbuf = ns_malloc(valuelen + 1u);
    memcpy(vbuf, value, valuelen);
    vbuf[valuelen] = '\0';

    if (nbuf[0] == ':') {
        SavePseudo(s, nbuf, vbuf);
    } else {
        Ns_SetPut(s->hdrs, nbuf, vbuf);
    }
    ns_free(nbuf);
    ns_free(vbuf);
    return 0;
}

static int H2OnDataChunk(nghttp2_session *session, uint8_t flags, int32_t stream_id,
                         const uint8_t *data, size_t len, void *user_data)
{
    H2Stream *s;

    (void) session;
    (void) flags;
    (void) user_data;
    s = (H2Stream *) nghttp2_session_get_stream_user_data(session, stream_id);
    if (s == NULL) {
        return 0;
    }
    Ns_DStringNAppend(&s->body, (const char *) data, (int) len);
    return 0;
}

static int DispatchH2Stream(H2Stream *s)
{
    Ns_DString line;
    Ns_Request *req;
    Conn *connPtr;
    Ns_Time now;
    int i, hl;
    char *clHeader;
    int contentLen = 0;

    if (s->closed_before_dispatch) {
	H2StreamDestroy(s);
	return 0;
    }
    if (s->dispatched || s->method == NULL || s->path == NULL) {
        return 0;
    }
    s->dispatched = 1;

    Ns_DStringInit(&line);
    Ns_DStringPrintf(&line, "%s %s HTTP/2.0", s->method, s->path);

    Ns_GetTime(&now);
    connPtr = NsDriverAllocConn(s->sock->drvPtr, &now, s->sock);

    Ns_SetFree(connPtr->headers);
    connPtr->headers = s->hdrs;
    s->hdrs = NULL;

    if (s->authority != NULL && Ns_SetIGet(connPtr->headers, "host") == NULL) {
        Ns_SetPut(connPtr->headers, "host", s->authority);
    }

    if (NsDriverMapHostToServer(connPtr) != NS_OK) {
        goto failEarly;
    }

    req = Ns_ParseRequestEx(line.string, connPtr->servPtr->urlEncoding);
    Ns_DStringFree(&line);
    if (req == NULL || req->method == NULL) {
	H2FailStream(s->sock, s->stream_id, NGHTTP2_PROTOCOL_ERROR);
        connPtr->sockPtr = NULL;
        NsFreeConn(connPtr);
        H2StreamDestroy(s);
        return 0;
    }

    connPtr->request = req;
    connPtr->major = 2u;
    connPtr->minor = 0u;

    connPtr->limitsPtr = NsGetRequestLimits(connPtr->server, req->method, req->url);
    clHeader = Ns_SetIGet(connPtr->headers, "content-length");
    if (clHeader != NULL) {
        if (sscanf(clHeader, "%d", &contentLen) != 1 || contentLen < 0) {
	    H2FailStream(s->sock, s->stream_id, NGHTTP2_PROTOCOL_ERROR);
            goto failConn;
        }
    } else {
        contentLen = (int) Ns_DStringLength(&s->body);
    }

    if (contentLen > (int) connPtr->limitsPtr->maxupload) {
	H2FailStream(s->sock, s->stream_id, NGHTTP2_REFUSED_STREAM);
        goto failConn;
    }

    connPtr->contentLength = contentLen;
    if (STREQ(req->method, "HEAD")) {
        connPtr->flags |= NS_CONN_SKIPBODY;
    }

    if (contentLen > 0) {
        connPtr->content = ns_malloc((size_t) contentLen + 1u);
        memcpy(connPtr->content, s->body.string, (size_t) contentLen);
        connPtr->content[contentLen] = '\0';
        connPtr->avail = (size_t) contentLen;
        connPtr->next = connPtr->content;
        connPtr->flags |= NS_CONN_H2_BODY;
    } else {
        connPtr->content = connPtr->next = (char *) "";
        connPtr->avail = 0;
    }

    connPtr->flags |= (NS_CONN_READHDRS | NS_CONN_HTTP2 | NS_CONN_KEEPALIVE);
    connPtr->http2_session = NULL;
    connPtr->http2_stream_id = s->stream_id;
    connPtr->http2_response_started = 0;

    hl = Ns_SetSize(connPtr->headers);
    for (i = 0; i < hl; ++i) {
        if (connPtr->servPtr->opts.hdrcase == ToLower) {
            Ns_StrToLower(Ns_SetKey(connPtr->headers, i));
        } else if (connPtr->servPtr->opts.hdrcase == ToUpper) {
            Ns_StrToUpper(Ns_SetKey(connPtr->headers, i));
        }
        /* Preserve: leave as-is */
    }

    if (NsRunFilters((Ns_Conn *) connPtr, NS_FILTER_PRE_QUEUE) != NS_OK) {
	H2FailStream(s->sock, s->stream_id, NGHTTP2_REFUSED_STREAM);
        goto failConn;
    }

    {
        Driver *drv = s->sock->drvPtr;

	H2ClearStreamUserData(s->sock, s->stream_id);
        H2StreamDestroy(s);
	H2_DUAL_INC(drv, streams_dispatched);
        NsDriverH2PendingAppend(drv, connPtr);
    }
    return 0;

failEarly:
    Ns_DStringFree(&line);
    H2ClearStreamUserData(s->sock, s->stream_id);
    connPtr->sockPtr = NULL;
    NsFreeConn(connPtr);
    H2StreamDestroy(s);
    return 0;

failConn:
    connPtr->sockPtr = NULL;
    NsFreeConn(connPtr);
    H2StreamDestroy(s);
    return 0;
}

static int H2OnFrameRecv(nghttp2_session *session, const nghttp2_frame *frame,
                         void *user_data)
{
    H2Stream *s;
    Sock *sock = (Sock *) user_data;

    (void) session;
    if (frame->hd.type == NGHTTP2_PING) {
	H2_DUAL_INC(sock->drvPtr, ping_recv);
    } else if (frame->hd.type == NGHTTP2_GOAWAY) {
	H2_DUAL_INC(sock->drvPtr, goaway_recv);
    } else if (frame->hd.type == NGHTTP2_RST_STREAM) {
	H2_DUAL_INC(sock->drvPtr, rst_stream_recv);
    }

    if (frame->hd.type == NGHTTP2_HEADERS
        && frame->headers.cat == NGHTTP2_HCAT_REQUEST
        && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
        && (frame->hd.flags & NGHTTP2_FLAG_END_HEADERS)) {
        s = (H2Stream *) nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
        if (s != NULL) {
	    H2DeferAppend(sock, s);
        }
    } else if (frame->hd.type == NGHTTP2_HEADERS
	    && frame->headers.cat == NGHTTP2_HCAT_HEADERS
	    && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
	    && (frame->hd.flags & NGHTTP2_FLAG_END_HEADERS)) {
	/*
	 * Trailer block (RFC 7540) ends the stream; defer like DATA+END_STREAM.
	 */
	s = (H2Stream *) nghttp2_session_get_stream_user_data(session,
		frame->hd.stream_id);
	if (s != NULL) {
	    H2DeferAppend(sock, s);
	}
    } else if (frame->hd.type == NGHTTP2_DATA
               && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        s = (H2Stream *) nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
        if (s != NULL) {
	    H2DeferAppend(sock, s);
        }
    } else if (frame->hd.type == NGHTTP2_CONTINUATION
               && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
               && (frame->hd.flags & NGHTTP2_FLAG_END_HEADERS)) {
	/*
	 * Header blocks split across HEADERS + CONTINUATION may carry END_STREAM only
	 * on the last CONTINUATION (RFC 7540 6.10). Require END_HEADERS so we do not
	 * double-defer with a prior HEADERS that already ended the block.
	 */
	s = (H2Stream *) nghttp2_session_get_stream_user_data(session,
		frame->hd.stream_id);
	if (s != NULL) {
	    H2DeferAppend(sock, s);
	}
    }
    return 0;
}

static int H2OnStreamClose(nghttp2_session *session, int32_t stream_id,
                           uint32_t error_code, void *user_data)
{
    H2Stream *s;

    (void) error_code;
    (void) user_data;
    s = (H2Stream *) nghttp2_session_get_stream_user_data(session, stream_id);
    if (s != NULL) {
        nghttp2_session_set_stream_user_data(session, stream_id, NULL);
	/*
	 * H2DeferAppend may still hold a pointer to this stream. Freeing here
	 * races H2DispatchDeferred and crashes (signal 11) under h2spec.
	 */
	if (s->defer_pend) {
	    s->closed_before_dispatch = 1;
	    return 0;
	}
        H2StreamDestroy(s);
    }
    return 0;
}

/*
 * Count frames actually serialized onto the wire (RST/GOAWAY/PING symmetry with recv).
 */
static int
H2OnFrameSend(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    Sock *sock = (Sock *) user_data;

    (void) session;
    switch (frame->hd.type) {
    case NGHTTP2_RST_STREAM:
	H2_DUAL_INC(sock->drvPtr, rst_stream_sent);
	break;
    case NGHTTP2_GOAWAY:
	H2_DUAL_INC(sock->drvPtr, goaway_sent);
	break;
    case NGHTTP2_PING:
	if ((frame->hd.flags & NGHTTP2_FLAG_ACK) != 0) {
	    H2_DUAL_INC(sock->drvPtr, ping_ack_sent);
	} else {
	    H2_DUAL_INC(sock->drvPtr, ping_sent);
	}
	break;
    default:
	break;
    }
    return 0;
}

/*
 * Optional nghttp2_session options from ns/server/.../module/<driver> (e.g. nsssl).
 */
static void
H2ApplySockModuleOptions(nghttp2_option *opt, Sock *sock)
{
    char *path;
    int n;

    path = Ns_ConfigGetPath(sock->drvPtr->server, sock->drvPtr->module, NULL);
    if (path == NULL) {
	return;
    }
    if (Ns_ConfigGetInt(path, "h2_max_deflate_dynamic_table_size", &n) == NS_TRUE
	    && n >= 0) {
	nghttp2_option_set_max_deflate_dynamic_table_size(opt, (uint32_t) n);
    }
    if (Ns_ConfigGetInt(path, "h2_max_send_header_block_length", &n) == NS_TRUE
	    && n > 0) {
	nghttp2_option_set_max_send_header_block_length(opt, (uint32_t) n);
    }
}

/*
 * Server connection preface: SETTINGS without ACK. If no h2_* SETTINGS params
 * are set, submit an empty SETTINGS (nghttp2/RFC defaults).
 */
static int
H2SubmitPrefaceSettings(nghttp2_session *session, Sock *sock)
{
    char *path;
    nghttp2_settings_entry iv[8];
    size_t niv = 0;
    int n, rv;
    uint32_t u;

    path = Ns_ConfigGetPath(sock->drvPtr->server, sock->drvPtr->module, NULL);
    if (path == NULL) {
	return nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, NULL, 0);
    }
    if (Ns_ConfigGetInt(path, "h2_header_table_size", &n) == NS_TRUE && n >= 0) {
	iv[niv].settings_id = NGHTTP2_SETTINGS_HEADER_TABLE_SIZE;
	iv[niv].value = (uint32_t) n;
	niv++;
    }
    if (Ns_ConfigGetBool(path, "h2_enable_push", &n) == NS_TRUE) {
	iv[niv].settings_id = NGHTTP2_SETTINGS_ENABLE_PUSH;
	iv[niv].value = n ? 1u : 0u;
	niv++;
    }
    if (Ns_ConfigGetInt(path, "h2_max_concurrent_streams", &n) == NS_TRUE && n >= 0) {
	iv[niv].settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
	iv[niv].value = (uint32_t) n;
	niv++;
    }
    if (Ns_ConfigGetInt(path, "h2_initial_window_size", &n) == NS_TRUE && n >= 0
	    && n <= 0x7FFFFFFF) {
	iv[niv].settings_id = NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE;
	iv[niv].value = (uint32_t) n;
	niv++;
    }
    if (Ns_ConfigGetInt(path, "h2_max_frame_size", &n) == NS_TRUE) {
	if (n >= 16384 && n <= 16777215) {
	    iv[niv].settings_id = NGHTTP2_SETTINGS_MAX_FRAME_SIZE;
	    iv[niv].value = (uint32_t) n;
	    niv++;
	} else {
	    Ns_Log(Warning,
		    "http2: h2_max_frame_size %d out of range 16384–16777215; ignored",
		    n);
	}
    }
    if (Ns_ConfigGetInt(path, "h2_max_header_list_size", &n) == NS_TRUE && n >= 0) {
	u = (uint32_t) n;
	iv[niv].settings_id = NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE;
	iv[niv].value = u;
	niv++;
    }
    if (niv == 0u) {
	return nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, NULL, 0);
    }
    rv = nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv, niv);
    return rv;
}

static nghttp2_session *H2SessionNew(Sock *sock)
{
    nghttp2_session_callbacks *callbacks;
    nghttp2_session *session;
    nghttp2_option *opt;
    int rv;

    rv = nghttp2_session_callbacks_new(&callbacks);
    if (rv != 0) {
        return NULL;
    }
    nghttp2_session_callbacks_set_send_callback(callbacks, H2SendCb);
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, H2OnBeginHeaders);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, H2OnHeader);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, H2OnDataChunk);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, H2OnFrameRecv);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, H2OnStreamClose);
    nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, H2OnFrameSend);

    rv = nghttp2_option_new(&opt);
    if (rv != 0) {
	nghttp2_session_callbacks_del(callbacks);
	return NULL;
    }
    H2ApplySockModuleOptions(opt, sock);
    rv = nghttp2_session_server_new2(&session, callbacks, sock, opt);
    nghttp2_option_del(opt);
    nghttp2_session_callbacks_del(callbacks);
    if (rv != 0) {
        return NULL;
    }
    rv = H2SubmitPrefaceSettings(session, sock);
    if (rv != 0) {
	nghttp2_session_del(session);
	return NULL;
    }
    H2_DUAL_INC(sock->drvPtr, sessions_created);
    return session;
}

/*
 * Create the nghttp2 server session as soon as ALPN selects h2, before any
 * client HTTP/2 bytes are read. Queues the SETTINGS preface so TLS sends
 * data to the peer immediately; avoids TCP deadlocks with Nagle (client
 * delays small preface until it sees server traffic — Python h2, some h2spec
 * timings) where the driver previously skipped TrySend while http2 was NULL.
 */
void
NsHttp2EnsureSession(Sock *sockPtr)
{
    nghttp2_session *session;

    if (((Ns_Sock *) sockPtr)->app_protocol != NS_APP_PROTO_H2) {
	return;
    }
    Ns_MutexLock(&sockPtr->h2Lock);
    if (sockPtr->http2 == NULL) {
	session = H2SessionNew(sockPtr);
	if (session != NULL) {
	    sockPtr->http2 = session;
	}
    }
    Ns_MutexUnlock(&sockPtr->h2Lock);
}

/*
 * Run mem_recv for a full TLS plaintext chunk. Caller must hold h2Lock; session
 * must be non-NULL. Does not call session_send.
 */
static int
H2MemRecvLocked(Sock *sockPtr, nghttp2_session *session, const unsigned char *data,
		size_t datalen)
{
    const unsigned char *p = data;
    size_t remain = datalen;
    ssize_t readlen;

    (void) sockPtr;
    while (remain > 0) {
	readlen = nghttp2_session_mem_recv(session, p, remain);
	if (readlen < 0) {
	    H2Trace("H2MemRecvLocked err %s remain=%lu",
		    nghttp2_strerror((int) readlen), (unsigned long) remain);
	    return -1;
	}
	if (readlen == 0) {
	    H2Trace("H2MemRecvLocked mem_recv 0 remain=%lu",
		    (unsigned long) remain);
	    return -1;
	}
	p += (size_t) readlen;
	remain -= (size_t) readlen;
    }
    return 0;
}

static int H2SessionSendAll(Sock *sock, nghttp2_session *session)
{
    int rv;

    /*
     * One nghttp2_session_send call runs until WOULDBLOCK or idle.
     * Looping on want_write here spins forever when the TLS layer keeps
     * returning WOULDBLOCK for the same pending frame.
     */
    rv = nghttp2_session_send(session);
    /*
     * Send callback may return NGHTTP2_ERR_WOULDBLOCK when TLS would block;
     * nghttp2_session_send propagates that; it is not a fatal error.
     */
    if (rv == 0 || rv == NGHTTP2_ERR_WOULDBLOCK) {
	return 0;
    }
    H2_DUAL_INC(sock->drvPtr, session_send_fail);
    Ns_Log(Warning, "http2: session_send failed: %s", nghttp2_strerror(rv));
    return -1;
}

/*
 * Run deferred stream dispatch: queue completed requests for the worker
 * threads.  This does not call nghttp2_submit_* — responses are submitted
 * later under h2Lock — so it does not reorder frames relative to data
 * already queued.  Previously we skipped dispatch while
 * nghttp2_session_want_write was true; that could stall forever when the
 * socket was not polled for write again before the peer timed out (h2spec
 * generic 3.x timeouts waiting for response HEADERS).
 *
 * Returns 1 if any stream was dispatched, 0 if the defer queue was empty.
 */
static int
H2DispatchDeferred(Sock *sockPtr)
{
    H2Defer *dq;
    nghttp2_session *session;

    Ns_MutexLock(&sockPtr->h2Lock);
    session = (nghttp2_session *) sockPtr->http2;
    if (session == NULL) {
	Ns_MutexUnlock(&sockPtr->h2Lock);
	return 0;
    }
    dq = H2DeferGrabAll(sockPtr);
    Ns_MutexUnlock(&sockPtr->h2Lock);
    if (dq == NULL) {
	return 0;
    }
    while (dq != NULL) {
	H2Defer *next = dq->next;

	dq->s->defer_pend = 0;
	(void) DispatchH2Stream(dq->s);
	ns_free(dq);
	dq = next;
    }
    return 1;
}

static void
H2WakeDriverIfPending(Sock *sp, nghttp2_session *sess)
{
    if (sess != NULL && nghttp2_session_want_write(sess)) {
	NsDriverTrigger(sp->drvPtr);
    }
}

static void
H2ClearStreamUserData(Sock *sock, int32_t stream_id)
{
    nghttp2_session *sess;

    Ns_MutexLock(&sock->h2Lock);
    sess = (nghttp2_session *) sock->http2;
    if (sess != NULL) {
	nghttp2_session_set_stream_user_data(sess, stream_id, NULL);
	(void) H2SessionSendAll(sock, sess);
	H2WakeDriverIfPending(sock, sess);
    }
    Ns_MutexUnlock(&sock->h2Lock);
}

static void
H2FailStream(Sock *sock, int32_t stream_id, uint32_t err)
{
    nghttp2_session *sess;

    Ns_MutexLock(&sock->h2Lock);
    sess = (nghttp2_session *) sock->http2;
    if (sess != NULL) {
	nghttp2_submit_rst_stream(sess, NGHTTP2_FLAG_NONE, stream_id, err);
	nghttp2_session_set_stream_user_data(sess, stream_id, NULL);
	(void) H2SessionSendAll(sock, sess);
	H2WakeDriverIfPending(sock, sess);
    }
    Ns_MutexUnlock(&sock->h2Lock);
}

int
NsHttp2TrySend(Sock *sockPtr)
{
    nghttp2_session *session;
    int rv;

    for (;;) {
	for (;;) {
	    int want;
	    unsigned char drainBuf[16384];
	    struct iovec iov;

	    Ns_MutexLock(&sockPtr->h2Lock);
	    session = (nghttp2_session *) sockPtr->http2;
	    if (session == NULL) {
		Ns_MutexUnlock(&sockPtr->h2Lock);
		return 0;
	    }
	    rv = H2SessionSendAll(sockPtr, session);
	    want = nghttp2_session_want_write(session);
	    Ns_MutexUnlock(&sockPtr->h2Lock);
	    H2WakeDriverIfPending(sockPtr, session);
	    if (rv != 0) {
		return rv;
	    }
	    if (!want) {
		break;
	    }
	    /*
	     * nghttp2 still has outbound frames but the TLS layer stopped with
	     * WOULDBLOCK (often SSL_write -> SSL_ERROR_WANT_READ). Read more TLS
	     * application data in this same scheduling turn so OpenSSL can flush
	     * SETTINGS ACK / coalesced client frames (h2spec PING, Python h2).
	     */
	    iov.iov_base = (void *) drainBuf;
	    iov.iov_len = sizeof(drainBuf);
	    rv = (*sockPtr->drvPtr->proc)(DriverRecv, (Ns_Sock *) sockPtr, &iov,
		    1);
	    if (rv == NS_DRIVER_RECV_TLS_EOF) {
		return -1;
	    }
	    if (rv <= 0) {
		break;
	    }
	    Ns_MutexLock(&sockPtr->h2Lock);
	    session = (nghttp2_session *) sockPtr->http2;
	    if (session == NULL) {
		Ns_MutexUnlock(&sockPtr->h2Lock);
		return 0;
	    }
	    if (H2MemRecvLocked(sockPtr, session, drainBuf, (size_t) rv) != 0) {
		Ns_MutexUnlock(&sockPtr->h2Lock);
		H2_DUAL_INC(sockPtr->drvPtr, feed_mem_recv_err);
		return -1;
	    }
	    Ns_MutexUnlock(&sockPtr->h2Lock);
	    H2_DUAL_INC(sockPtr->drvPtr, trysend_drain_reads);
	    H2_DUAL_ADD(sockPtr->drvPtr, bytes_fed, (unsigned long long) rv);
	}
	if (H2DispatchDeferred(sockPtr) <= 0) {
	    break;
	}
    }
    return 0;
}

/*
 * Nonzero if an nghttp2 session exists and the library expects more
 * inbound bytes (used by the driver when poll omits POLLIN).
 */
int
NsHttp2WantReadInput(Sock *sockPtr)
{
    nghttp2_session *session;
    int want;

    if (((Ns_Sock *) sockPtr)->app_protocol != NS_APP_PROTO_H2) {
	return 0;
    }
    Ns_MutexLock(&sockPtr->h2Lock);
    session = (nghttp2_session *) sockPtr->http2;
    want = (session != NULL && nghttp2_session_want_read(session) != 0);
    Ns_MutexUnlock(&sockPtr->h2Lock);
    return want ? 1 : 0;
}

int
NsHttp2Feed(Driver *drvPtr, Sock *sockPtr, Conn *connPtr,
            const unsigned char *data, size_t datalen)
{
    nghttp2_session *session;

    (void) drvPtr;
    (void) connPtr;
    if (H2DebugEnv()) {
	fprintf(stderr, "AOLSERVER_H2_DEBUG NsHttp2Feed enter id=%u datalen=%lu\n",
		sockPtr->id, (unsigned long) datalen);
	fflush(stderr);
    }
    Ns_MutexLock(&sockPtr->h2Lock);
    if (sockPtr->http2 == NULL) {
        session = H2SessionNew(sockPtr);
        if (session == NULL) {
            Ns_MutexUnlock(&sockPtr->h2Lock);
	    H2DebugFeedMsg("H2SessionNew failed", sockPtr->id, datalen,
		    H2_E_HINVAL, NULL);
            return H2_E_HINVAL;
        }
        sockPtr->http2 = session;
    } else {
        session = (nghttp2_session *) sockPtr->http2;
    }

    /*
     * mem_recv may consume a prefix of the buffer and return early (e.g.
     * incomplete frame header). The remainder must be fed in the same I/O
     * cycle; dropping it corrupts the session and breaks clients (h2spec).
     *
     * Do not call session_send between mem_recv steps on the same DriverRecv
     * chunk: a WOULDBLOCK or transient send failure after the first frame
     * (e.g. PRIORITY) must not abort the loop or we discard following frames
     * (e.g. PING) while nghttp2 has already processed the first. Flush once
     * after the full chunk is consumed (below).
     */
    H2Trace("NsHttp2Feed enter datalen=%lu", (unsigned long) datalen);
    if (H2MemRecvLocked(sockPtr, session, data, datalen) != 0) {
	Ns_MutexUnlock(&sockPtr->h2Lock);
	H2_DUAL_INC(sockPtr->drvPtr, feed_mem_recv_err);
	H2DebugFeedMsg("mem_recv error", sockPtr->id, datalen, H2_E_HINVAL, NULL);
	return H2_E_HINVAL;
    }
    H2_DUAL_ADD(sockPtr->drvPtr, bytes_fed, (unsigned long long) datalen);
    Ns_MutexUnlock(&sockPtr->h2Lock);
    /*
     * Deferred streams are dispatched from NsHttp2TrySend only after outbound
     * data (e.g. SETTINGS) is flushed so frames are not reordered on the wire.
     */
    if (NsHttp2TrySend(sockPtr) != 0) {
	H2Trace("NsHttp2Feed NsHttp2TrySend failed (non-fatal; trigger driver)");
	H2DebugFeedMsg(
		"NsHttp2TrySend failed — treat as WOULDBLOCK; mem_recv ok",
		sockPtr->id, datalen, H2_E_OK, NULL);
	H2_DUAL_INC(sockPtr->drvPtr, trysend_recoveries);
	NsDriverTrigger(sockPtr->drvPtr);
	H2_DUAL_INC(sockPtr->drvPtr, feed_ok);
	return H2_E_OK;
    }
    /*
     * While ciphertext is already in the TCP buffer, keep feeding nghttp2 in
     * this same turn. Do not loop on want_read alone (empty TCP + WANT_READ
     * breaks the h2spec TLS handshake).
     */
    {
	int di;
	unsigned char more[16384];
	struct iovec iov;
	unsigned char pb;
	ssize_t pr;
	int n;

	for (di = 0; di < 32; di++) {
	    int tlsPending = 0;

	    if (sockPtr->sock == INVALID_SOCKET) {
		break;
	    }
	    if ((sockPtr->drvPtr->opts & NS_DRIVER_SSL) != 0) {
		tlsPending = (*sockPtr->drvPtr->proc)(DriverTlsAppPending,
			(Ns_Sock *) sockPtr, NULL, 0) > 0;
	    }
	    pr = recv(sockPtr->sock, (char *) &pb, 1, MSG_PEEK);
	    if (pr <= 0 && !tlsPending) {
		break;
	    }
	    iov.iov_base = (void *) more;
	    iov.iov_len = sizeof(more);
	    n = (*drvPtr->proc)(DriverRecv, (Ns_Sock *) sockPtr, &iov, 1);
	    if (n == NS_DRIVER_RECV_TLS_EOF) {
		return 1;
	    }
	    if (n <= 0) {
		break;
	    }
	    Ns_MutexLock(&sockPtr->h2Lock);
	    session = (nghttp2_session *) sockPtr->http2;
	    if (session == NULL) {
		Ns_MutexUnlock(&sockPtr->h2Lock);
		return H2_E_HINVAL;
	    }
	    if (H2MemRecvLocked(sockPtr, session, more, (size_t) n) != 0) {
		Ns_MutexUnlock(&sockPtr->h2Lock);
		H2_DUAL_INC(sockPtr->drvPtr, feed_mem_recv_err);
		return H2_E_HINVAL;
	    }
	    H2_DUAL_ADD(sockPtr->drvPtr, bytes_fed, (unsigned long long) n);
	    Ns_MutexUnlock(&sockPtr->h2Lock);
	    if (NsHttp2TrySend(sockPtr) != 0) {
		H2_DUAL_INC(sockPtr->drvPtr, trysend_recoveries);
		NsDriverTrigger(sockPtr->drvPtr);
		H2Trace("NsHttp2Feed drain TrySend failed (non-fatal)");
		break;
	    }
	}
    }
    H2Trace("NsHttp2Feed ok");
    H2DebugFeedMsg("ok", sockPtr->id, datalen, H2_E_OK, NULL);
    H2_DUAL_INC(sockPtr->drvPtr, feed_ok);
    return H2_E_OK;
}

void
NsHttp2SockCleanup(Sock *sockPtr)
{
    Ns_MutexLock(&sockPtr->h2Lock);
    H2DeferAbort(sockPtr);
    if (sockPtr->http2 != NULL) {
	H2_DUAL_INC(sockPtr->drvPtr, sessions_destroyed);
        nghttp2_session_del((nghttp2_session *) sockPtr->http2);
        sockPtr->http2 = NULL;
    }
    Ns_MutexUnlock(&sockPtr->h2Lock);
}

static char *DupLower(const char *s, size_t len)
{
    char *p = ns_malloc(len + 1u);
    size_t i;

    for (i = 0; i < len; ++i) {
        p[i] = (char) tolower((unsigned char) s[i]);
    }
    p[len] = '\0';
    return p;
}

static void FreeNvArray(nghttp2_nv *nva, size_t nnv)
{
    size_t i;

    for (i = 0; i < nnv; ++i) {
        ns_free(nva[i].name);
        ns_free(nva[i].value);
    }
    ns_free(nva);
}

/*
 * Build nghttp2 response header list from :status and outputheaders.
 */
static int BuildResponseNv(Ns_Conn *conn, nghttp2_nv **out, size_t *outn)
{
    Ns_Set *oh = Ns_ConnOutputHeaders(conn);
    int i, sz = Ns_SetSize(oh);
    char statusBuf[16];
    nghttp2_nv *nva = NULL;
    size_t n = 0;
    const char *k, *v;
    nghttp2_nv *tmp;

    sprintf(statusBuf, "%d", Ns_ConnGetStatus(conn));

    tmp = ns_realloc(nva, (n + 1u) * sizeof(nghttp2_nv));
    nva = tmp;
    nva[n].name = (uint8_t *) ns_strdup(":status");
    nva[n].namelen = 7;
    nva[n].value = (uint8_t *) ns_strdup(statusBuf);
    nva[n].valuelen = strlen(statusBuf);
    nva[n].flags = NGHTTP2_NV_FLAG_NONE;
    ++n;

    for (i = 0; i < sz; ++i) {
        k = Ns_SetKey(oh, i);
        v = Ns_SetValue(oh, i);
        if (k == NULL || v == NULL || k[0] == '\0') {
            continue;
        }
        /* Response pseudo-headers must not appear in the field section (RFC 9113). */
        if (k[0] == ':') {
            continue;
        }
        if (strcasecmp(k, "connection") == 0
            || strcasecmp(k, "transfer-encoding") == 0
            || strcasecmp(k, "keep-alive") == 0) {
            continue;
        }
        tmp = ns_realloc(nva, (n + 1u) * sizeof(nghttp2_nv));
        nva = tmp;
        nva[n].name = (uint8_t *) DupLower(k, strlen(k));
        nva[n].namelen = strlen(k);
        nva[n].value = (uint8_t *) ns_strdup(v);
        nva[n].valuelen = strlen(v);
        nva[n].flags = NGHTTP2_NV_FLAG_NONE;
        ++n;
    }
    *out = nva;
    *outn = n;
    return 0;
}

typedef struct {
    struct iovec *bufs;
    int nbufs;
    size_t iov_idx;
    size_t byte_off;
    char *owned; /* response body copy; freed after nghttp2 finishes DATA */
} H2DataSrc;

static ssize_t H2DataRead(nghttp2_session *session, int32_t stream_id, uint8_t *buf,
                          size_t length, uint32_t *data_flags,
                          nghttp2_data_source *source, void *user_data)
{
    H2DataSrc *ds = (H2DataSrc *) source->ptr;
    size_t n, avail;
    const uint8_t *p;

    (void) session;
    (void) stream_id;
    (void) user_data;
    while (ds->iov_idx < (size_t) ds->nbufs) {
        avail = ds->bufs[ds->iov_idx].iov_len - ds->byte_off;
        if (avail == 0u) {
            ++ds->iov_idx;
            ds->byte_off = 0u;
            continue;
        }
        n = length < avail ? length : avail;
        p = (const uint8_t *) ds->bufs[ds->iov_idx].iov_base + ds->byte_off;
        memcpy(buf, p, n);
        ds->byte_off += n;
        if (ds->byte_off >= ds->bufs[ds->iov_idx].iov_len) {
            ++ds->iov_idx;
            ds->byte_off = 0u;
        }
        if (ds->iov_idx >= (size_t) ds->nbufs) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        }
        return (ssize_t) n;
    }
    *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return 0;
}

int
NsHttp2ConnFlushDirect(Ns_Conn *conn, char *buf, int len, int stream)
{
    Conn *connPtr = (Conn *) conn;
    nghttp2_session *session;
    Sock *sock;
    nghttp2_nv *nva = NULL;
    size_t nnv = 0;
    H2DataSrc *ds = NULL;
    nghttp2_data_provider provider;
    int niov = 0, rv;

    if (connPtr->sockPtr == NULL) {
	return NS_ERROR;
    }
    sock = connPtr->sockPtr;

    /*
     * Read sock->http2 only under h2Lock so it cannot race NsHttp2SockCleanup
     * or session creation on another thread.
     */
    Ns_MutexLock(&sock->h2Lock);
    session = (nghttp2_session *) sock->http2;
    if (session == NULL) {
	Ns_MutexUnlock(&sock->h2Lock);
	return NS_ERROR;
    }

    if (!connPtr->http2_response_started) {
        BuildResponseNv(conn, &nva, &nnv);
        if (!(conn->flags & NS_CONN_SKIPBODY) && len > 0) {
            niov = 1;
            ds = (H2DataSrc *) ns_calloc(1, sizeof(H2DataSrc));
            ds->owned = ns_malloc((size_t) len);
            memcpy(ds->owned, buf, (size_t) len);
            ds->bufs = ns_malloc(sizeof(struct iovec));
            ds->bufs[0].iov_base = ds->owned;
            ds->bufs[0].iov_len = (size_t) len;
            ds->nbufs = niov;
            provider.source.ptr = ds;
            provider.read_callback = H2DataRead;
            rv = nghttp2_submit_response(session, connPtr->http2_stream_id,
                                         nva, nnv, &provider);
        } else if (!(conn->flags & NS_CONN_SKIPBODY) && connPtr->obuf.length > 0) {
            niov = 1;
            ds = (H2DataSrc *) ns_calloc(1, sizeof(H2DataSrc));
            ds->owned = ns_malloc((size_t) connPtr->obuf.length);
            memcpy(ds->owned, connPtr->obuf.string,
                   (size_t) connPtr->obuf.length);
            ds->bufs = ns_malloc(sizeof(struct iovec));
            ds->bufs[0].iov_base = ds->owned;
            ds->bufs[0].iov_len = (size_t) connPtr->obuf.length;
            ds->nbufs = niov;
            provider.source.ptr = ds;
            provider.read_callback = H2DataRead;
            rv = nghttp2_submit_response(session, connPtr->http2_stream_id,
                                         nva, nnv, &provider);
        } else {
            rv = nghttp2_submit_response(session, connPtr->http2_stream_id,
                                         nva, nnv, NULL);
        }
        FreeNvArray(nva, nnv);
        if (rv != 0) {
            if (ds != NULL) {
                ns_free(ds->bufs);
                ns_free(ds->owned);
                ns_free(ds);
            }
	    H2WakeDriverIfPending(sock, session);
            Ns_MutexUnlock(&sock->h2Lock);
            return NS_ERROR;
        }
        connPtr->http2_response_started = 1;
    } else if (!(conn->flags & NS_CONN_SKIPBODY) && len > 0) {
        /* Single nghttp2 response per request; streaming uses one merged write. */
	H2WakeDriverIfPending(sock, session);
        Ns_MutexUnlock(&sock->h2Lock);
        return NS_ERROR;
    }

    if (H2SessionSendAll(sock, session) != 0) {
        if (ds != NULL) {
            ns_free(ds->bufs);
            ns_free(ds->owned);
            ns_free(ds);
        }
	H2WakeDriverIfPending(sock, session);
        Ns_MutexUnlock(&sock->h2Lock);
        return NS_ERROR;
    }
    if (ds != NULL) {
        ns_free(ds->bufs);
        ns_free(ds->owned);
        ns_free(ds);
    }
    Tcl_DStringTrunc(&connPtr->obuf, 0);

    H2WakeDriverIfPending(sock, session);
    Ns_MutexUnlock(&sock->h2Lock);
    return NS_OK;
}

int
NsHttp2ConnSend(Conn *connPtr, struct iovec *bufs, int nbufs)
{
    Ns_Conn *conn = (Ns_Conn *) connPtr;
    int towrite = 0, i, rc;
    char *merged;
    size_t off, oblen;

    for (i = 0; i < nbufs; ++i) {
        towrite += (int) bufs[i].iov_len;
    }
    oblen = (size_t) connPtr->obuf.length;
    if (connPtr->http2_response_started) {
        return -1;
    }
    /*
     * Ns_ConnFlushHeaders queues metadata (SENTHDRS) but skips obuf for HTTP/2;
     * Ns_WriteConn(..., 0) must still submit HEADERS (e.g. HEAD / fastpath).
     */
    if (oblen == 0u && towrite == 0) {
	if (conn->flags & NS_CONN_SENTHDRS) {
	    rc = NsHttp2ConnFlushDirect(conn, NULL, 0, 1);
	    return rc == NS_OK ? 0 : -1;
	}
	return 0;
    }
    merged = ns_malloc(oblen + (size_t) towrite);
    off = 0;
    if (oblen > 0u) {
        memcpy(merged + off, connPtr->obuf.string, oblen);
        off += oblen;
    }
    for (i = 0; i < nbufs; ++i) {
        if (bufs[i].iov_len > 0) {
            memcpy(merged + off, bufs[i].iov_base, bufs[i].iov_len);
            off += bufs[i].iov_len;
        }
    }
    Tcl_DStringTrunc(&connPtr->obuf, 0);
    rc = NsHttp2ConnFlushDirect(conn, merged, (int) off, 1);
    ns_free(merged);
    return rc == NS_OK ? (int) off : -1;
}

void
NsHttp2DriverStatsGet(Driver *drvPtr, NsHttp2Stats *outPtr)
{
    if (outPtr == NULL) {
	return;
    }
    if (drvPtr == NULL) {
	memset(outPtr, 0, sizeof(*outPtr));
	return;
    }
#define H2_DRV_LOAD(f) \
    outPtr->f = atomic_load_explicit(&drvPtr->h2.f, memory_order_relaxed)
    H2_DRV_LOAD(feed_ok);
    H2_DRV_LOAD(feed_mem_recv_err);
    H2_DRV_LOAD(trysend_recoveries);
    H2_DRV_LOAD(sessions_created);
    H2_DRV_LOAD(sessions_destroyed);
    H2_DRV_LOAD(streams_dispatched);
    H2_DRV_LOAD(rst_stream_sent);
    H2_DRV_LOAD(session_send_fail);
    H2_DRV_LOAD(bytes_sent);
    H2_DRV_LOAD(ping_recv);
    H2_DRV_LOAD(goaway_recv);
    H2_DRV_LOAD(rst_stream_recv);
    H2_DRV_LOAD(goaway_sent);
    H2_DRV_LOAD(ping_sent);
    H2_DRV_LOAD(ping_ack_sent);
    H2_DRV_LOAD(defer_appends);
    H2_DRV_LOAD(defer_max_depth);
    H2_DRV_LOAD(trysend_drain_reads);
    H2_DRV_LOAD(bytes_fed);
#undef H2_DRV_LOAD
}

void
NsHttp2StatsGet(NsHttp2Stats *outPtr)
{
    if (outPtr == NULL) {
	return;
    }
#define H2_G_LOAD(f) \
    outPtr->f = atomic_load_explicit(&h2_stat_##f, memory_order_relaxed)
    H2_G_LOAD(feed_ok);
    H2_G_LOAD(feed_mem_recv_err);
    H2_G_LOAD(trysend_recoveries);
    H2_G_LOAD(sessions_created);
    H2_G_LOAD(sessions_destroyed);
    H2_G_LOAD(streams_dispatched);
    H2_G_LOAD(rst_stream_sent);
    H2_G_LOAD(session_send_fail);
    H2_G_LOAD(bytes_sent);
    H2_G_LOAD(ping_recv);
    H2_G_LOAD(goaway_recv);
    H2_G_LOAD(rst_stream_recv);
    H2_G_LOAD(goaway_sent);
    H2_G_LOAD(ping_sent);
    H2_G_LOAD(ping_ack_sent);
    H2_G_LOAD(defer_appends);
    H2_G_LOAD(defer_max_depth);
    H2_G_LOAD(trysend_drain_reads);
    H2_G_LOAD(bytes_fed);
#undef H2_G_LOAD
}

void
Ns_Http2DriverStatsSnapshot(void *p, NsHttp2Stats *outPtr)
{
    NsHttp2DriverStatsGet((Driver *) p, outPtr);
}

#else /* !HAVE_NGHTTP2 */

void
NsHttp2StatsGet(NsHttp2Stats *outPtr)
{
    if (outPtr != NULL) {
	memset(outPtr, 0, sizeof(*outPtr));
    }
}

void
NsHttp2EnsureSession(Sock *sockPtr)
{
    (void) sockPtr;
}

int
NsHttp2TrySend(Sock *sockPtr)
{
    (void) sockPtr;
    return 0;
}

int
NsHttp2WantReadInput(Sock *sockPtr)
{
    (void) sockPtr;
    return 0;
}

int
NsHttp2Feed(Driver *drvPtr, Sock *sockPtr, Conn *connPtr,
            const unsigned char *data, size_t datalen)
{
    (void) drvPtr;
    (void) sockPtr;
    (void) connPtr;
    (void) data;
    (void) datalen;
    return 9;
}

void
NsHttp2SockCleanup(Sock *sockPtr)
{
    (void) sockPtr;
}

int
NsHttp2ConnSend(Conn *connPtr, struct iovec *bufs, int nbufs)
{
    (void) connPtr;
    (void) bufs;
    (void) nbufs;
    return -1;
}

int
NsHttp2ConnFlushDirect(Ns_Conn *conn, char *buf, int len, int stream)
{
    (void) conn;
    (void) buf;
    (void) len;
    (void) stream;
    return NS_ERROR;
}

#endif /* HAVE_NGHTTP2 */
