/*
 * TLS 1.2 PSK channel. The sensor is the TLS client and offers exactly one
 * suite, TLS_PSK_WITH_AES_128_GCM_SHA256; the host runs OpenSSL as the server
 * over memory BIOs and ferries records through 0xb0 packs.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "gx5125.h"

#define HANDSHAKE_ROUNDS 10

struct gx_tls {
    SSL_CTX *ctx;
    SSL *ssl;
    BIO *rbio, *wbio;
    uint8_t psk[GX_PSK_LEN];
};

static unsigned int psk_cb(SSL *ssl, const char *identity, unsigned char *psk,
                           unsigned int max_len)
{
    struct gx_tls *tls = SSL_get_app_data(ssl);

    (void)identity;  /* the sensor sends OpenSSL's default "Client_identity" */
    if (!tls || max_len < GX_PSK_LEN)
        return 0;
    memcpy(psk, tls->psk, GX_PSK_LEN);
    return GX_PSK_LEN;
}

static void tls_free(struct gx_tls *tls)
{
    if (!tls)
        return;
    if (tls->ssl)
        SSL_free(tls->ssl);  /* owns the BIOs */
    if (tls->ctx)
        SSL_CTX_free(tls->ctx);
    OPENSSL_cleanse(tls->psk, sizeof(tls->psk));
    free(tls);
}

static struct gx_tls *tls_new(const uint8_t psk[GX_PSK_LEN])
{
    struct gx_tls *tls = calloc(1, sizeof(*tls));

    if (!tls)
        return NULL;
    memcpy(tls->psk, psk, GX_PSK_LEN);

