/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef POSIX_NET_H
#define POSIX_NET_H
#include "phedav.h"
typedef struct { int fd; } posix_net;
/* a plain-TCP phedav_net (blocking, 20 s receive timeout, TCP_NODELAY); tls requests are refused */
phedav_net posix_net_make(posix_net *p);
/* the same stream without the phedav_net wrapper, for the TLS adapter */
int posix_net_raw_open(void *ctx, const char *host, unsigned port);
int posix_net_raw_send(void *ctx, const void *d, size_t n);
int posix_net_raw_recv(void *ctx, void *d, size_t n);
void posix_net_raw_close(void *ctx);
#endif
