/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */
#include "phedav_mbedtls.h"

#include <psa/crypto.h>
#include <stdio.h>
#include <string.h>

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    phedav_tls *t = (phedav_tls *)ctx;
    int r = t->raw.send(t->raw.ctx, buf, len);
    return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)len;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    phedav_tls *t = (phedav_tls *)ctx;
    int r = t->raw.recv(t->raw.ctx, buf, len);
    if (r > 0) return r;
    return MBEDTLS_ERR_SSL_CONN_EOF;       /* 0 = EOF, < 0 = error or timeout */
}

void phedav_tls_init(phedav_tls *t, phedav_raw raw, const char *ca_pem, const uint8_t *pin)
{
    memset(t, 0, sizeof *t);
    t->raw = raw;
    t->ca_pem = ca_pem;
    if (pin) { memcpy(t->pin, pin, 32); t->have_pin = 1; }
}

static void tls_teardown(phedav_tls *t)
{
    if (!t->tls_active) return;
    mbedtls_ssl_close_notify(&t->ssl);
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_x509_crt_free(&t->ca);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
    t->tls_active = 0;
}

static int t_open(void *ctx, const char *host, unsigned port, int tls)
{
    phedav_tls *t = (phedav_tls *)ctx;
    static const char pers[] = "phedav";
    unsigned char hash[32];
    size_t hl = 0;
    int rc;

    if (tls && !t->ca_pem && !t->have_pin) return -1;
    if (t->raw.open(t->raw.ctx, host, port) < 0) return -1;
    if (!tls) return 0;

    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_x509_crt_init(&t->ca);
    mbedtls_entropy_init(&t->entropy);
    mbedtls_ctr_drbg_init(&t->drbg);
    t->tls_active = 1;
    if (psa_crypto_init() != PSA_SUCCESS) goto fail;
    if (mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy, (const unsigned char *)pers, sizeof pers - 1)) goto fail;
    if (mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) goto fail;
    mbedtls_ssl_conf_min_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
    if (t->ca_pem) {
        if (mbedtls_x509_crt_parse(&t->ca, (const unsigned char *)t->ca_pem, strlen(t->ca_pem) + 1) < 0) goto fail;
        mbedtls_ssl_conf_ca_chain(&t->conf, &t->ca, NULL);
        mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE);        /* the pin replaces chain validation */
    }
    if (mbedtls_ssl_setup(&t->ssl, &t->conf)) goto fail;
    if (mbedtls_ssl_set_hostname(&t->ssl, host)) goto fail;                  /* SNI and, with a CA bundle, name verification */
    mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);
    while ((rc = mbedtls_ssl_handshake(&t->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (t->debug) fprintf(stderr, "phedav_tls: handshake failed: -0x%04x\n", (unsigned)-rc);
            goto fail;
        }
    }
    if (!t->ca_pem) {            /* pin mode */
        const mbedtls_x509_crt *crt = mbedtls_ssl_get_peer_cert(&t->ssl);
        if (!crt) goto fail;
        if (psa_hash_compute(PSA_ALG_SHA_256, crt->raw.p, crt->raw.len, hash, sizeof hash, &hl) != PSA_SUCCESS || hl != 32) goto fail;
        if (memcmp(hash, t->pin, 32) != 0) goto fail;
    }
    return 0;
fail:
    tls_teardown(t);
    t->raw.close(t->raw.ctx);
    return -1;
}

static int t_send(void *ctx, const void *p, size_t n)
{
    phedav_tls *t = (phedav_tls *)ctx;
    size_t off = 0;
    if (!t->tls_active) return t->raw.send(t->raw.ctx, p, n);
    while (off < n) {
        int rc = mbedtls_ssl_write(&t->ssl, (const unsigned char *)p + off, n - off);
        if (rc > 0) { off += (size_t)rc; continue; }
        if (rc == MBEDTLS_ERR_SSL_WANT_WRITE || rc == MBEDTLS_ERR_SSL_WANT_READ) continue;
        return -1;
    }
    return (int)n;
}

static int t_recv(void *ctx, void *p, size_t n)
{
    phedav_tls *t = (phedav_tls *)ctx;
    if (!t->tls_active) return t->raw.recv(t->raw.ctx, p, n);
    for (;;) {
        int rc = mbedtls_ssl_read(&t->ssl, (unsigned char *)p, n);
        if (rc > 0) return rc;
        if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET || rc == MBEDTLS_ERR_SSL_WANT_READ) continue;   /* TLS 1.3 ticket: not data */
        return -1;
    }
}

static void t_close(void *ctx)
{
    phedav_tls *t = (phedav_tls *)ctx;
    tls_teardown(t);
    t->raw.close(t->raw.ctx);
}

phedav_net phedav_tls_net(phedav_tls *t)
{
    phedav_net n;
    n.ctx = t; n.open = t_open; n.send = t_send; n.recv = t_recv; n.close = t_close;
    return n;
}
