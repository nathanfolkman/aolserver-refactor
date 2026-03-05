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
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>

#define DRIVER_NAME "nsssl"

static Ns_DriverProc SSLProc;

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

    return Ns_DriverInit(server, module, &init);
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
    SSL_CTX *ctx;
    SSL *ssl;
    int n, total;

    switch (cmd) {
    case DriverRecv:
    case DriverSend:
        /*
         * On first I/O, create an SSL connection and perform the handshake.
         */

        if (sock->arg == NULL) {
            ctx = (SSL_CTX *) driver->arg;
            ssl = SSL_new(ctx);
            if (ssl == NULL) {
                Ns_Log(Error, "nsssl: SSL_new failed");
                return -1;
            }
            SSL_set_fd(ssl, (int) sock->sock);
            if (SSL_accept(ssl) <= 0) {
                Ns_Log(Warning, "nsssl: SSL_accept failed");
                SSL_free(ssl);
                return -1;
            }
            sock->arg = ssl;
        }

        ssl = (SSL *) sock->arg;
        total = 0;
        do {
            if (cmd == DriverSend) {
                n = SSL_write(ssl, bufs->iov_base, (int) bufs->iov_len);
            } else {
                n = SSL_read(ssl, bufs->iov_base, (int) bufs->iov_len);
            }
            if (n < 0 && total > 0) {
                n = 0;
            }
            ++bufs;
            total += n;
        } while (n > 0 && --nbufs > 0);
        n = total;
        break;

    case DriverKeep:
        ssl = (SSL *) sock->arg;
        if (ssl != NULL) {
            n = (SSL_shutdown(ssl) >= 0) ? 0 : -1;
        } else {
            n = -1;
        }
        break;

    case DriverClose:
        ssl = (SSL *) sock->arg;
        if (ssl != NULL) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            sock->arg = NULL;
        }
        n = 0;
        break;

    default:
        n = -1;
        break;
    }
    return n;
}
