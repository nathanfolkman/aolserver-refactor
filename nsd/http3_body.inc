/* HTTP/3 QUIC engine (included from http3.c). */

static void h3_make_path(Driver *drv, const struct sockaddr_in *rem,
    ngtcp2_path *path);

static ngtcp2_tstamp
h3_now(void)
{
    Ns_Time t;

    Ns_GetTime(&t);
    return (ngtcp2_tstamp) t.sec * NGTCP2_SECONDS
	+ (ngtcp2_tstamp) t.usec * 1000u;
}

static ngtcp2_conn *
h3_get_conn_cb(ngtcp2_crypto_conn_ref *cref)
{
    return ((NsH3Conn *) cref->user_data)->conn;
}

static unsigned
h3_dcid_hash(const ngtcp2_cid *cid)
{
    size_t i;
    unsigned h = 0u;

    for (i = 0; i < cid->datalen && i < 8u; i++) {
	h = h * 31u + cid->data[i];
    }
    return h % 256u;
}

static void
h3_handlers_add(H3HandlerSet *hs, NsH3Conn *hc)
{
    unsigned b = h3_dcid_hash(&hc->dcid_key);

    hc->hnext = hs->by_dcid[b];
    hs->by_dcid[b] = hc;
    hc->next = hs->head;
    hs->head = hc;
    hc->in_table = 1;
}

static void
h3_handlers_remove(H3HandlerSet *hs, NsH3Conn *hc)
{
    unsigned b;
    NsH3Conn **pp;

    if (!hc->in_table) {
	return;
    }
    b = h3_dcid_hash(&hc->dcid_key);
    pp = &hs->by_dcid[b];
    while (*pp != NULL) {
	if (*pp == hc) {
	    *pp = hc->hnext;
	    break;
	}
	pp = &(*pp)->hnext;
    }
    pp = &hs->head;
    while (*pp != NULL) {
	if (*pp == hc) {
	    *pp = hc->next;
	    break;
	}
	pp = &(*pp)->next;
    }
    hc->in_table = 0;
    hc->hnext = NULL;
    hc->next = NULL;
}

/*
 * True if |wire_dcid| matches any local SCID we have offered the peer (not
 * retired). Peers put one of these in the DCID field of packets to us; after
 * NEW_CONNECTION_ID / rotation it may differ from hc->dcid_key.
 */
static int
h3_conn_matches_local_scid(const NsH3Conn *c, const ngtcp2_cid *wire_dcid)
{
    ngtcp2_cid stack_cids[16];
    ngtcp2_cid *scids = stack_cids;
    size_t n, i;
    int found = 0;

    if (c->conn == NULL || wire_dcid == NULL || wire_dcid->datalen == 0u) {
	return 0;
    }
    n = ngtcp2_conn_get_scid(c->conn, NULL);
    if (n == 0u) {
	return 0;
    }
    if (n > (sizeof(stack_cids) / sizeof(stack_cids[0]))) {
	scids = (ngtcp2_cid *) ns_malloc(sizeof(ngtcp2_cid) * n);
	if (scids == NULL) {
	    return 0;
	}
    }
    (void) ngtcp2_conn_get_scid(c->conn, scids);
    for (i = 0u; i < n; i++) {
	if (ngtcp2_cid_eq(wire_dcid, &scids[i])) {
	    found = 1;
	    break;
	}
    }
    if (scids != stack_cids) {
	ns_free(scids);
    }
    return found;
}

static NsH3Conn *
h3_lookup(H3HandlerSet *hs, const ngtcp2_cid *dcid)
{
    NsH3Conn *c;

    /*
     * Walk all handlers: hash buckets are keyed only by the first SCID
     * (dcid_key); a packet may carry a later issued local CID.
     */
    for (c = hs->head; c != NULL; c = c->next) {
	if (h3_conn_matches_local_scid(c, dcid)) {
	    return c;
	}
    }
    return NULL;
}

/*
 * Short-header packets do not encode DCID length. Try each active local SCID
 * length against the bytes following the first octet (see ngtcp2_conn_get_scid).
 */
static NsH3Conn *
h3_lookup_short_dcid(H3HandlerSet *hs, const uint8_t *buf, size_t nread)
{
    NsH3Conn *c;
    ngtcp2_cid stack_cids[16];
    ngtcp2_cid *scids = stack_cids;
    size_t n, i, j;
    ngtcp2_cid pktcid;

    for (c = hs->head; c != NULL; c = c->next) {
	if (c->conn == NULL) {
	    continue;
	}
	n = ngtcp2_conn_get_scid(c->conn, NULL);
	if (n == 0u) {
	    continue;
	}
	if (n > (sizeof(stack_cids) / sizeof(stack_cids[0]))) {
	    scids = (ngtcp2_cid *) ns_malloc(sizeof(ngtcp2_cid) * n);
	    if (scids == NULL) {
		continue;
	    }
	} else {
	    scids = stack_cids;
	}
	(void) ngtcp2_conn_get_scid(c->conn, scids);
	for (j = 0u; j < n; j++) {
	    size_t dclen = scids[j].datalen;

	    if (dclen == 0u || dclen > NGTCP2_MAX_CIDLEN
		    || nread < 1u + dclen) {
		continue;
	    }
	    ngtcp2_cid_init(&pktcid, buf + 1, dclen);
	    if (ngtcp2_cid_eq(&pktcid, &scids[j])) {
		if (scids != stack_cids) {
		    ns_free(scids);
		}
		return c;
	    }
	}
	if (scids != stack_cids) {
	    ns_free(scids);
	    scids = stack_cids;
	}
    }
    return NULL;
}

static void
H3StreamDestroy(H3Stream *s)
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

static void
h3_make_path(Driver *drv, const struct sockaddr_in *rem, ngtcp2_path *path)
{
    ngtcp2_addr_init(&path->local,
	(const ngtcp2_sockaddr *) &drv->quic.localAddr,
	sizeof(drv->quic.localAddr));
    ngtcp2_addr_init(&path->remote, (const ngtcp2_sockaddr *) rem,
	sizeof(*rem));
    path->user_data = drv;
}

