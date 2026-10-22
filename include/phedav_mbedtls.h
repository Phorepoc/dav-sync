/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

/*
 * phedav_mbedtls -- adds TLS 1.2 / 1.3 (mbedTLS 3.6, an external dependency, not vendored) to a plain byte-stream provider and
 * presents the result as a phedav_net.
 *
 * The host supplies a raw stream (a socket: connect / send / recv / close). For a request with tls = 0 the adapter passes
 * straight through; for tls = 1 it runs the handshake after the raw connect. Server verification, in order of preference:
 *   - ca_pem: a PEM bundle of trusted roots; the chain and the host name are verified (the normal case: Let's Encrypt etc.)
 *   - pin:    the SHA-256 of the server certificate's DER encoding (self-signed servers); no chain validation
 *   - neither: the connection is refused (there is deliberately no "insecure" switch)
 * No malloc of its own (mbedTLS allocates internally; on a phone point it at a static pool with MBEDTLS_MEMORY_BUFFER_ALLOC_C).
 * Random numbers: mbedTLS needs an entropy source; a Symbian build must provide mbedtls_hardware_poll().
 */
#ifndef PHEDAV_MBEDTLS_H
#define PHEDAV_MBEDTLS_H

#include "phedav.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ctx;
    int (*open)(void *ctx, const char *host, unsigned port);   /* TCP connect; 0 or < 0 */
    int (*send)(void *ctx, const void *p, size_t n);           /* all n bytes, or < 0 */
    int (*recv)(void *ctx, void *p, size_t n);                 /* blocking: > 0, 0 = EOF, < 0 = error / timeout */
    void (*close)(void *ctx);
} phedav_raw;

typedef struct {
    phedav_raw raw;
    const char *ca_pem;             /* NUL-terminated PEM bundle (the NUL is part of the length mbedTLS wants) */
    uint8_t pin[32];
    int have_pin;
    int tls_active;
    int debug;                      /* nonzero: print the mbedTLS error of a failed handshake to stderr */
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
} phedav_tls;

/* ca_pem and/or pin (32 bytes) may be NULL; at least one is needed to open a TLS connection */
void phedav_tls_init(phedav_tls *t, phedav_raw raw, const char *ca_pem, const uint8_t *pin);
phedav_net phedav_tls_net(phedav_tls *t);

#ifdef __cplusplus
}
#endif
#endif