    tls->ctx = SSL_CTX_new(TLS_server_method());
    if (!tls->ctx)
        goto fail;
    SSL_CTX_set_min_proto_version(tls->ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(tls->ctx, TLS1_2_VERSION);
    /* Plain PSK has no forward secrecy; the default level refuses it. */
    SSL_CTX_set_security_level(tls->ctx, 0);
    if (SSL_CTX_set_cipher_list(tls->ctx, "PSK-AES128-GCM-SHA256") != 1)
        goto fail;
    SSL_CTX_set_psk_server_callback(tls->ctx, psk_cb);

    tls->ssl = SSL_new(tls->ctx);
    tls->rbio = BIO_new(BIO_s_mem());
    tls->wbio = BIO_new(BIO_s_mem());
    if (!tls->ssl || !tls->rbio || !tls->wbio)
        goto fail;
    /* An empty buffer must mean "not yet", not EOF. */
    BIO_set_mem_eof_return(tls->rbio, -1);
    BIO_set_mem_eof_return(tls->wbio, -1);
    SSL_set_bio(tls->ssl, tls->rbio, tls->wbio);
    SSL_set_app_data(tls->ssl, tls);
    SSL_set_accept_state(tls->ssl);
    return tls;

fail:
    if (!tls->ssl) {
        BIO_free(tls->rbio);
        BIO_free(tls->wbio);
    }
    tls_free(tls);
    return NULL;
}

/* Sends whatever OpenSSL queued. A failing handshake still queues an alert. */
static int flush_out(gx_dev *dev)
{
    uint8_t buf[GX_RX_MAX - 64];
    int n;

    while ((n = BIO_read(dev->tls->wbio, buf, sizeof(buf))) > 0) {
        int rc = gx_send_tls(dev, buf, (size_t)n);
        if (rc < 0)
            return rc;
    }
    return GX_OK;
}

static int absorb_in(gx_dev *dev, unsigned timeout_ms)
{
    const uint8_t *body;
    size_t len;
    uint8_t flags;
    int rc = gx_recv_pack(dev, &flags, &body, &len, timeout_ms);

    if (rc < 0)
        return rc;
    if (flags != GX_FLAGS_TLS) {
        if (dev->log)
            dev->log(dev->log_ctx, "TLS: unexpected pack flags %02x", flags);
        return GX_ERR_PROTOCOL;
    }
    return BIO_write(dev->tls->rbio, body, (int)len) == (int)len ? GX_OK
                                                                 : GX_ERR_TLS;
}

int gx_tls_open(gx_dev *dev, const uint8_t psk[GX_PSK_LEN])
{
    static const uint8_t none[2];
    int rc;

    if (dev->tls)
        return GX_ERR_ARG;
    dev->tls = tls_new(psk);
    if (!dev->tls)
        return GX_ERR_NOMEM;

    rc = gx_command(dev, GX_CMD_REQUEST_TLS, none, sizeof(none), NULL, 0, NULL);
    if (rc < 0)
        goto fail;

    /* ClientHello */
    rc = absorb_in(dev, dev->timeout_ms);
    if (rc < 0)
        goto fail;

    for (int round = 0; round < HANDSHAKE_ROUNDS; round++) {
        int hs = SSL_do_handshake(dev->tls->ssl);
        int err = hs == 1 ? SSL_ERROR_NONE : SSL_get_error(dev->tls->ssl, hs);

        rc = flush_out(dev);
        if (rc < 0)
            goto fail;

        if (hs == 1) {
            if (dev->log)
                dev->log(dev->log_ctx, "TLS up: %s",
                         SSL_get_cipher_name(dev->tls->ssl));
            /* Not always acknowledged right after a fresh handshake. */
            unsigned saved = dev->timeout_ms;
            dev->timeout_ms = 300;
            gx_command(dev, GX_CMD_TLS_ESTABLISHED, none, sizeof(none), NULL,
                       0, NULL);
            dev->timeout_ms = saved;
            return GX_OK;
        }
        if (err != SSL_ERROR_WANT_READ) {
            unsigned long e = ERR_get_error();
            if (dev->log)
                dev->log(dev->log_ctx, "TLS handshake failed: %s",
                         e ? ERR_reason_error_string(e) : "?");
            rc = GX_ERR_TLS;
            goto fail;
        }
        rc = absorb_in(dev, dev->timeout_ms);
        if (rc < 0)
            goto fail;
    }
    rc = GX_ERR_TLS;

fail:
    gx_tls_close(dev);
    return rc;
}

void gx_tls_close(gx_dev *dev)
{
    static const uint8_t none[2];

    if (!dev->tls)
        return;
    /* A sensor left mid-handshake refuses plain commands until it gets this,
     * even across power cycles. */
    gx_drain(dev);
    unsigned saved = dev->timeout_ms;
    dev->timeout_ms = 500;
    gx_command(dev, GX_CMD_TLS_ESTABLISHED, none, sizeof(none), NULL, 0, NULL);
    dev->timeout_ms = saved;
    gx_drain(dev);

    tls_free(dev->tls);
    dev->tls = NULL;
}

int gx_tls_read(gx_dev *dev, uint8_t *buf, size_t len)
{
    size_t got = 0;

    if (!dev->tls)
        return GX_ERR_ARG;

    while (got < len) {
        int n = SSL_read(dev->tls->ssl, buf + got, (int)(len - got));
        int rc;

        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        if (SSL_get_error(dev->tls->ssl, n) != SSL_ERROR_WANT_READ)
            return GX_ERR_TLS;
        rc = absorb_in(dev, dev->timeout_ms);
        if (rc < 0)
            return rc;
    }
    return GX_OK;
}

/* Accepts the 32 raw bytes written by extract-psk.ps1, or 64 hex digits. */
int gx_psk_load(const char *path, uint8_t psk[GX_PSK_LEN])
{
    uint8_t buf[256];
    size_t n, digits = 0;
    FILE *f = fopen(path, "rb");
    int rc = GX_ERR_ARG;

    if (!f)
        return GX_ERR_IO;
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    if (n == GX_PSK_LEN) {
        memcpy(psk, buf, GX_PSK_LEN);
        rc = GX_OK;
        goto out;
    }

    for (size_t i = 0; i < n; i++) {
        int c = buf[i];
        if (!isxdigit(c))
            continue;
        if (digits >= 2 * GX_PSK_LEN) {
            rc = GX_ERR_ARG;
            goto out;
        }
        int v = isdigit(c) ? c - '0' : tolower(c) - 'a' + 10;
        psk[digits / 2] = (uint8_t)(digits % 2 ? (psk[digits / 2] | v) : v << 4);
        digits++;
    }
    rc = digits == 2 * GX_PSK_LEN ? GX_OK : GX_ERR_ARG;

out:
    OPENSSL_cleanse(buf, sizeof(buf));
    return rc;
}
