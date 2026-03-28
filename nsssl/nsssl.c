/*
 * The contents of this file are subject to the AOLserver Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://aolserver.com/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is AOLserver Code and related documentation
 * distributed by AOL.
 *
 * The Initial Developer of the Original Code is America Online,
 * Inc. Portions created by AOL are Copyright (C) 1999 America Online,
 * Inc. All Rights Reserved.
 *
 * Alternatively, the contents of this file may be used under the terms
 * of the GNU General Public License (the "GPL"), in which case the
 * provisions of GPL are applicable instead of those above.  If you wish
 * to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the
 * License, indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by the GPL.
 * If you do not delete the provisions above, a recipient may use your
 * version of this file under either the License or the GPL.
 */

/*
 * nsssl.c --
 *
 *      SSL driver for AOLserver using OpenSSL.
 */

#include "ns.h"
#ifdef HAVE_NSCONFIG_H
#include "nsconfig.h"
#endif
#include <errno.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

#define DRIVER_NAME "nsssl"

/*
 * Per-connection TLS state. OpenSSL SSL* is not thread-safe; the reader
 * thread and worker threads may call DriverRecv/DriverSend concurrently
 * on HTTP/2, so all SSL operations must hold lock.
 */
typedef struct NsSslSockArg {
    SSL *ssl;
    Ns_Mutex lock;
    /*
     * Set after NsHttp2ReaderYieldFlush runs post-handshake (once per connection).
     * Covers ALPN=h2 immediately and sniff-based H2 on the next DriverRecv.
     */
    int h2PostTlsHandshakeFlush;
    /*
     * When ALPN is unknown, the HTTP/2 connection preface may span multiple
     * SSL_read buffers; accumulate up to four bytes to classify before
     * defaulting to HTTP/1.1 (h2spec sequential connections).
     */
    unsigned char appPrefSniff[4];
    int appPrefSniffLen;
} NsSslSockArg;

static Ns_Mutex sslInitLock;
static int sslInitLockReady;

/*
 * Resolve NsHttp2ReaderYieldFlush at runtime so nsssl does not depend on the
 * linker's choice of libnsd (same process already loaded libnsd from nsd).
 */
static void
H2PostTlsHandshakeFlush(Ns_Sock *sock)
{
    static void (*fn)(void *);
    static int tried;

    if (!tried) {
	tried = 1;
	fn = (void (*)(void *)) dlsym(RTLD_DEFAULT, "NsHttp2ReaderYieldFlush");
    }
    if (fn != NULL) {
	fn((void *) sock);
    }
}

/*
 * ALPN preference: offer h2 and http/1.1 (length-prefixed protocol ids).
 */
static const unsigned char alpn_prefs[] = {
    2, 'h', '2',
    8, 'h', 't', 't', 'p', '/', '1', '.', '1',
};

static int
SslAlpnSelectCb(SSL *ssl, const unsigned char **out, unsigned char *outlen,
                const unsigned char *in, unsigned int inlen, void *arg)
{
    unsigned char *proto;
    unsigned int plen;

    (void) ssl;
    (void) arg;
    if (SSL_select_next_proto(&proto, &plen, (unsigned char *) alpn_prefs,
                              sizeof(alpn_prefs), (unsigned char *) in, inlen)
        != OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_NOACK;
    }
    *out = proto;
    *outlen = (unsigned char) plen;
    return SSL_TLSEXT_ERR_OK;
}

static Ns_DriverProc SSLProc;
static int SslAcceptNonBlocking(SSL *ssl, SOCKET fd);

/*
 * True if the TCP socket has ciphertext waiting (next TLS record).
 * Combined with SSL_has_pending so one DriverRecv can decrypt multiple
 * records when the HTTP/2 preface spans TLS records (h2spec back-to-back).
 */
static int
SslTcpHasIncoming(SSL *ssl)
{
    int fd = SSL_get_fd(ssl);
    unsigned char byte;

    if (fd < 0) {
	return 0;
    }
    return recv(fd, (char *) &byte, 1, MSG_PEEK) > 0;
}

/*
 * True if buf[0..len-1] matches the RFC 7540 connection preface prefix.
 * After TLS session resumption, ALPN may be unset and the first SSL_read can
 * return fewer than four bytes; we must not treat a partial "PRI" as HTTP/1.1.
 */
