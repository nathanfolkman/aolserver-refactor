/*
 * http3.c -- HTTP/3 over QUIC (ngtcp2 + nghttp3 + OpenSSL crypto_ossl).
 */

#include "nsd.h"

#if HAVE_NGHTTP3

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <nghttp3/nghttp3.h>
#include <openssl/ssl.h>
#include <openssl/tls1.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define H3_SV_SCIDLEN 18
#define H3_MAX_HANDLERS 128
#define H3_TXBUF 65536

static _Atomic unsigned long long h3_g_packets_recv;
static _Atomic unsigned long long h3_g_packets_sent;
static _Atomic unsigned long long h3_g_bytes_recv_udp;
static _Atomic unsigned long long h3_g_bytes_sent_udp;
static _Atomic unsigned long long h3_g_conn_accepted;
static _Atomic unsigned long long h3_g_conn_closed;
static _Atomic unsigned long long h3_g_handshake_completed;
static _Atomic unsigned long long h3_g_handshake_fail;
static _Atomic unsigned long long h3_g_streams_dispatched;
static _Atomic unsigned long long h3_g_read_pkt_err;
static _Atomic unsigned long long h3_g_send_fail;
static _Atomic unsigned long long h3_g_version_negotiation_sent;
static _Atomic unsigned long long h3_g_defer_appends;
static _Atomic unsigned long long h3_g_defer_max_depth;

#define H3_STAT_ADD(drv, field, n) do { \
    atomic_fetch_add_explicit(&h3_g_##field, (n), memory_order_relaxed); \
    if ((drv) != NULL) { \
        atomic_fetch_add_explicit(&(drv)->h3stats.field, (n), \
	    memory_order_relaxed); \
    } \
} while (0)
#define H3_STAT_INC(drv, field) H3_STAT_ADD(drv, field, 1ULL)

typedef struct H3Stream {
    int64_t stream_id;
    char *method;
    char *path;
    char *scheme;
    char *authority;
    Ns_Set *hdrs;
    Ns_DString body;
    int dispatched;
} H3Stream;

typedef struct H3Defer {
    H3Stream *s;
    struct H3Defer *next;
} H3Defer;

typedef struct NsH3Conn NsH3Conn;

struct NsH3Conn {
    Driver *drv;
    Sock *anchor;
    ngtcp2_conn *conn;
    nghttp3_conn *httpconn;
    ngtcp2_crypto_ossl_ctx *ossl_ctx;
    SSL *ssl;
    ngtcp2_crypto_conn_ref cref;
    ngtcp2_ccerr lasterr;
    /*
     * After NGTCP2_ERR_CALLBACK_FAILURE, h3_write_cc_after_read_pkt_fail uses
     * lasterr (application or lib) instead of deriving from rv.
     */
    int pending_lasterr_cc;
    struct sockaddr_in peer;
    struct sockaddr_in local;
    int in_table;
    ngtcp2_cid dcid_key;
    NsH3Conn *next;
    NsH3Conn *hnext;
    H3Defer *defFirst;
    H3Defer *defLast;
    uint8_t txbuf[H3_TXBUF];
};

typedef struct {
    NsH3Conn *head;
    NsH3Conn *by_dcid[256];
} H3HandlerSet;

static ngtcp2_tstamp h3_now(void);
static ngtcp2_conn *h3_get_conn_cb(ngtcp2_crypto_conn_ref *cref);
static void H3StreamDestroy(H3Stream *s);
static int h3_write_streams(NsH3Conn *hc);
static ngtcp2_ssize h3_write_pkt_cb(ngtcp2_conn *conn, ngtcp2_path *path,
    ngtcp2_pkt_info *pi, uint8_t *dest, size_t destlen,
    ngtcp2_tstamp ts, void *user_data);
static int h3_send_udp(Driver *drv, const ngtcp2_path *path,
    unsigned int ecn, const uint8_t *data, size_t len);
static int h3_setup_httpconn(NsH3Conn *hc);
static int h3_dispatch_stream(NsH3Conn *hc, H3Stream *s);
static void H3DeferAppend(NsH3Conn *hc, H3Stream *s);

/*
 * Must match driver.c enum (SOCK_READWAIT); anchor Sock for QUIC uses this state.
 */
#define H3_SOCK_ANCHOR_STATE 1

#include "http3_body.inc"

void
NsHttp3StatsGet(NsHttp3Stats *outPtr)
{
    if (outPtr == NULL) {
	return;
    }
    outPtr->packets_recv = atomic_load_explicit(&h3_g_packets_recv, memory_order_relaxed);
    outPtr->packets_sent = atomic_load_explicit(&h3_g_packets_sent, memory_order_relaxed);
    outPtr->bytes_recv_udp = atomic_load_explicit(&h3_g_bytes_recv_udp, memory_order_relaxed);
    outPtr->bytes_sent_udp = atomic_load_explicit(&h3_g_bytes_sent_udp, memory_order_relaxed);
    outPtr->conn_accepted = atomic_load_explicit(&h3_g_conn_accepted, memory_order_relaxed);
    outPtr->conn_closed = atomic_load_explicit(&h3_g_conn_closed, memory_order_relaxed);
    outPtr->handshake_completed = atomic_load_explicit(&h3_g_handshake_completed, memory_order_relaxed);
    outPtr->handshake_fail = atomic_load_explicit(&h3_g_handshake_fail, memory_order_relaxed);
    outPtr->streams_dispatched = atomic_load_explicit(&h3_g_streams_dispatched, memory_order_relaxed);
    outPtr->read_pkt_err = atomic_load_explicit(&h3_g_read_pkt_err, memory_order_relaxed);
    outPtr->send_fail = atomic_load_explicit(&h3_g_send_fail, memory_order_relaxed);
    outPtr->version_negotiation_sent = atomic_load_explicit(&h3_g_version_negotiation_sent, memory_order_relaxed);
    outPtr->defer_appends = atomic_load_explicit(&h3_g_defer_appends, memory_order_relaxed);
    outPtr->defer_max_depth = atomic_load_explicit(&h3_g_defer_max_depth, memory_order_relaxed);
}

void
NsHttp3DriverStatsGet(Driver *drvPtr, NsHttp3Stats *outPtr)
{
    if (outPtr == NULL) {
	return;
    }
    if (drvPtr == NULL) {
	memset(outPtr, 0, sizeof(*outPtr));
	return;
    }
#define L(f) outPtr->f = atomic_load_explicit(&drvPtr->h3stats.f, \
	    memory_order_relaxed)
    L(packets_recv); L(packets_sent); L(bytes_recv_udp); L(bytes_sent_udp);
    L(conn_accepted); L(conn_closed); L(handshake_completed); L(handshake_fail);
    L(streams_dispatched); L(read_pkt_err); L(send_fail);
    L(version_negotiation_sent); L(defer_appends); L(defer_max_depth);
#undef L
}

void
Ns_Http3DriverStatsSnapshot(void *p, NsHttp3Stats *outPtr)
{
    NsHttp3DriverStatsGet((Driver *) p, outPtr);
}

int
Ns_Http3AttachDriver(void *driverPtr, const char *nssslConfigPath)
{
    return NsHttp3DriverInit((Driver *) driverPtr, nssslConfigPath);
}

#endif /* HAVE_NGHTTP3 */