static int
h3_alpn_select(SSL *ssl, const unsigned char **out, unsigned char *outlen,
    const unsigned char *in, unsigned int inlen, void *arg)
{
    static const unsigned char alpn[] = { 2, 'h', '3' };
    unsigned char *p;
    unsigned int plen;

    (void) ssl;
    (void) arg;
    /*
     * TLS 1.3 / QUIC: use ALERT_FATAL so OpenSSL emits no_application_protocol
     * (RFC 7301). NOACK is documented for classic TLS but is unreliable with
     * QUIC stack + h3spec's cryptoErrorsIn [NoApplicationProtocol] check.
     */
    if (SSL_select_next_proto(&p, &plen, alpn, sizeof(alpn),
	    (unsigned char *) in, inlen) != OPENSSL_NPN_NEGOTIATED) {
	return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    *out = p;
    *outlen = (unsigned char) plen;
    return SSL_TLSEXT_ERR_OK;
}

/*
 * TLS 8.2 / RFC 9001: ClientHello must include quic_transport_parameters.
 * Without this, OpenSSL may advance the handshake until the peer times out;
 * abort early with missing_extension (alert 109) so QUIC carries a crypto CC.
 */
static int
h3_quic_require_transport_params_cb(SSL *ssl, int *al, void *arg)
{
    int *exts = NULL;
    size_t i, n;

    (void) arg;
    if (!SSL_client_hello_get1_extensions_present(ssl, &exts, &n)) {
	*al = TLS13_AD_MISSING_EXTENSION;
	return SSL_CLIENT_HELLO_ERROR;
    }
    for (i = 0; i < n; ++i) {
	if ((unsigned int) exts[i] == TLSEXT_TYPE_quic_transport_parameters) {
	    OPENSSL_free(exts);
	    return SSL_CLIENT_HELLO_SUCCESS;
	}
    }
    OPENSSL_free(exts);
    *al = TLS13_AD_MISSING_EXTENSION;
    return SSL_CLIENT_HELLO_ERROR;
}

static void
h3_rand_ngtcp2(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx *rand_ctx)
{
    (void) rand_ctx;
    if (RAND_bytes(dest, (int) destlen) != 1) {
	abort();
    }
}

static void
h3_rand_nghttp3(uint8_t *dest, size_t destlen)
{
    if (RAND_bytes(dest, (int) destlen) != 1) {
	abort();
    }
}

static int
h3_extend_max_stream_data(ngtcp2_conn *conn, int64_t stream_id,
    uint64_t max_data, void *user_data, void *stream_user_data)
{
    NsH3Conn *hc = user_data;
    (void) conn;
    (void) max_data;
    (void) stream_user_data;
    if (hc->httpconn != NULL
	    && nghttp3_conn_unblock_stream(hc->httpconn, stream_id) != 0) {
	return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

static int
h3_extend_max_remote_streams_bidi(ngtcp2_conn *conn, uint64_t max_streams,
    void *user_data)
{
    NsH3Conn *hc = user_data;
    (void) conn;
    if (hc->httpconn != NULL) {
	nghttp3_conn_set_max_client_streams_bidi(hc->httpconn, max_streams);
    }
    return 0;
}

static int
h3_recv_crypto_data(ngtcp2_conn *conn, ngtcp2_encryption_level encryption_level,
    uint64_t offset, const uint8_t *data, size_t datalen, void *user_data)
{
    return ngtcp2_crypto_recv_crypto_data_cb(conn, encryption_level, offset,
	data, datalen, user_data);
}

static int
h3_handshake_completed_cb(ngtcp2_conn *conn, void *user_data)
{
    NsH3Conn *hc = user_data;
    (void) conn;
    H3_STAT_INC(hc->drv, handshake_completed);
    return 0;
}

static int
h3_recv_stream_data(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id,
    uint64_t offset, const uint8_t *data, size_t datalen,
    void *user_data, void *stream_user_data)
{
    NsH3Conn *hc = user_data;
    nghttp3_ssize nconsumed;
    int fin = (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0;
    (void) conn;
    (void) offset;
    (void) stream_user_data;
    if (hc->httpconn == NULL) {
	/*
	 * OpenSSL QUIC may install the 1-RTT RX key (recv_rx_key) before the
	 * TX key (recv_tx_key). Application data must not be swallowed with
	 * success while httpconn is still NULL.
	 */
	if (datalen == 0u) {
	    return 0;
	}
	if (h3_setup_httpconn(hc) != 0) {
	    ngtcp2_ccerr_default(&hc->lasterr);
	    ngtcp2_ccerr_set_liberr(&hc->lasterr, NGTCP2_ERR_INTERNAL, NULL, 0);
	    hc->pending_lasterr_cc = 1;
	    return NGTCP2_ERR_CALLBACK_FAILURE;
	}
    }
    nconsumed = nghttp3_conn_read_stream2(hc->httpconn, stream_id, data,
	datalen, fin, ngtcp2_conn_get_timestamp(hc->conn));
    if (nconsumed < 0) {
	ngtcp2_ccerr_default(&hc->lasterr);
	ngtcp2_ccerr_set_application_error(&hc->lasterr,
	    nghttp3_err_infer_quic_app_error_code((int) nconsumed), NULL, 0);
	hc->pending_lasterr_cc = 1;
	return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    ngtcp2_conn_extend_max_stream_offset(hc->conn, stream_id,
	(uint64_t) nconsumed);
    ngtcp2_conn_extend_max_offset(hc->conn, (uint64_t) nconsumed);
    return 0;
}

static int
h3_stream_open(ngtcp2_conn *conn, int64_t stream_id, void *user_data)
{
    (void) conn;
    (void) stream_id;
    (void) user_data;
    return 0;
}

static int
h3_stream_close(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id,
    uint64_t app_error_code, void *user_data, void *stream_user_data)
{
    NsH3Conn *hc = user_data;
    (void) conn;
    (void) stream_user_data;
    if (!(flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET)) {
	app_error_code = NGHTTP3_H3_NO_ERROR;
    }
    if (hc->httpconn != NULL) {
	(void) nghttp3_conn_close_stream(hc->httpconn, stream_id,
	    app_error_code);
    }
    if (ngtcp2_is_bidi_stream(stream_id)
	    && !ngtcp2_conn_is_local_stream(hc->conn, stream_id)) {
	ngtcp2_conn_extend_max_streams_bidi(hc->conn, 1);
    }
    return 0;
}

static int
h3_stream_reset(ngtcp2_conn *conn, int64_t stream_id, uint64_t final_size,
    uint64_t app_error_code, void *user_data, void *stream_user_data)
{
    NsH3Conn *hc = user_data;
    (void) conn;
    (void) final_size;
    (void) app_error_code;
    (void) stream_user_data;
    if (hc->httpconn != NULL) {
	int rv = nghttp3_conn_shutdown_stream_read(hc->httpconn, stream_id);

	if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
	    return NGTCP2_ERR_CALLBACK_FAILURE;
	}
    }
    return 0;
}

static int
h3_stream_stop_sending(ngtcp2_conn *conn, int64_t stream_id,
    uint64_t app_error_code, void *user_data, void *stream_user_data)
{
    NsH3Conn *hc = user_data;
    (void) conn;
    (void) app_error_code;
    (void) stream_user_data;
    if (hc->httpconn != NULL) {
	int rv = nghttp3_conn_shutdown_stream_read(hc->httpconn, stream_id);

	if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
	    return NGTCP2_ERR_CALLBACK_FAILURE;
	}
    }
    return 0;
}

static int
h3_update_key(ngtcp2_conn *conn, uint8_t *rx_secret, uint8_t *tx_secret,
    ngtcp2_crypto_aead_ctx *rx_aead_ctx, uint8_t *rx_iv,
    ngtcp2_crypto_aead_ctx *tx_aead_ctx, uint8_t *tx_iv,
    const uint8_t *current_rx_secret, const uint8_t *current_tx_secret,
    size_t secretlen, void *user_data)
{
    const ngtcp2_crypto_ctx *cctx;
    const ngtcp2_crypto_aead *aead;
    size_t keylen, ivlen;
    uint8_t rx_key[64], tx_key[64];
    (void) user_data;
    cctx = ngtcp2_conn_get_crypto_ctx(conn);
    aead = &cctx->aead;
    keylen = ngtcp2_crypto_aead_keylen(aead);
    ivlen = ngtcp2_crypto_packet_protection_ivlen(aead);
    (void) keylen;
    (void) ivlen;
    if (ngtcp2_crypto_update_key(conn, rx_secret, tx_secret, rx_aead_ctx,
	    rx_key, rx_iv, tx_aead_ctx, tx_key, tx_iv, current_rx_secret,
	    current_tx_secret, secretlen) != 0) {
	return -1;
    }
    return 0;
}

static int
h3_path_validation(ngtcp2_conn *conn, uint32_t flags, const ngtcp2_path *path,
    const ngtcp2_path *fallback_path, ngtcp2_path_validation_result res,
    void *user_data)
{
    (void) conn;
    (void) flags;
    (void) path;
    (void) fallback_path;
    (void) res;
    (void) user_data;
    return 0;
}

static int
h3_get_new_connection_id(ngtcp2_conn *conn, ngtcp2_cid *cid, uint8_t *token,
    size_t cidlen, void *user_data)
{
    NsH3Conn *hc = user_data;
    (void) conn;
    if (RAND_bytes(cid->data, (int) cidlen) != 1) {
	return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    cid->datalen = cidlen;
    if (ngtcp2_crypto_generate_stateless_reset_token(token,
	    hc->drv->quic.staticSecret, sizeof(hc->drv->quic.staticSecret),
	    cid) != 0) {
	return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

static int
h3_remove_connection_id(ngtcp2_conn *conn, const ngtcp2_cid *cid,
    void *user_data)
{
    (void) conn;
    (void) cid;
    (void) user_data;
    return 0;
}

static int
h3_recv_rx_key(ngtcp2_conn *conn, ngtcp2_encryption_level level,
    void *user_data)
{
    NsH3Conn *hc = user_data;
    (void) conn;
    if (level != NGTCP2_ENCRYPTION_LEVEL_1RTT) {
	return 0;
    }
    return h3_setup_httpconn(hc) == 0 ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
}

static int
h3_recv_tx_key(ngtcp2_conn *conn, ngtcp2_encryption_level level,
    void *user_data)
{
    NsH3Conn *hc = user_data;
    (void) conn;
    if (level != NGTCP2_ENCRYPTION_LEVEL_1RTT) {
	return 0;
    }
    return h3_setup_httpconn(hc) == 0 ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
}

static int
h3_acked_stream_data_offset(ngtcp2_conn *conn, int64_t stream_id,
    uint64_t offset, uint64_t datalen, void *user_data,
    void *stream_user_data)
{
    NsH3Conn *hc = user_data;
    (void) conn;
    (void) offset;
    (void) stream_user_data;
    if (hc->httpconn != NULL
	    && nghttp3_conn_add_ack_offset(hc->httpconn, stream_id, datalen)
	    != 0) {
	return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

static int
h3_http_recv_data(nghttp3_conn *conn, int64_t stream_id, const uint8_t *data,
    size_t datalen, void *conn_user_data, void *stream_user_data)
{
    H3Stream *s = stream_user_data;
    (void) conn;
    (void) stream_id;
    (void) conn_user_data;
    if (s == NULL) {
	return 0;
    }
    Ns_DStringNAppend(&s->body, (const char *) data, (int) datalen);
    return 0;
}

static int
h3_http_deferred_consume(nghttp3_conn *conn, int64_t stream_id,
    size_t nconsumed, void *conn_user_data, void *stream_user_data)
{
    NsH3Conn *hc = conn_user_data;
    (void) conn;
    (void) stream_user_data;
    ngtcp2_conn_extend_max_stream_offset(hc->conn, stream_id, nconsumed);
    ngtcp2_conn_extend_max_offset(hc->conn, nconsumed);
    return 0;
}

static int
h3_http_begin_headers(nghttp3_conn *conn, int64_t stream_id,
    void *conn_user_data, void *stream_user_data)
{
    H3Stream *s;
    (void) conn_user_data;
    (void) stream_user_data;
    s = (H3Stream *) ns_calloc(1, sizeof(H3Stream));
    s->stream_id = stream_id;
    s->hdrs = Ns_SetCreate(NULL);
    nghttp3_conn_set_stream_user_data(conn, stream_id, s);
    return 0;
}

static int
h3_http_recv_header(nghttp3_conn *conn, int64_t stream_id, int32_t token,
    nghttp3_rcbuf *name, nghttp3_rcbuf *value, uint8_t flags,
    void *conn_user_data, void *stream_user_data)
{
    H3Stream *s = stream_user_data;
    nghttp3_vec vn, vv;
    char *nbuf, *vbuf;
    (void) conn;
    (void) stream_id;
    (void) conn_user_data;
    (void) flags;
    if (s == NULL) {
	return 0;
    }
    vn = nghttp3_rcbuf_get_buf(name);
    vv = nghttp3_rcbuf_get_buf(value);
    switch (token) {
    case NGHTTP3_QPACK_TOKEN__PATH:
	ns_free(s->path);
	s->path = ns_malloc(vv.len + 1u);
	memcpy(s->path, vv.base, vv.len);
	s->path[vv.len] = '\0';
	break;
    case NGHTTP3_QPACK_TOKEN__METHOD:
	ns_free(s->method);
	s->method = ns_malloc(vv.len + 1u);
	memcpy(s->method, vv.base, vv.len);
	s->method[vv.len] = '\0';
	break;
    case NGHTTP3_QPACK_TOKEN__AUTHORITY:
	ns_free(s->authority);
	s->authority = ns_malloc(vv.len + 1u);
	memcpy(s->authority, vv.base, vv.len);
	s->authority[vv.len] = '\0';
	break;
    case NGHTTP3_QPACK_TOKEN__SCHEME:
	ns_free(s->scheme);
	s->scheme = ns_malloc(vv.len + 1u);
	memcpy(s->scheme, vv.base, vv.len);
	s->scheme[vv.len] = '\0';
	break;
    default:
	nbuf = ns_malloc(vn.len + 1u);
	memcpy(nbuf, vn.base, vn.len);
	nbuf[vn.len] = '\0';
	vbuf = ns_malloc(vv.len + 1u);
	memcpy(vbuf, vv.base, vv.len);
	vbuf[vv.len] = '\0';
	Ns_SetPut(s->hdrs, nbuf, vbuf);
	ns_free(nbuf);
	ns_free(vbuf);
	break;
    }
    return 0;
}

static int
h3_http_end_headers(nghttp3_conn *conn, int64_t stream_id, int fin,
    void *conn_user_data, void *stream_user_data)
{
    (void) conn;
    (void) stream_id;
    (void) fin;
    (void) conn_user_data;
    (void) stream_user_data;
    return 0;
}

static int
h3_http_stop_sending(nghttp3_conn *conn, int64_t stream_id,
    uint64_t app_error_code, void *conn_user_data, void *stream_user_data)
{
    NsH3Conn *hc = conn_user_data;
    (void) conn;
    (void) stream_user_data;
    if (ngtcp2_conn_shutdown_stream_read(hc->conn, 0, stream_id,
	    app_error_code) != 0) {
	return NGHTTP3_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

static void
H3DeferAppend(NsH3Conn *hc, H3Stream *s);

static int
h3_http_end_stream(nghttp3_conn *conn, int64_t stream_id,
    void *conn_user_data, void *stream_user_data)
{
    NsH3Conn *hc = conn_user_data;
    H3Stream *s = stream_user_data;
    (void) conn;
    (void) stream_id;
    if (s != NULL && !s->dispatched) {
	H3DeferAppend(hc, s);
    }
    return 0;
}

static int
h3_http_reset_stream(nghttp3_conn *conn, int64_t stream_id,
    uint64_t app_error_code, void *conn_user_data, void *stream_user_data)
{
    NsH3Conn *hc = conn_user_data;
    (void) conn;
    (void) stream_user_data;
    if (ngtcp2_conn_shutdown_stream_write(hc->conn, 0, stream_id,
	    app_error_code) != 0) {
	return NGHTTP3_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

static int
h3_http_stream_close(nghttp3_conn *conn, int64_t stream_id,
    uint64_t app_error_code, void *conn_user_data, void *stream_user_data)
{
    H3Stream *s = stream_user_data;
    (void) stream_id;
    (void) app_error_code;
    (void) conn_user_data;
    if (s != NULL) {
	nghttp3_conn_set_stream_user_data(conn, stream_id, NULL);
	H3StreamDestroy(s);
    }
    return 0;
}

static int
h3_http_acked_stream_data(nghttp3_conn *conn, int64_t stream_id,
    uint64_t datalen, void *conn_user_data, void *stream_user_data)
{
    Conn *connPtr = stream_user_data;
    (void) conn;
    (void) stream_id;
    (void) datalen;
    (void) conn_user_data;
    if (connPtr != NULL && connPtr->h3_write_buf != NULL) {
	connPtr->h3_write_off += datalen;
	if (connPtr->h3_write_off >= connPtr->h3_write_len) {
	    ns_free(connPtr->h3_write_buf);
	    connPtr->h3_write_buf = NULL;
	    connPtr->h3_write_len = 0u;
	    connPtr->h3_write_off = 0u;
	}
    }
    return 0;
}

static int
h3_http_recv_settings(nghttp3_conn *conn, const nghttp3_settings *settings,
    void *conn_user_data)
{
    (void) conn;
    (void) settings;
    (void) conn_user_data;
    return 0;
}

static int
h3_setup_httpconn(NsH3Conn *hc)
{
    nghttp3_callbacks callbacks = { 0 };
    nghttp3_settings settings;
    const ngtcp2_transport_params *params;
    int64_t ctrl_sid, qe_sid, qd_sid;
    int rv;

    if (hc->httpconn != NULL) {
	return 0;
    }
    if (ngtcp2_conn_get_streams_uni_left(hc->conn) < 3) {
	return -1;
    }
    callbacks.acked_stream_data = h3_http_acked_stream_data;
    callbacks.stream_close = h3_http_stream_close;
    callbacks.recv_data = h3_http_recv_data;
    callbacks.deferred_consume = h3_http_deferred_consume;
    callbacks.begin_headers = h3_http_begin_headers;
    callbacks.recv_header = h3_http_recv_header;
    callbacks.end_headers = h3_http_end_headers;
    callbacks.stop_sending = h3_http_stop_sending;
    callbacks.end_stream = h3_http_end_stream;
    callbacks.reset_stream = h3_http_reset_stream;
    callbacks.recv_settings = h3_http_recv_settings;
    callbacks.rand = h3_rand_nghttp3;
    nghttp3_settings_default(&settings);
    settings.qpack_max_dtable_capacity = hc->drv->quic.qpackMaxDtable;
    settings.qpack_blocked_streams = hc->drv->quic.qpackBlockedStreams;
    rv = nghttp3_conn_server_new(&hc->httpconn, &callbacks, &settings,
	nghttp3_mem_default(), hc);
    if (rv != 0) {
	return -1;
    }
    params = ngtcp2_conn_get_local_transport_params(hc->conn);
    nghttp3_conn_set_max_client_streams_bidi(hc->httpconn,
	params->initial_max_streams_bidi);
    if (ngtcp2_conn_open_uni_stream(hc->conn, &ctrl_sid, NULL) != 0) {
	return -1;
    }
    if (nghttp3_conn_bind_control_stream(hc->httpconn, ctrl_sid) != 0) {
	return -1;
    }
    if (ngtcp2_conn_open_uni_stream(hc->conn, &qe_sid, NULL) != 0) {
	return -1;
    }
    if (ngtcp2_conn_open_uni_stream(hc->conn, &qd_sid, NULL) != 0) {
	return -1;
    }
    if (nghttp3_conn_bind_qpack_streams(hc->httpconn, qe_sid, qd_sid) != 0) {
	return -1;
    }
    return 0;
}

static void
H3DeferAppend(NsH3Conn *hc, H3Stream *s)
{
    H3Defer *d = (H3Defer *) ns_malloc(sizeof(H3Defer));
    H3Defer *p;
    unsigned long long depth = 0u;

    d->s = s;
    d->next = NULL;
    if (hc->defLast == NULL) {
	hc->defFirst = hc->defLast = d;
    } else {
	hc->defLast->next = d;
	hc->defLast = d;
    }
    for (p = hc->defFirst; p != NULL; p = p->next) {
	depth++;
    }
    H3_STAT_INC(hc->drv, defer_appends);
    {
	_Atomic unsigned long long *a = &h3_g_defer_max_depth;
	unsigned long long cur;

	do {
	    cur = atomic_load_explicit(a, memory_order_relaxed);
	    if (depth <= cur) {
		break;
	    }
	} while (!atomic_compare_exchange_weak_explicit(a, &cur, depth,
		memory_order_relaxed, memory_order_relaxed));
    }
    if (hc->drv != NULL) {
	_Atomic unsigned long long *a2 = &hc->drv->h3stats.defer_max_depth;
	unsigned long long cur2;

	do {
	    cur2 = atomic_load_explicit(a2, memory_order_relaxed);
	    if (depth <= cur2) {
		break;
	    }
	} while (!atomic_compare_exchange_weak_explicit(a2, &cur2, depth,
		memory_order_relaxed, memory_order_relaxed));
    }
}

static H3Defer *
H3DeferGrabAll(NsH3Conn *hc)
{
    H3Defer *h = hc->defFirst;

    hc->defFirst = hc->defLast = NULL;
    return h;
}

static int
h3_dispatch_stream(NsH3Conn *hc, H3Stream *s)
{
    Ns_DString line;
    Ns_Request *req;
    Conn *connPtr;
    Ns_Time now;
    int i, hl;
    char *clHeader;
    int contentLen = 0;

    if (s->dispatched || s->method == NULL || s->path == NULL) {
	return 0;
    }
    s->dispatched = 1;
    Ns_DStringInit(&line);
    Ns_DStringPrintf(&line, "%s %s HTTP/3.0", s->method, s->path);
    Ns_GetTime(&now);
    connPtr = NsDriverAllocConn(hc->drv, &now, hc->anchor);
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
	connPtr->sockPtr = NULL;
	NsFreeConn(connPtr);
	H3StreamDestroy(s);
	return 0;
    }
    connPtr->request = req;
    connPtr->major = 3u;
    connPtr->minor = 0u;
    connPtr->limitsPtr = NsGetRequestLimits(connPtr->server, req->method,
	req->url);
    clHeader = Ns_SetIGet(connPtr->headers, "content-length");
    if (clHeader != NULL) {
	if (sscanf(clHeader, "%d", &contentLen) != 1 || contentLen < 0) {
	    goto failConn;
	}
    } else {
	contentLen = (int) Ns_DStringLength(&s->body);
    }
    if (contentLen > (int) connPtr->limitsPtr->maxupload) {
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
	connPtr->flags |= NS_CONN_H3_BODY;
    } else {
	connPtr->content = connPtr->next = (char *) "";
	connPtr->avail = 0;
    }
    connPtr->flags |= (NS_CONN_READHDRS | NS_CONN_HTTP3 | NS_CONN_KEEPALIVE);
    connPtr->http2_session = NULL;
    connPtr->h3_handler = hc;
    connPtr->h3_stream_id = s->stream_id;
    connPtr->h3_response_started = 0;
    connPtr->h3_write_buf = NULL;
    connPtr->h3_write_len = 0u;
    connPtr->h3_write_off = 0u;
    hl = Ns_SetSize(connPtr->headers);
    for (i = 0; i < hl; ++i) {
	if (connPtr->servPtr->opts.hdrcase == ToLower) {
	    Ns_StrToLower(Ns_SetKey(connPtr->headers, i));
	} else if (connPtr->servPtr->opts.hdrcase == ToUpper) {
	    Ns_StrToUpper(Ns_SetKey(connPtr->headers, i));
	}
    }
    if (NsRunFilters((Ns_Conn *) connPtr, NS_FILTER_PRE_QUEUE) != NS_OK) {
	goto failConn;
    }
    nghttp3_conn_set_stream_user_data(hc->httpconn, s->stream_id, NULL);
    H3StreamDestroy(s);
    H3_STAT_INC(hc->drv, streams_dispatched);
    NsDriverH3PendingAppend(hc->drv, connPtr);
    return 0;

failEarly:
    Ns_DStringFree(&line);
    connPtr->sockPtr = NULL;
    NsFreeConn(connPtr);
    H3StreamDestroy(s);
    return 0;

failConn:
    connPtr->sockPtr = NULL;
    NsFreeConn(connPtr);
    H3StreamDestroy(s);
    return 0;
}

static int
h3_send_udp(Driver *drv, const ngtcp2_path *path, unsigned int ecn,
    const uint8_t *data, size_t len)
{
    ssize_t n;
    (void) ecn;
    n = sendto(drv->quic.udpSock, (const void *) data, len, 0,
	(const struct sockaddr *) path->remote.addr,
	(socklen_t) path->remote.addrlen);
    if (n < 0) {
	H3_STAT_INC(drv, send_fail);
	return -1;
    }
    H3_STAT_INC(drv, packets_sent);
    H3_STAT_ADD(drv, bytes_sent_udp, (unsigned long long) n);
    return 0;
}

/*
 * After ngtcp2_conn_read_pkt fails, build and send a CONNECTION_CLOSE per
 * ngtcp2 API notes. Skips codes that forbid or replace this path (silent
 * drop, draining/closing, Retry).
 */
static int
h3_write_cc_after_read_pkt_fail(NsH3Conn *hc, ngtcp2_path *path, int rv)
{
    ngtcp2_ccerr ccerr;
    const ngtcp2_ccerr *ccerr_ptr;
    ngtcp2_pkt_info cc_pi;
    ngtcp2_ssize nwrite;
    ngtcp2_tstamp ts;

    if (rv == NGTCP2_ERR_DROP_CONN || rv == NGTCP2_ERR_DRAINING
	    || rv == NGTCP2_ERR_CLOSING || rv == NGTCP2_ERR_RETRY) {
	return 0;
    }
    ts = h3_now();
    if (rv == NGTCP2_ERR_CALLBACK_FAILURE
	    && (hc->pending_lasterr_cc
		|| hc->lasterr.type == NGTCP2_CCERR_TYPE_APPLICATION)) {
	if (hc->pending_lasterr_cc) {
	    hc->pending_lasterr_cc = 0;
	}
	ccerr_ptr = &hc->lasterr;
    } else {
	ngtcp2_ccerr_default(&ccerr);
	if (rv == NGTCP2_ERR_CRYPTO) {
	    ngtcp2_ccerr_set_tls_alert(&ccerr,
		ngtcp2_conn_get_tls_alert(hc->conn), NULL, 0);
	} else {
	    ngtcp2_ccerr_set_liberr(&ccerr, rv, NULL, 0);
	}
	ccerr_ptr = &ccerr;
    }
    memset(&cc_pi, 0, sizeof(cc_pi));
    {
	const ngtcp2_path *cpath = ngtcp2_conn_get_path(hc->conn);
	ngtcp2_path *wpath = path;

	if (cpath != NULL) {
	    wpath = (ngtcp2_path *) cpath;
	}
	nwrite = ngtcp2_conn_write_connection_close(hc->conn, wpath, &cc_pi,
	    hc->txbuf, sizeof(hc->txbuf), ccerr_ptr, ts);
	if (ccerr_ptr == &hc->lasterr) {
	    ngtcp2_ccerr_default(&hc->lasterr);
	}
	if (nwrite < 0) {
	    return -1;
	}
	if (nwrite == 0) {
	    return 0;
	}
	ngtcp2_conn_update_pkt_tx_time(hc->conn, ts);
	return h3_send_udp(hc->drv, wpath, cc_pi.ecn, hc->txbuf, (size_t) nwrite);
    }
}

static ngtcp2_ssize
h3_write_pkt_cb(ngtcp2_conn *conn, ngtcp2_path *path, ngtcp2_pkt_info *pi,
    uint8_t *dest, size_t destlen, ngtcp2_tstamp ts, void *user_data)
{
    NsH3Conn *hc = user_data;
    nghttp3_vec vec[16];
    (void) conn;
    for (;;) {
	int64_t stream_id = -1;
	int fin = 0;
	nghttp3_ssize sveccnt = 0;
	ngtcp2_ssize nwrite;
	int64_t ndatalen;
	uint32_t wflags;

	if (hc->httpconn != NULL && ngtcp2_conn_get_max_data_left(hc->conn)) {
	    sveccnt = nghttp3_conn_writev_stream(hc->httpconn, &stream_id,
		&fin, vec, (sizeof(vec) / sizeof(vec[0])));
	    if (sveccnt < 0) {
		return NGTCP2_ERR_CALLBACK_FAILURE;
	    }
	}
	wflags = NGTCP2_WRITE_STREAM_FLAG_MORE | NGTCP2_WRITE_STREAM_FLAG_PADDING;
	if (fin) {
	    wflags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
	}
	nwrite = ngtcp2_conn_writev_stream(hc->conn, path, pi, dest, destlen,
	    &ndatalen, wflags, stream_id, (const ngtcp2_vec *) vec,
	    (size_t) sveccnt, ts);
	if (nwrite < 0) {
	    if (nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
		nghttp3_conn_block_stream(hc->httpconn, stream_id);
		continue;
	    }
	    if (nwrite == NGTCP2_ERR_STREAM_SHUT_WR) {
		nghttp3_conn_shutdown_stream_write(hc->httpconn, stream_id);
		continue;
	    }
	    if (nwrite == NGTCP2_ERR_WRITE_MORE) {
		if (ndatalen >= 0 && nghttp3_conn_add_write_offset(hc->httpconn,
			stream_id, (size_t) ndatalen) != 0) {
		    return NGTCP2_ERR_CALLBACK_FAILURE;
		}
		continue;
	    }
	    return nwrite;
	}
	if (ndatalen >= 0) {
	    if (nghttp3_conn_add_write_offset(hc->httpconn, stream_id,
		    (size_t) ndatalen) != 0) {
		return NGTCP2_ERR_CALLBACK_FAILURE;
	    }
	}
	return nwrite;
    }
}

static int
h3_write_streams(NsH3Conn *hc)
{
    ngtcp2_path_storage ps;
    ngtcp2_pkt_info pi;
    ngtcp2_ssize nwrite;
    ngtcp2_tstamp ts = h3_now();
    size_t gso = 0u;

    if (ngtcp2_conn_in_closing_period(hc->conn)
	    || ngtcp2_conn_in_draining_period(hc->conn)) {
	return 0;
    }
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_addr_init(&ps.path.local,
	(const ngtcp2_sockaddr *) &hc->drv->quic.localAddr,
	sizeof(hc->drv->quic.localAddr));
    ngtcp2_addr_init(&ps.path.remote, (const ngtcp2_sockaddr *) &hc->peer,
	sizeof(hc->peer));
    ps.path.user_data = hc->drv;
    memset(&pi, 0, sizeof(pi));
    nwrite = ngtcp2_conn_write_aggregate_pkt2(hc->conn, &ps.path, &pi,
	hc->txbuf, sizeof(hc->txbuf), &gso, h3_write_pkt_cb, 64, ts);
    if (nwrite < 0) {
	return -1;
    }
    ngtcp2_conn_update_pkt_tx_time(hc->conn, ts);
    if (nwrite == 0) {
	return 0;
    }
    return h3_send_udp(hc->drv, &ps.path, pi.ecn, hc->txbuf, (size_t) nwrite);
}

static void
h3_conn_free(NsH3Conn *hc)
{
    Driver *drv = hc->drv;

    if (drv != NULL && drv->quic.handlers != NULL) {
	h3_handlers_remove((H3HandlerSet *) drv->quic.handlers, hc);
    }
    if (hc->httpconn != NULL) {
	nghttp3_conn_del(hc->httpconn);
	hc->httpconn = NULL;
    }
    if (hc->conn != NULL) {
	ngtcp2_conn_del(hc->conn);
	hc->conn = NULL;
    }
    if (hc->ssl != NULL) {
	SSL_set_app_data(hc->ssl, NULL);
	SSL_free(hc->ssl);
	hc->ssl = NULL;
    }
    if (hc->ossl_ctx != NULL) {
	ngtcp2_crypto_ossl_ctx_del(hc->ossl_ctx);
	hc->ossl_ctx = NULL;
    }
    if (hc->anchor != NULL) {
	NsSockClose(hc->anchor, 0);
	hc->anchor = NULL;
    }
    ns_free(hc);
    if (drv != NULL) {
	H3_STAT_INC(drv, conn_closed);
    }
}

/*
 * Shared handling for ngtcp2_conn_read_pkt / handle_expiry failure paths.
 */
static void
h3_handle_ngtcp2_fail(NsH3Conn *hc, ngtcp2_path *path, int rv)
{
    Driver *drv = hc->drv;

    if (rv == 0) {
	return;
    }
    H3_STAT_INC(drv, read_pkt_err);
    if (rv == NGTCP2_ERR_DROP_CONN || rv == NGTCP2_ERR_IDLE_CLOSE) {
	h3_conn_free(hc);
	return;
    }
    (void) h3_write_cc_after_read_pkt_fail(hc, path, rv);
    (void) h3_write_streams(hc);
}

static int
h3_conn_init(NsH3Conn *hc, Driver *drv, const ngtcp2_pkt_hd *hd,
    const struct sockaddr_in *local, const struct sockaddr_in *peer,
    SSL_CTX *ssl_ctx)
{
    ngtcp2_callbacks callbacks = { 0 };
    ngtcp2_settings settings;
    ngtcp2_transport_params params;
    ngtcp2_path path;
    ngtcp2_cid scid;
    int rv;

    memset(hc, 0, sizeof(*hc));
    hc->drv = drv;
    hc->peer = *peer;
    scid.datalen = H3_SV_SCIDLEN;
    if (RAND_bytes(scid.data, (int) scid.datalen) != 1) {
	return -1;
    }
    /*
     * Route incoming datagrams by the DCID clients send to us — that is
     * our chosen source CID (scid), not hd->dcid from their first Initial.
     */
    hc->dcid_key = scid;
    callbacks.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
    callbacks.recv_crypto_data = h3_recv_crypto_data;
    callbacks.handshake_completed = h3_handshake_completed_cb;
    callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
    callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
    callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
    callbacks.recv_stream_data = h3_recv_stream_data;
    callbacks.acked_stream_data_offset = h3_acked_stream_data_offset;
    callbacks.stream_open = h3_stream_open;
    callbacks.stream_close = h3_stream_close;
    callbacks.rand = h3_rand_ngtcp2;
    callbacks.get_new_connection_id = h3_get_new_connection_id;
    callbacks.remove_connection_id = h3_remove_connection_id;
    callbacks.update_key = h3_update_key;
    callbacks.path_validation = h3_path_validation;
    callbacks.stream_reset = h3_stream_reset;
    callbacks.extend_max_remote_streams_bidi =
	h3_extend_max_remote_streams_bidi;
    callbacks.extend_max_stream_data = h3_extend_max_stream_data;
    callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    callbacks.delete_crypto_cipher_ctx =
	ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    callbacks.get_path_challenge_data =
	ngtcp2_crypto_get_path_challenge_data_cb;
    callbacks.stream_stop_sending = h3_stream_stop_sending;
    callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    callbacks.recv_rx_key = h3_recv_rx_key;
    callbacks.recv_tx_key = h3_recv_tx_key;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = h3_now();
    if (drv->quic.maxTxUdpPayload > 0u) {
	settings.max_tx_udp_payload_size = drv->quic.maxTxUdpPayload;
	settings.no_tx_udp_payload_size_shaping = 1;
    }
    ngtcp2_transport_params_default(&params);
    params.initial_max_stream_data_bidi_local =
	drv->quic.maxStreamDataBidiLocal;
    params.initial_max_stream_data_bidi_remote =
	drv->quic.maxStreamDataBidiRemote;
    params.initial_max_stream_data_uni = drv->quic.maxStreamDataUni;
    params.initial_max_data = drv->quic.maxData;
    params.initial_max_streams_bidi = drv->quic.maxStreamsBidi;
    params.initial_max_streams_uni = drv->quic.maxStreamsUni;
    params.max_idle_timeout = drv->quic.maxIdleTimeout;
    params.stateless_reset_token_present = 1;
    params.active_connection_id_limit = 7;
    params.grease_quic_bit = 1;
    /* RFC 9000 / ngtcp2: first Initial's Destination CID field. */
    params.original_dcid = hd->dcid;
    params.original_dcid_present = 1;
    if (ngtcp2_crypto_generate_stateless_reset_token(
	    params.stateless_reset_token, drv->quic.staticSecret,
	    sizeof(drv->quic.staticSecret), &scid) != 0) {
	return -1;
    }
    ngtcp2_addr_init(&path.local, (const ngtcp2_sockaddr *) local,
	sizeof(*local));
    ngtcp2_addr_init(&path.remote, (const ngtcp2_sockaddr *) peer,
	sizeof(*peer));
    path.user_data = drv;
    /*
     * ngtcp2_conn_server_new first CID is the client's Initial *source*
     * (hd->scid), despite the parameter name "dcid". original_dcid in
     * transport params remains hd->dcid per RFC 9000.
     */
    rv = ngtcp2_conn_server_new(&hc->conn, &hd->scid, &scid, &path,
	hd->version, &callbacks, &settings, &params, NULL, hc);
    if (rv != 0) {
	return -1;
    }
    hc->cref.get_conn = h3_get_conn_cb;
    hc->cref.user_data = hc;
    if (ngtcp2_crypto_ossl_ctx_new(&hc->ossl_ctx, NULL) != 0) {
	return -1;
    }
    hc->ssl = SSL_new(ssl_ctx);
    if (hc->ssl == NULL) {
	return -1;
    }
    ngtcp2_crypto_ossl_ctx_set_ssl(hc->ossl_ctx, hc->ssl);
    if (ngtcp2_crypto_ossl_configure_server_session(hc->ssl) != 0) {
	return -1;
    }
    SSL_set_app_data(hc->ssl, &hc->cref);
    SSL_set_accept_state(hc->ssl);
    ngtcp2_conn_set_tls_native_handle(hc->conn, hc->ossl_ctx);
    return 0;
}

void
NsHttp3DrainDeferred(Driver *drv)
{
    H3HandlerSet *hs;
    NsH3Conn *hc;

    if (drv == NULL || drv->quic.handlers == NULL) {
	return;
    }
    hs = (H3HandlerSet *) drv->quic.handlers;
    for (hc = hs->head; hc != NULL; hc = hc->next) {
	H3Defer *d, *dnext;

	for (d = H3DeferGrabAll(hc); d != NULL; d = dnext) {
	    dnext = d->next;
	    if (d->s != NULL) {
		h3_dispatch_stream(hc, d->s);
	    }
	    ns_free(d);
	}
	(void) h3_write_streams(hc);
    }
}

/*
 * Earliest ngtcp2 timer as an absolute Ns_Time for the driver poll (same clock
 * base as h3_now / Ns_GetTime).
 */
int
NsHttp3GetNextDeadline(Driver *drv, Ns_Time *outAbs)
{
    H3HandlerSet *hs;
    NsH3Conn *hc;
    ngtcp2_tstamp minexp, exp;

    if (drv == NULL || !drv->quic.enabled || drv->quic.handlers == NULL
	    || outAbs == NULL) {
	return 0;
    }
    hs = (H3HandlerSet *) drv->quic.handlers;
    minexp = UINT64_MAX;
    for (hc = hs->head; hc != NULL; hc = hc->next) {
	exp = ngtcp2_conn_get_expiry(hc->conn);
	if (exp < minexp) {
	    minexp = exp;
	}
    }
    if (minexp == UINT64_MAX) {
	return 0;
    }
    outAbs->sec = (time_t) (minexp / NGTCP2_SECONDS);
    outAbs->usec = (long) ((minexp % NGTCP2_SECONDS) / 1000u);
    return 1;
}

/*
 * Run QUIC timers and flush outgoing packets so the stack can send without a
 * new incoming UDP datagram (avoids wedging the driver in NsPoll forever).
 */
void
NsHttp3ProcessTimers(Driver *drv)
{
    H3HandlerSet *hs;
    NsH3Conn *hc, *hnext;
    ngtcp2_path path;
    ngtcp2_tstamp ts;
    int rv_e;

    if (drv == NULL || !drv->quic.enabled || drv->quic.handlers == NULL) {
	return;
    }
    hs = (H3HandlerSet *) drv->quic.handlers;
    ts = h3_now();
    for (hc = hs->head; hc != NULL; hc = hnext) {
	hnext = hc->next;
	h3_make_path(drv, &hc->peer, &path);
	rv_e = ngtcp2_conn_handle_expiry(hc->conn, ts);
	if (rv_e != 0) {
	    h3_handle_ngtcp2_fail(hc, &path, rv_e);
	} else {
	    (void) h3_write_streams(hc);
	}
    }
}

void
NsHttp3ServiceUdp(Driver *drv)
{
    uint8_t buf[65536];
    struct sockaddr_in remote;
    socklen_t rlen;
    ssize_t nread;
    ngtcp2_version_cid vc;
    ngtcp2_pkt_hd hd;
    H3HandlerSet *hs;
    ngtcp2_pkt_info pi;
    ngtcp2_path path;

    if (drv == NULL || drv->quic.udpSock == INVALID_SOCKET
	    || !drv->quic.enabled) {
	return;
    }
    hs = (H3HandlerSet *) drv->quic.handlers;
    if (hs == NULL) {
	return;
    }
    for (size_t udpDrain = 0u; udpDrain < 256u; udpDrain++) {
	rlen = sizeof(remote);
	nread = recvfrom(drv->quic.udpSock, (char *) buf, sizeof(buf), 0,
	    (struct sockaddr *) &remote, &rlen);
	if (nread < 0) {
	    if (ns_sockerrno == EWOULDBLOCK || ns_sockerrno == EAGAIN) {
		break;
	    }
	    break;
	}
	if (nread < 21) {
	    break;
	}
	H3_STAT_INC(drv, packets_recv);
	H3_STAT_ADD(drv, bytes_recv_udp, (unsigned long long) nread);
	memset(&pi, 0, sizeof(pi));
	h3_make_path(drv, &remote, &path);
	{
	    NsH3Conn *hc = NULL;
	    ngtcp2_cid lookup_cid;

	    /*
	     * Long header: decode_version_cid yields DCID + length from the wire.
	     * Short header: DCID length is implicit; match any active local SCID
	     * (ngtcp2_conn_get_scid), not a single fixed length.
	     */
	    if ((buf[0] & 0x80u) != 0u) {
		switch (ngtcp2_pkt_decode_version_cid(&vc, buf, (size_t) nread,
			H3_SV_SCIDLEN)) {
		case 0:
		    break;
		case NGTCP2_ERR_VERSION_NEGOTIATION:
		    H3_STAT_INC(drv, version_negotiation_sent);
		    continue;
		default:
		    H3_STAT_INC(drv, read_pkt_err);
		    continue;
		}
		if (vc.dcidlen > NGTCP2_MAX_CIDLEN) {
		    H3_STAT_INC(drv, read_pkt_err);
		    continue;
		}
		ngtcp2_cid_init(&lookup_cid, vc.dcid, vc.dcidlen);
		hc = h3_lookup(hs, &lookup_cid);
	    } else {
		hc = h3_lookup_short_dcid(hs, buf, (size_t) nread);
	    }

	    if (hc != NULL) {
		int rv, rv_e;
		ngtcp2_tstamp now = h3_now();

		rv = ngtcp2_conn_read_pkt(hc->conn, &path, &pi, buf,
		    (size_t) nread, now);
		rv_e = ngtcp2_conn_handle_expiry(hc->conn, h3_now());
		if (rv != 0) {
		    h3_handle_ngtcp2_fail(hc, &path, rv);
		} else if (rv_e != 0) {
		    h3_handle_ngtcp2_fail(hc, &path, rv_e);
		} else {
		    (void) h3_write_streams(hc);
		}
		continue;
	    }
	}
	if (ngtcp2_accept(&hd, buf, (size_t) nread) != 0) {
	    H3_STAT_INC(drv, read_pkt_err);
	    continue;
	}
	if (hd.type != NGTCP2_PKT_INITIAL) {
	    continue;
	}
	{
	    NsH3Conn *hc = (NsH3Conn *) ns_calloc(1, sizeof(NsH3Conn));
	    Sock *sp;

	    if (h3_conn_init(hc, drv, &hd, &drv->quic.localAddr, &remote,
		    (SSL_CTX *) drv->quic.sslCtx) != 0) {
		ns_free(hc);
		H3_STAT_INC(drv, handshake_fail);
		continue;
	    }
	    sp = drv->freeSockPtr;
	    if (sp == NULL) {
		h3_conn_free(hc);
		continue;
	    }
	    drv->freeSockPtr = sp->nextPtr;
	    sp->nextPtr = NULL;
	    sp->sock = INVALID_SOCKET;
	    sp->drvPtr = drv;
	    sp->id = drv->nextid++;
	    sp->app_protocol = NS_APP_PROTO_H3;
	    sp->arg = NULL;
	    sp->connPtr = NULL;
	    sp->http2 = NULL;
	    sp->h2NeedDriverPoll = 0;
	    sp->h2DeferFirst = sp->h2DeferLast = NULL;
	    sp->state = H3_SOCK_ANCHOR_STATE;
	    sp->sa = remote;
	    ++drv->nactive;
	    hc->anchor = sp;
	    {
		int rv, rv_e;
		ngtcp2_tstamp now = h3_now();

		rv = ngtcp2_conn_read_pkt(hc->conn, &path, &pi, buf,
		    (size_t) nread, now);
		rv_e = ngtcp2_conn_handle_expiry(hc->conn, h3_now());
		if (rv != 0) {
		    if (rv != NGTCP2_ERR_DROP_CONN
			    && rv != NGTCP2_ERR_IDLE_CLOSE) {
			(void) h3_write_cc_after_read_pkt_fail(hc, &path, rv);
		    }
		    h3_conn_free(hc);
		    continue;
		}
		if (rv_e != 0) {
		    if (rv_e != NGTCP2_ERR_DROP_CONN
			    && rv_e != NGTCP2_ERR_IDLE_CLOSE) {
			(void) h3_write_cc_after_read_pkt_fail(hc, &path, rv_e);
		    }
		    h3_conn_free(hc);
		    continue;
		}
	    }
	    h3_handlers_add(hs, hc);
	    H3_STAT_INC(drv, conn_accepted);
	    (void) ngtcp2_conn_handle_expiry(hc->conn, h3_now());
	    (void) h3_write_streams(hc);
	}
    }
}

int
NsHttp3DriverInit(Driver *drv, const char *configPath)
{
    SSL_CTX *ctx = NULL;
    const char *cert, *key;
    H3HandlerSet *hs;
    int n, port = (int) drv->port;
    int enable = 0;

    if (!Ns_ConfigGetBool(configPath, "h3", &enable) || !enable) {
	drv->quic.enabled = 0;
	return NS_OK;
    }
    drv->quic.enabled = 1;
    if (RAND_bytes(drv->quic.staticSecret, (int) sizeof(drv->quic.staticSecret))
	    != 1) {
	Ns_Log(Error, "nsssl: RAND_bytes failed for QUIC static secret");
	return NS_ERROR;
    }
    drv->quic.qpackMaxDtable = 4096u;
    drv->quic.qpackBlockedStreams = 100u;
    drv->quic.maxStreamsBidi = 100u;
    drv->quic.maxStreamsUni = 3u;
    drv->quic.maxData = 10ull * 1024ull * 1024ull;
    drv->quic.maxStreamDataBidiLocal = 1024ull * 1024ull;
    drv->quic.maxStreamDataBidiRemote = 1024ull * 1024ull;
    drv->quic.maxStreamDataUni = 64ull * 1024ull;
    drv->quic.maxIdleTimeout = 30 * NGTCP2_SECONDS;
    drv->quic.maxTxUdpPayload = 0u;
    if (Ns_ConfigGetInt(configPath, "h3_max_streams_bidi", &n) && n > 0) {
	drv->quic.maxStreamsBidi = (uint64_t) n;
    }
    if (Ns_ConfigGetInt(configPath, "h3_max_idle_timeout_ms", &n) && n > 0) {
	drv->quic.maxIdleTimeout = (uint64_t) n * NGTCP2_MILLISECONDS;
    }
    if (Ns_ConfigGetInt(configPath, "h3_max_tx_udp_payload_size", &n)
	    && n >= 1200 && n <= 65535) {
	drv->quic.maxTxUdpPayload = (uint32_t) n;
    }
    if (Ns_ConfigGetInt(configPath, "h3_udp_port", &n) && n > 0 && n < 65536) {
	port = n;
    }
    cert = Ns_ConfigGet(configPath, "certificate");
    key = Ns_ConfigGet(configPath, "key");
    if (cert == NULL || key == NULL) {
	Ns_Log(Error, "nsssl: h3=1 requires certificate and key in %s",
	    configPath);
	return NS_ERROR;
    }
    if (ngtcp2_crypto_ossl_init() != 0) {
	Ns_Log(Error, "nsssl: ngtcp2_crypto_ossl_init failed");
	return NS_ERROR;
    }
    ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == NULL) {
	Ns_Log(Error, "nsssl: QUIC SSL_CTX_new failed");
	return NS_ERROR;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_alpn_select_cb(ctx, h3_alpn_select, NULL);
    SSL_CTX_set_client_hello_cb(ctx, h3_quic_require_transport_params_cb, NULL);
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
    if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1
	    || SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1
	    || SSL_CTX_check_private_key(ctx) != 1) {
	Ns_Log(Error, "nsssl: QUIC SSL_CTX cert/key load failed (%s)", configPath);
	SSL_CTX_free(ctx);
	return NS_ERROR;
    }
    drv->quic.sslCtx = ctx;
    drv->quic.udpSock = Ns_SockUdpListen(drv->bindaddr, port);
    if (drv->quic.udpSock == INVALID_SOCKET) {
	Ns_Log(Error, "nsssl: QUIC UDP bind %s:%d failed: %s", drv->bindaddr,
	    port, ns_sockstrerror(ns_sockerrno));
	SSL_CTX_free(ctx);
	drv->quic.sslCtx = NULL;
	return NS_ERROR;
    }
    Ns_SockSetNonBlocking(drv->quic.udpSock);
    if (Ns_GetSockAddr(&drv->quic.localAddr, drv->bindaddr, port) != NS_OK) {
	Ns_Log(Error, "nsssl: QUIC Ns_GetSockAddr failed for %s:%d", drv->bindaddr,
	    port);
	ns_sockclose(drv->quic.udpSock);
	drv->quic.udpSock = INVALID_SOCKET;
	SSL_CTX_free(ctx);
	drv->quic.sslCtx = NULL;
	return NS_ERROR;
    }
    hs = (H3HandlerSet *) ns_calloc(1, sizeof(H3HandlerSet));
    if (hs == NULL) {
	Ns_Log(Error, "nsssl: QUIC handler set alloc failed");
	ns_sockclose(drv->quic.udpSock);
	drv->quic.udpSock = INVALID_SOCKET;
	SSL_CTX_free(ctx);
	drv->quic.sslCtx = NULL;
	return NS_ERROR;
    }
    drv->quic.handlers = hs;
    Ns_Log(Notice, "HTTP/3 QUIC UDP on %s:%d (driver %s)", drv->bindaddr, port,
	drv->fullname);
    return NS_OK;
}

void
NsHttp3DriverShutdown(Driver *drv)
{
    H3HandlerSet *hs;
    NsH3Conn *hc;

    if (drv == NULL) {
	return;
    }
    hs = (H3HandlerSet *) drv->quic.handlers;
    if (hs != NULL) {
	while ((hc = hs->head) != NULL) {
	    h3_conn_free(hc);
	}
	ns_free(hs);
	drv->quic.handlers = NULL;
    }
    if (drv->quic.udpSock != INVALID_SOCKET) {
	ns_sockclose(drv->quic.udpSock);
	drv->quic.udpSock = INVALID_SOCKET;
    }
    if (drv->quic.sslCtx != NULL) {
	SSL_CTX_free((SSL_CTX *) drv->quic.sslCtx);
	drv->quic.sslCtx = NULL;
    }
    drv->quic.enabled = 0;
}

/*
 * Grow HTTP/3 response body buffer for streaming DATA.
 */
static int
H3AppendBody(Conn *connPtr, const char *p, size_t len)
{
    unsigned char *q;

    if (len == 0u) {
	return 0;
    }
    q = ns_realloc(connPtr->h3_write_buf, connPtr->h3_write_len + len);
    if (q == NULL) {
	return -1;
    }
    connPtr->h3_write_buf = q;
    memcpy(connPtr->h3_write_buf + connPtr->h3_write_len, p, len);
    connPtr->h3_write_len += len;
    return 0;
}

static nghttp3_ssize
h3_read_resp_data(nghttp3_conn *conn, int64_t stream_id, nghttp3_vec *vec,
    size_t veccnt, uint32_t *pflags, void *conn_user_data,
    void *stream_user_data)
{
    Conn *connPtr = stream_user_data;
    size_t n;
    (void) conn;
    (void) stream_id;
    (void) conn_user_data;
    if (veccnt == 0) {
	return 0;
    }
    if (connPtr->h3_write_buf == NULL
	    || connPtr->h3_write_off >= connPtr->h3_write_len) {
	if (connPtr->h3_flush_mode == 1) {
	    return NGHTTP3_ERR_WOULDBLOCK;
	}
	*pflags |= NGHTTP3_DATA_FLAG_EOF;
	vec[0].len = 0;
	return 1;
    }
    n = connPtr->h3_write_len - connPtr->h3_write_off;
    if (n > vec[0].len) {
	n = vec[0].len;
    }
    memcpy(vec[0].base, connPtr->h3_write_buf + connPtr->h3_write_off, n);
    vec[0].len = n;
    connPtr->h3_write_off += n;
    if (connPtr->h3_write_off >= connPtr->h3_write_len) {
	if (connPtr->h3_flush_mode == 1) {
	    ns_free(connPtr->h3_write_buf);
	    connPtr->h3_write_buf = NULL;
	    connPtr->h3_write_len = connPtr->h3_write_off = 0u;
	    return NGHTTP3_ERR_WOULDBLOCK;
	}
	*pflags |= NGHTTP3_DATA_FLAG_EOF;
    }
    return 1;
}

int
NsHttp3ConnFlushDirect(Ns_Conn *conn, char *buf, int len, int stream)
{
    Conn *connPtr = (Conn *) conn;
    NsH3Conn *hc = connPtr->h3_handler;
    nghttp3_nv *nva = NULL;
    size_t nnv = 0, i;
    nghttp3_data_reader dr;
    char statbuf[8];
    int rv;

    /*
     * stream: non-zero => more Ns_ConnFlushDirect calls follow; zero => last
     * body chunk (RFC 9114 response DATA / FIN).
     */
    connPtr->h3_flush_mode = stream ? 1 : 0;
    if (hc == NULL || hc->httpconn == NULL) {
	return NS_ERROR;
    }
    if (!connPtr->h3_response_started) {
	snprintf(statbuf, sizeof(statbuf), "%d", connPtr->status);
	nnv = 1u + (size_t) Ns_SetSize(connPtr->outputheaders);
	nva = (nghttp3_nv *) ns_calloc(nnv, sizeof(nghttp3_nv));
	nva[0].name = (uint8_t *) ":status";
	nva[0].namelen = 7;
	nva[0].value = (uint8_t *) statbuf;
	nva[0].valuelen = strlen(statbuf);
	nva[0].flags = NGHTTP3_NV_FLAG_NONE;
	for (i = 1u; i < nnv; i++) {
	    nva[i].name = (uint8_t *) Ns_SetKey(connPtr->outputheaders,
		(int) i - 1);
	    nva[i].namelen = strlen(Ns_SetKey(connPtr->outputheaders,
		(int) i - 1));
	    nva[i].value = (uint8_t *) Ns_SetValue(connPtr->outputheaders,
		(int) i - 1);
	    nva[i].valuelen = strlen(Ns_SetValue(connPtr->outputheaders,
		(int) i - 1));
	    nva[i].flags = NGHTTP3_NV_FLAG_NONE;
	}
	ns_free(connPtr->h3_write_buf);
	connPtr->h3_write_buf = NULL;
	connPtr->h3_write_len = 0u;
	connPtr->h3_write_off = 0u;
	if (buf != NULL && len > 0 && !(conn->flags & NS_CONN_SKIPBODY)) {
	    if (H3AppendBody(connPtr, buf, (size_t) len) != 0) {
		ns_free(nva);
		return NS_ERROR;
	    }
	}
	nghttp3_conn_set_stream_user_data(hc->httpconn, connPtr->h3_stream_id,
	    connPtr);
	memset(&dr, 0, sizeof(dr));
	dr.read_data = h3_read_resp_data;
	if (connPtr->h3_write_buf != NULL) {
	    rv = nghttp3_conn_submit_response(hc->httpconn,
		connPtr->h3_stream_id, nva, nnv, &dr);
	} else {
	    rv = nghttp3_conn_submit_response(hc->httpconn,
		connPtr->h3_stream_id, nva, nnv, NULL);
	}
	ns_free(nva);
	if (rv != 0) {
	    return NS_ERROR;
	}
	connPtr->h3_response_started = 1;
    } else if (!(conn->flags & NS_CONN_SKIPBODY) && len > 0) {
	if (H3AppendBody(connPtr, buf, (size_t) len) != 0) {
	    return NS_ERROR;
	}
	rv = nghttp3_conn_unblock_stream(hc->httpconn, connPtr->h3_stream_id);
	if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
	    return NS_ERROR;
	}
    }
    if (h3_write_streams(hc) != 0) {
	return NS_ERROR;
    }
    return NS_OK;
}

int
NsHttp3ConnSend(Conn *connPtr, struct iovec *bufs, int nbufs)
{
    Ns_Conn *conn = (Ns_Conn *) connPtr;
    int towrite = 0, i, rc, sm;
    char *merged;
    size_t off, oblen;

    for (i = 0; i < nbufs; ++i) {
	towrite += (int) bufs[i].iov_len;
    }
    oblen = (size_t) connPtr->obuf.length;
    sm = connPtr->h3_flush_mode;
    if (sm < 0) {
	sm = 0;
    }
    connPtr->h3_flush_mode = -1;
    if (oblen == 0u && towrite == 0) {
	if (conn->flags & NS_CONN_SENTHDRS) {
	    rc = NsHttp3ConnFlushDirect(conn, NULL, 0, sm);
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
    rc = NsHttp3ConnFlushDirect(conn, merged, (int) off, sm);
    ns_free(merged);
    return rc == NS_OK ? (int) off : -1;
}

void
NsHttp3ConnDetach(Conn *connPtr)
{
    connPtr->h3_handler = NULL;
}