static int
H2ConnPrefacePrefix(const unsigned char *buf, size_t len)
{
    static const char pref[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    size_t i;

    if (len > sizeof(pref) - 1u) {
	len = sizeof(pref) - 1u;
    }
    for (i = 0; i < len; i++) {
	if (buf[i] != (unsigned char) pref[i]) {
	    return 0;
	}
    }
    return 1;
}

/*
 * Lazily create the SSL connection (under sslInitLock) and return the
 * per-socket arg wrapper.
 */
static NsSslSockArg *
SslEnsureConn(Ns_Sock *sock, Ns_Driver *driver)
{
    NsSslSockArg *sa;
    SSL_CTX *ctx;
    SSL *ssl;

    sa = sock->arg;
    if (sa != NULL) {
	return sa;
    }
    /*
     * Only hold sslInitLock around SSL_new and attaching sa to sock->arg.
     * Do not run SslAcceptNonBlocking under this lock — it polls indefinitely
     * and would block every other new TLS connection on the process.
     */
    Ns_MutexLock(&sslInitLock);
    sa = sock->arg;
    if (sa != NULL) {
	Ns_MutexUnlock(&sslInitLock);
	return sa;
    }
    ctx = (SSL_CTX *) driver->arg;
    sa = calloc(1, sizeof(NsSslSockArg));
    if (sa == NULL) {
	Ns_MutexUnlock(&sslInitLock);
	return NULL;
    }
    Ns_MutexInit(&sa->lock);
    ssl = SSL_new(ctx);
    if (ssl == NULL) {
	Ns_Log(Error, "nsssl: SSL_new failed");
	Ns_MutexDestroy(&sa->lock);
	free(sa);
	Ns_MutexUnlock(&sslInitLock);
	return NULL;
    }
    SSL_set_fd(ssl, (int) sock->sock);
    Ns_MutexUnlock(&sslInitLock);

    if (!SslAcceptNonBlocking(ssl, sock->sock)) {
	Ns_Log(Warning, "nsssl: SSL_accept failed");
	SSL_free(ssl);
	Ns_MutexDestroy(&sa->lock);
	free(sa);
	return NULL;
    }

    Ns_MutexLock(&sslInitLock);
    if (sock->arg != NULL) {
	/* Another path published first (should not happen for a new sock). */
	Ns_MutexUnlock(&sslInitLock);
	SSL_free(ssl);
	Ns_MutexDestroy(&sa->lock);
	free(sa);
	return sock->arg;
    }
    sa->ssl = ssl;
    sock->arg = sa;
    if (sock->app_protocol == NS_APP_PROTO_UNKNOWN) {
	const unsigned char *p;
	unsigned int pl;

	SSL_get0_alpn_selected(ssl, &p, &pl);
	if (pl == 2u && p != NULL && p[0] == 'h' && p[1] == '2') {
	    sock->app_protocol = NS_APP_PROTO_H2;
	} else if (pl == 8u && p != NULL && memcmp(p, "http/1.1", 8) == 0) {
	    sock->app_protocol = NS_APP_PROTO_HTTP11;
	} else {
	    /*
	     * TLS 1.3 resumption: some stacks (e.g. Go h2spec after a prior case)
	     * leave ALPN empty on the SSL object even though the session
	     * negotiated h2. Without H2, SockReadLine skips NsHttp2EnsureSession
	     * on the first n==0 read and the peer stalls (generic/2/2 then
	     * generic/2/3).
	     */
	    SSL_SESSION *sess = SSL_get0_session(ssl);

	    if (sess != NULL) {
		const unsigned char *sp;
		size_t slen;

		SSL_SESSION_get0_alpn_selected(sess, &sp, &slen);
		if (slen == 2u && sp != NULL && sp[0] == 'h' && sp[1] == '2') {
		    sock->app_protocol = NS_APP_PROTO_H2;
		} else if (slen == 8u && sp != NULL
			&& memcmp(sp, "http/1.1", 8) == 0) {
		    sock->app_protocol = NS_APP_PROTO_HTTP11;
		}
	    }
	}
	/*
	 * If still UNKNOWN, classify using appPrefSniff across SSL_read chunks
	 * in SSLProc (below).
	 */
    }
    Ns_MutexUnlock(&sslInitLock);
    return sa;
}

/*
 * Complete TLS handshake on a non-blocking socket (DriverRecv uses the
 * same fd in non-blocking mode).
 */
static int
SslAcceptNonBlocking(SSL *ssl, SOCKET fd)
{
    int r, err;

    for (;;) {
        r = SSL_accept(ssl);
        if (r > 0) {
            return 1;
        }
        err = SSL_get_error(ssl, r);
        if (err == SSL_ERROR_WANT_READ) {
            struct pollfd p;

            p.fd = fd;
            p.events = POLLIN;
            p.revents = 0;
            if (poll(&p, 1, -1) <= 0) {
                return 0;
            }
        } else if (err == SSL_ERROR_WANT_WRITE) {
            struct pollfd p;

            p.fd = fd;
            p.events = POLLOUT;
            p.revents = 0;
            if (poll(&p, 1, -1) <= 0) {
                return 0;
            }
        } else {
            return 0;
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * NsSSL_ModInit --
 *
 *      Initialize the OpenSSL driver module.
 *
 * Results:
 *      NS_OK if initialized, NS_ERROR otherwise.
 *
 * Side effects:
 *      Creates an SSL_CTX, loads certificates, and registers
 *      the SSL driver with AOLserver.
 *
 *----------------------------------------------------------------------
 */

int
NsSSL_ModInit(char *server, char *module)
{
    Ns_DriverInitData init;
    SSL_CTX *ctx;
    char *path, *cert, *key, *ciphers;
    int verify;

    if (!sslInitLockReady) {
	Ns_MutexInit(&sslInitLock);
	sslInitLockReady = 1;
    }

    path = Ns_ConfigGetPath(server, module, NULL);

    cert = Ns_ConfigGet(path, "certificate");
    if (cert == NULL) {
        Ns_Log(Error, "%s: certificate parameter required", module);
        return NS_ERROR;
    }

    key = Ns_ConfigGet(path, "key");
    if (key == NULL) {
        key = cert;
    }

    /*
     * Initialize OpenSSL.
     */

    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS
                     | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);

    ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == NULL) {
        Ns_Log(Error, "%s: SSL_CTX_new failed", module);
        return NS_ERROR;
    }

    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

    SSL_CTX_set_alpn_select_cb(ctx, SslAlpnSelectCb, NULL);

    if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1) {
        Ns_Log(Error, "%s: failed to load certificate: %s", module, cert);
        SSL_CTX_free(ctx);
        return NS_ERROR;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1) {
        Ns_Log(Error, "%s: failed to load private key: %s", module, key);
        SSL_CTX_free(ctx);
        return NS_ERROR;
    }

    if (SSL_CTX_check_private_key(ctx) != 1) {
        Ns_Log(Error, "%s: private key does not match certificate", module);
        SSL_CTX_free(ctx);
        return NS_ERROR;
    }

    ciphers = Ns_ConfigGet(path, "ciphers");
    if (ciphers != NULL) {
        if (SSL_CTX_set_cipher_list(ctx, ciphers) != 1) {
            Ns_Log(Error, "%s: failed to set cipher list: %s", module, ciphers);
            SSL_CTX_free(ctx);
            return NS_ERROR;
        }
    }

    if (Ns_ConfigGetBool(path, "verify", &verify) && verify) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER
                           | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    }

    Ns_Log(Notice, "%s: initialized OpenSSL with cert %s", module, cert);

    /*
     * Register the driver without the async option so all I/O
     * happens in the connection thread, avoiding any possible
     * blocking in the driver thread due to SSL overhead.
     */

    init.version = NS_DRIVER_VERSION_1;
    init.name = DRIVER_NAME;
    init.proc = SSLProc;
    init.arg = ctx;
    init.opts = NS_DRIVER_SSL;
    init.path = NULL;

    {
	int drc;
	Ns_DString fullds;

	drc = Ns_DriverInit(server, module, &init);
#if HAVE_NGHTTP3
	if (drc == NS_OK) {
	    void *drv;

	    Ns_DStringInit(&fullds);
	    Ns_DStringVarAppend(&fullds, server, "/", module, NULL);
	    drv = Ns_DriverFindByFullName(fullds.string);
	    Ns_DStringFree(&fullds);
	    if (drv == NULL) {
		Ns_Log(Error, "%s: Ns_DriverFindByFullName failed for HTTP/3",
		    module);
		return NS_ERROR;
	    }
	    if (Ns_Http3AttachDriver(drv, path) != NS_OK) {
		return NS_ERROR;
	    }
	}
#endif
	return drc;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * SSLProc --
 *
 *      SSL driver callback. Handles TLS handshake, read, write,
 *      and close operations using OpenSSL.
 *
 * Results:
 *      For close, always 0. For keep, 0 if connection could be
 *      properly shut down, -1 otherwise. For send and recv, number
 *      of bytes processed or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
SSLProc(Ns_DriverCmd cmd, Ns_Sock *sock, struct iovec *bufs, int nbufs)
{
    Ns_Driver *driver = sock->driver;
    NsSslSockArg *sa;
    SSL *ssl;
    int n, total;

    switch (cmd) {
    case DriverRecv:
    case DriverSend:
	sa = SslEnsureConn(sock, driver);
	if (sa == NULL) {
	    return -1;
	}
	if (cmd == DriverRecv && sock->app_protocol == NS_APP_PROTO_H2
		&& !sa->h2PostTlsHandshakeFlush) {
	    H2PostTlsHandshakeFlush(sock);
	    sa->h2PostTlsHandshakeFlush = 1;
	}
	ssl = sa->ssl;
	Ns_MutexLock(&sa->lock);
	total = 0;
	{
	    struct iovec *firstiov = bufs;
	    int nbufs_in = nbufs;

	do {
	    if (cmd == DriverSend) {
		n = SSL_write(ssl, bufs->iov_base, (int) bufs->iov_len);
	    } else {
		/*
		 * Never ask SSL_read for more plaintext than OpenSSL has already
		 * decrypted. With a huge iovec (AOLserver may use ~1MiB), OpenSSL
		 * 3.3 on Darwin can return SSL_ERROR_SYSCALL / errno 0 while
		 * SSL_pending() and SSL_peek() still show data; clamping the read
		 * size to SSL_pending() breaks that stall (h2spec generic/2/1).
		 */
		{
		    int want = (int) bufs->iov_len;
		    int pend = SSL_pending(ssl);

		    if (cmd == DriverRecv && pend > 0 && want > pend) {
			want = pend;
		    }
		    n = SSL_read(ssl, bufs->iov_base, want);
		}
	    }
	    if (n > 0) {
		total += n;
		/*
		 * Drain decrypted application data already in the SSL read BIO.
		 * Without this, poll() may stay idle (no TCP bytes) while OpenSSL
		 * still holds a complete follow-on record; HTTP/2 clients then
		 * stall or RST (h2spec generic/2, PRIORITY + PING after SETTINGS).
		 */
		if (cmd == DriverRecv && nbufs == 1) {
		    char *base = bufs->iov_base;
		    int cap = (int) bufs->iov_len;

		    while (total < cap
			    && (SSL_has_pending(ssl) != 0
				    || SslTcpHasIncoming(ssl) != 0)) {
			int room = cap - total;
			int pend = SSL_pending(ssl);

			if (pend > 0 && room > pend) {
			    room = pend;
			}
			n = SSL_read(ssl, base + total, room);
			if (n > 0) {
			    total += n;
			    continue;
			}
			{
			    int sslErr = SSL_get_error(ssl, n);

			    if (sslErr == SSL_ERROR_WANT_READ
				    || sslErr == SSL_ERROR_WANT_WRITE) {
				break;
			    }
			    if (n == 0 && sslErr == SSL_ERROR_ZERO_RETURN) {
				break;
			    }
			    if (n == 0 && sslErr == SSL_ERROR_SYSCALL
				    && errno == 0) {
				break;
			    }
			    total = -1;
			    break;
			}
		    }
		}
	    } else {
		int sslErr = SSL_get_error(ssl, n);

		if (sslErr == SSL_ERROR_WANT_READ
			|| sslErr == SSL_ERROR_WANT_WRITE) {
		    break;
		}
		/*
		 * SSL_read() returns 0 for end-of-stream (close_notify). Do not
		 * lump that with hard errors; driver maps this to E_CLOSE.
		 */
		if (cmd == DriverRecv && n == 0
			&& sslErr == SSL_ERROR_ZERO_RETURN) {
		    total = NS_DRIVER_RECV_TLS_EOF;
		    break;
		}
		/*
		 * TCP FIN/RST without TLS close_notify is often reported as
		 * SSL_ERROR_SYSCALL with errno 0. On Darwin, the same signature
		 * has been observed while OpenSSL still reports pending read data
		 * (SSL_has_pending / SSL_pending). Mapping that to EOF closes the
		 * connection before the HTTP/2 preface reaches nghttp2; return 0
		 * bytes from this recv instead (same as WANT_READ) and let the next
		 * poll/read retry.
		 */
		if (cmd == DriverRecv && n == 0
			&& sslErr == SSL_ERROR_SYSCALL && errno == 0) {
		    if (SSL_has_pending(ssl) || SSL_pending(ssl) > 0) {
			break;
		    }
		    /*
		     * More TLS records may be waiting in the TCP buffer while
		     * OpenSSL reports SYSCALL/0 (Darwin). Avoid false EOF so the
		     * driver polls again (h2spec PING after SETTINGS, etc.).
		     */
		    {
			int fd = SSL_get_fd(ssl);
			unsigned char byte;

			if (fd >= 0) {
			    ssize_t pr = recv(fd, (char *) &byte, 1, MSG_PEEK);

			    if (pr > 0) {
				break;
			    }
			    if (pr == 0) {
				total = NS_DRIVER_RECV_TLS_EOF;
				break;
			    }
			    /* pr < 0: EINTR/EAGAIN/etc. — not a confirmed TCP close */
			    break;
			}
		    }
		    total = NS_DRIVER_RECV_TLS_EOF;
		    break;
		}
		if (n < 0 && total > 0 && cmd == DriverSend) {
		    break;
		}
		total = -1;
		break;
	    }
	    ++bufs;
	} while (n > 0 && --nbufs > 0);
	n = total;
	if (cmd == DriverRecv && nbufs_in == 1 && n > 0
		&& sock->app_protocol != NS_APP_PROTO_H2) {
	    unsigned char *b = (unsigned char *) firstiov->iov_base;

	    /*
	     * RFC 7540 connection preface begins with "PRI * HTTP/2.0\r\n".
	     * When ALPN was not available after handshake (session resume),
	     * the preface may span multiple SSL_read buffers; accumulate up to
	     * four bytes in appPrefSniff before defaulting to HTTP/1.1.
	     */
	    if (sock->app_protocol == NS_APP_PROTO_UNKNOWN) {
		size_t bi;

		for (bi = 0; bi < (size_t) n && sa->appPrefSniffLen < 4; bi++) {
		    sa->appPrefSniff[sa->appPrefSniffLen++] = b[bi];
		}
		if (sa->appPrefSniffLen >= 4) {
		    if (sa->appPrefSniff[0] == 'P' && sa->appPrefSniff[1] == 'R'
			    && sa->appPrefSniff[2] == 'I'
			    && sa->appPrefSniff[3] == ' ') {
			sock->app_protocol = NS_APP_PROTO_H2;
		    } else {
			sock->app_protocol = NS_APP_PROTO_HTTP11;
		    }
		} else if (sa->appPrefSniffLen > 0
			&& !H2ConnPrefacePrefix(sa->appPrefSniff,
				(size_t) sa->appPrefSniffLen)) {
		    sock->app_protocol = NS_APP_PROTO_HTTP11;
		}
	    } else if ((size_t) n >= 4u && b[0] == 'P' && b[1] == 'R'
		    && b[2] == 'I' && b[3] == ' ') {
		sock->app_protocol = NS_APP_PROTO_H2;
	    }
	}
	}
	Ns_MutexUnlock(&sa->lock);
	break;

    case DriverKeep:
	sa = sock->arg;
	if (sa != NULL) {
	    Ns_MutexLock(&sa->lock);
	    ssl = sa->ssl;
	    if (ssl != NULL && sock->app_protocol != NS_APP_PROTO_H2) {
		n = (SSL_shutdown(ssl) >= 0) ? 0 : -1;
	    } else if (ssl != NULL) {
		n = 0;
	    } else {
		n = -1;
	    }
	    Ns_MutexUnlock(&sa->lock);
	} else {
	    n = -1;
	}
	break;

    case DriverClose:
	sa = sock->arg;
	if (sa != NULL) {
	    Ns_MutexLock(&sa->lock);
	    ssl = sa->ssl;
	    if (ssl != NULL) {
		SSL_shutdown(ssl);
		SSL_free(ssl);
		sa->ssl = NULL;
	    }
	    Ns_MutexUnlock(&sa->lock);
	    Ns_MutexDestroy(&sa->lock);
	    free(sa);
	    sock->arg = NULL;
	}
	sock->app_protocol = NS_APP_PROTO_UNKNOWN;
	n = 0;
	break;

    case DriverTlsAppPending:
	sa = sock->arg;
	n = 0;
	if (sa != NULL) {
	    Ns_MutexLock(&sa->lock);
	    ssl = sa->ssl;
	    if (ssl != NULL
		    && (SSL_pending(ssl) > 0 || SSL_has_pending(ssl))) {
		n = 1;
	    }
	    Ns_MutexUnlock(&sa->lock);
	}
	break;

    case DriverTlsWantWrite:
	sa = sock->arg;
	n = 0;
	if (sa != NULL) {
	    Ns_MutexLock(&sa->lock);
	    ssl = sa->ssl;
	    if (ssl != NULL) {
		if (SSL_want_write(ssl) != 0) {
		    n = 1;
		} else {
		    BIO *wbio = SSL_get_wbio(ssl);

		    if (wbio != NULL
			    && BIO_ctrl(wbio, BIO_CTRL_WPENDING, 0, NULL) > 0) {
			n = 1;
		    }
		}
	    }
	    Ns_MutexUnlock(&sa->lock);
	}
	break;

    default:
	n = -1;
	break;
    }
    return n;
}
