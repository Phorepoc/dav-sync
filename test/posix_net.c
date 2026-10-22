/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */
#include "posix_net.h"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int posix_net_raw_open(void *ctx, const char *host, unsigned port);
static int p_open(void *ctx, const char *host, unsigned port, int tls)
{
    if (tls) return -1;
    return posix_net_raw_open(ctx, host, port);
}

int posix_net_raw_open(void *ctx, const char *host, unsigned port)
{
    posix_net *p = (posix_net *)ctx;
    struct addrinfo hints, *res, *a;
    char ps[8];
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof ps, "%u", port);
    if (getaddrinfo(host, ps, &hints, &res)) return -1;
    p->fd = -1;
    for (a = res; a; a = a->ai_next) {
        int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) { p->fd = fd; break; }
        close(fd);
    }
    freeaddrinfo(res);
    if (p->fd < 0) return -1;
    {
        int one = 1;
        struct timeval tv = { 20, 0 };
        setsockopt(p->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        setsockopt(p->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    return 0;
}

int posix_net_raw_send(void *ctx, const void *d, size_t n);
static int p_send(void *ctx, const void *d, size_t n);
int posix_net_raw_send(void *ctx, const void *d, size_t n) { return p_send(ctx, d, n); }
static int p_send(void *ctx, const void *d, size_t n)
{
    posix_net *p = (posix_net *)ctx;
    size_t off = 0;
    while (off < n) {
        ssize_t r = send(p->fd, (const char *)d + off, n - off, MSG_NOSIGNAL);
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return (int)n;
}

static int p_recv(void *ctx, void *d, size_t n)
{
    posix_net *p = (posix_net *)ctx;
    ssize_t r = recv(p->fd, d, n, 0);
    return r < 0 ? -1 : (int)r;
}

static int p_recv(void *ctx, void *d, size_t n);
int posix_net_raw_recv(void *ctx, void *d, size_t n) { return p_recv(ctx, d, n); }
static void p_close(void *ctx)
{
    posix_net *p = (posix_net *)ctx;
    if (p->fd >= 0) close(p->fd);
    p->fd = -1;
}

phedav_net posix_net_make(posix_net *p)
{
    phedav_net n;
    p->fd = -1;
    n.ctx = p; n.open = p_open; n.send = p_send; n.recv = p_recv; n.close = p_close;
    return n;
}

void posix_net_raw_close(void *ctx) { p_close(ctx); }
