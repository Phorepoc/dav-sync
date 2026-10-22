/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */
#include "phedav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_HOPS 6

/* ------------------------------------------------------------------------------------------------ small helpers */

size_t phedav_base64(const void *in, size_t n, char *out, size_t cap)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *p = (const unsigned char *)in;
    size_t i, o = 0;
    if (cap < ((n + 2) / 3) * 4 + 1) return 0;
    for (i = 0; i + 2 < n; i += 3) {
        unsigned v = ((unsigned)p[i] << 16) | ((unsigned)p[i + 1] << 8) | p[i + 2];
        out[o++] = t[v >> 18]; out[o++] = t[(v >> 12) & 63]; out[o++] = t[(v >> 6) & 63]; out[o++] = t[v & 63];
    }
    if (i < n) {
        unsigned v = (unsigned)p[i] << 16;
        if (i + 1 < n) v |= (unsigned)p[i + 1] << 8;
        out[o++] = t[v >> 18]; out[o++] = t[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? t[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

static int ieq_n(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return 1;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* parses "http(s)://host[:port]/path"; returns 0 and fills the parts */
static int parse_url(const char *url, int *tls, char *host, size_t hcap, unsigned *port, const char **path)
{
    const char *p, *e, *colon;
    size_t hl;
    if (!strncmp(url, "https://", 8)) { *tls = 1; p = url + 8; }
    else if (!strncmp(url, "http://", 7)) { *tls = 0; p = url + 7; }
    else return -1;
    e = p;
    while (*e && *e != '/' && *e != '?') e++;
    colon = NULL;
    {
        const char *q;
        for (q = p; q < e; q++) if (*q == ':') colon = q;     /* IPv6 literals are not supported */
    }
    hl = (size_t)((colon ? colon : e) - p);
    if (hl == 0 || hl >= hcap) return -1;
    memcpy(host, p, hl);
    host[hl] = 0;
    *port = *tls ? 443u : 80u;
    if (colon) {
        unsigned v = 0;
        const char *q;
        if (colon + 1 == e) return -1;
        for (q = colon + 1; q < e; q++) {
            if (*q < '0' || *q > '9') return -1;
            v = v * 10 + (unsigned)(*q - '0');
            if (v > 65535) return -1;
        }
        *port = v;
    }
    *path = (*e == '/') ? e : "/";
    return 0;
}

void phedav_href_norm(const char *in, char *out, size_t cap)
{
    int tls;
    char host[PHEDAV_HOST_MAX];
    unsigned port;
    const char *path;
    if (cap == 0) return;
    if ((!strncmp(in, "http://", 7) || !strncmp(in, "https://", 8)) && parse_url(in, &tls, host, sizeof host, &port, &path) == 0)
        copy_str(out, cap, path);
    else copy_str(out, cap, in);
}

int phedav_set_origin(phedav *c, const char *url, char *path_out, size_t cap)
{
    const char *path;
    int tls;
    char host[PHEDAV_HOST_MAX];
    unsigned port;
    if (parse_url(url, &tls, host, sizeof host, &port, &path)) return PHEDAV_ERR_ARG;
    c->tls = tls;
    copy_str(c->host, sizeof c->host, host);
    c->port = port;
    if (path_out && cap) copy_str(path_out, cap, path);
    return PHEDAV_OK;
}

int phedav_init(phedav *c, phedav_net net, const char *user, const char *pass,
                char *item_buf, size_t item_cap, void *work, size_t work_cap)
{
    memset(c, 0, sizeof *c);
    c->net = net;
    c->user_agent = "phedav/" PHEDAV_VERSION;
    c->item = item_buf;
    c->item_cap = item_cap;
    c->work = (unsigned char *)work;
    c->work_cap = work_cap;
    if (user) {
        char up[200];
        size_t n;
        const char *pw = pass ? pass : "";
        if (strlen(user) + strlen(pw) + 2 > sizeof up) return PHEDAV_ERR_ARG;
        n = (size_t)snprintf(up, sizeof up, "%s:%s", user, pw);
        memcpy(c->auth, "Basic ", 6);
        if (!phedav_base64(up, n, c->auth + 6, sizeof c->auth - 6)) return PHEDAV_ERR_ARG;
    }
    return PHEDAV_OK;
}

void phedav_close(phedav *c)
{
    if (c->connected && c->net.close) c->net.close(c->net.ctx);
    c->connected = 0;
    c->rpos = c->rlen = 0;
}

/* ------------------------------------------------------------------------------------------------ connection I/O */

static int fill(phedav *c)
{
    int n;
    if (c->rpos < c->rlen) return 1;
    n = c->net.recv(c->net.ctx, c->rbuf, sizeof c->rbuf);
    if (n < 0) return PHEDAV_ERR_IO;
    if (n == 0) return 0;
    c->rpos = 0;
    c->rlen = (size_t)n;
    return 1;
}

/* reads a line (CRLF or LF stripped) into buf; returns length >= 0, PHEDAV_ERR_IO on EOF/error, PHEDAV_ERR_BUF if too long */
static int read_line(phedav *c, char *buf, size_t cap)
{
    size_t n = 0;
    for (;;) {
        int f = fill(c);
        char ch;
        if (f <= 0) return PHEDAV_ERR_IO;
        ch = (char)c->rbuf[c->rpos++];
        if (ch == '\n') {
            if (n && buf[n - 1] == '\r') n--;
            buf[n] = 0;
            return (int)n;
        }
        if (n + 1 >= cap) return PHEDAV_ERR_BUF;
        buf[n++] = ch;
    }
}

static int ensure_conn(phedav *c, int *reused)
{
    *reused = 0;
    if (c->connected) {
        if (c->c_tls == c->tls && c->c_port == c->port && !strcmp(c->c_host, c->host)) { *reused = 1; return 0; }
        phedav_close(c);
    }
    if (c->net.open(c->net.ctx, c->host, c->port, c->tls) < 0) return PHEDAV_ERR_IO;
    c->connected = 1;
    c->c_tls = c->tls;
    c->c_port = c->port;
    copy_str(c->c_host, sizeof c->c_host, c->host);
    c->rpos = c->rlen = 0;
    return 0;
}

static int send_all(phedav *c, const void *p, size_t n)
{
    return (n == 0 || c->net.send(c->net.ctx, p, n) == (int)n) ? 0 : PHEDAV_ERR_IO;
}

/* ------------------------------------------------------------------------------------------------ one exchange */

static int read_headers(phedav *c, int *http10)
{
    char *l = c->hdr;
    for (;;) {
        int n = read_line(c, l, sizeof c->hdr);
        if (n < 0) return n;
        if (n < 12 || strncmp(l, "HTTP/1.", 7) != 0) return PHEDAV_ERR_PROTO;
        *http10 = (l[7] == '0');
        c->status = atoi(l + 9);
        if (c->status < 100 || c->status > 599) return PHEDAV_ERR_PROTO;
        c->etag[0] = 0;
        c->location[0] = 0;
        c->content_length = -1;
        c->chunked = 0;
        c->conn_close = *http10;
        for (;;) {
            char *v;
            n = read_line(c, l, sizeof c->hdr);
            if (n < 0) return n;
            if (n == 0) break;
            v = strchr(l, ':');
            if (!v) continue;
            {
                size_t nl = (size_t)(v - l);
                v++;
                while (*v == ' ' || *v == '\t') v++;
                if (nl == 14 && ieq_n(l, "content-length", 14)) c->content_length = atol(v);
                else if (nl == 17 && ieq_n(l, "transfer-encoding", 17)) { if (strstr(v, "hunked")) c->chunked = 1; }
                else if (nl == 10 && ieq_n(l, "connection", 10)) {
                    if (strstr(v, "lose")) c->conn_close = 1;
                    else if (strstr(v, "eep-alive")) c->conn_close = 0;
                } else if (nl == 4 && ieq_n(l, "etag", 4)) copy_str(c->etag, sizeof c->etag, v);
                else if (nl == 8 && ieq_n(l, "location", 8)) copy_str(c->location, sizeof c->location, v);
            }
        }
        if (c->status >= 200) return 0;      /* 1xx: read the next status line */
    }
}

/* passes n body bytes to the sink (or drops them) */
static int take(phedav *c, size_t n, phedav_sink sink, void *sctx)
{
    while (n) {
        int f = fill(c);
        size_t k;
        if (f <= 0) return PHEDAV_ERR_IO;
        k = c->rlen - c->rpos;
        if (k > n) k = n;
        if (sink) {
            int r = sink(sctx, c->rbuf + c->rpos, k);
            if (r) { c->conn_close = 1; return r < 0 ? r : PHEDAV_ERR_BUF; }
        }
        c->rpos += k;
        n -= k;
    }
    return 0;
}

static int read_body(phedav *c, int deliver, phedav_sink sink, void *sctx)
{
    phedav_sink s = deliver ? sink : NULL;
    int r;
    if (c->status == 204 || c->status == 304 || c->status < 200) return 0;
    if (c->chunked) {
        for (;;) {
            char *l = c->hdr;
            unsigned long sz;
            r = read_line(c, l, sizeof c->hdr);
            if (r < 0) return r;
            if (r == 0) continue;
            sz = strtoul(l, NULL, 16);
            if (sz == 0) {
                for (;;) { r = read_line(c, l, sizeof c->hdr); if (r < 0) return r; if (r == 0) break; }
                return 0;
            }
            if ((r = take(c, sz, s, sctx))) return r;
        }
    }
    if (c->content_length >= 0) return take(c, (size_t)c->content_length, s, sctx);
    /* until the server closes */
    c->conn_close = 1;
    for (;;) {
        int f = fill(c);
        size_t k;
        if (f < 0) return f;
        if (f == 0) return 0;
        k = c->rlen - c->rpos;
        if (s && (r = s(sctx, c->rbuf + c->rpos, k)) != 0) return r < 0 ? r : PHEDAV_ERR_BUF;
        c->rpos = c->rlen;
    }
}

static int exchange(phedav *c, const char *method, const char *path, const char *extra, const void *body, size_t blen,
                    phedav_sink sink, void *sctx)
{
    int attempt;
    for (attempt = 0; attempt < 2; attempt++) {
        int reused, r, n, http10 = 0;
        char *h = c->hdr;
        size_t hl;
        char hostline[PHEDAV_HOST_MAX + 8];
        if ((r = ensure_conn(c, &reused))) return r;
        if ((c->tls && c->port != 443) || (!c->tls && c->port != 80))
            snprintf(hostline, sizeof hostline, "%s:%u", c->host, c->port);
        else copy_str(hostline, sizeof hostline, c->host);
        n = snprintf(h, sizeof c->hdr, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\n%s%s%s%s",
                     method, path, hostline, c->user_agent,
                     c->auth[0] ? "Authorization: " : "", c->auth[0] ? c->auth : "", c->auth[0] ? "\r\n" : "",
                     extra ? extra : "");
        if (n < 0 || (size_t)n >= sizeof c->hdr - 64) return PHEDAV_ERR_BUF;
        hl = (size_t)n;
        if (body || blen || !strcmp(method, "PUT") || !strcmp(method, "POST") || !strcmp(method, "PROPFIND") || !strcmp(method, "REPORT"))
            hl += (size_t)snprintf(h + hl, sizeof c->hdr - hl, "Content-Length: %lu\r\n", (unsigned long)blen);
        hl += (size_t)snprintf(h + hl, sizeof c->hdr - hl, "\r\n");
        r = send_all(c, h, hl);
        if (!r && blen) r = send_all(c, body, blen);
        if (r) {
            phedav_close(c);
            if (reused && attempt == 0) continue;     /* stale keep-alive connection */
            return r;
        }
        /* first byte of the answer: a reused connection that dies here is retried once */
        {
            int f = fill(c);
            if (f <= 0) {
                phedav_close(c);
                if (reused && attempt == 0) continue;
                return PHEDAV_ERR_IO;
            }
        }
        if ((r = read_headers(c, &http10))) { phedav_close(c); return r; }
        r = read_body(c, c->status >= 200 && c->status < 300, sink, sctx);
        if (r || c->conn_close) phedav_close(c);
        return r;
    }
    return PHEDAV_ERR_IO;
}

/* ------------------------------------------------------------------------------------------------ redirects */

int phedav_request(phedav *c, const char *method, const char *target, const char *extra,
                   const void *body, size_t blen, phedav_sink sink, void *sctx)
{
    char cur[PHEDAV_HREF_MAX + 16];
    int hop;
    char m[16];
    copy_str(m, sizeof m, method);
    copy_str(cur, sizeof cur, target);
    for (hop = 0; hop < MAX_HOPS; hop++) {
        const char *path = cur;
        int r, tls;
        char host[PHEDAV_HOST_MAX];
        unsigned port;
        if (!strncmp(cur, "http://", 7) || !strncmp(cur, "https://", 8)) {
            if (parse_url(cur, &tls, host, sizeof host, &port, &path)) return PHEDAV_ERR_PROTO;
            if (tls != c->tls || port != c->port || strcmp(host, c->host)) {
                if (!c->allow_cross_origin) return PHEDAV_ERR_REDIRECT;
                if (c->tls && !tls) return PHEDAV_ERR_REDIRECT;       /* never downgrade */
                c->tls = tls; c->port = port; copy_str(c->host, sizeof c->host, host);
            }
        }
        c->requests++;
        r = exchange(c, m, path, extra, body, blen, sink, sctx);
        if (r) return r;
        if (c->status == 401) return PHEDAV_ERR_AUTH;
        if ((c->status == 301 || c->status == 302 || c->status == 303 || c->status == 307 || c->status == 308) && c->location[0]) {
            char loc[sizeof c->location];
            copy_str(loc, sizeof loc, c->location);
            if (c->status == 303) { copy_str(m, sizeof m, "GET"); body = NULL; blen = 0; }
            if (loc[0] == '/') {
                if (loc[1] == '/') return PHEDAV_ERR_REDIRECT;
                copy_str(cur, sizeof cur, loc);
            } else if (!strncmp(loc, "http://", 7) || !strncmp(loc, "https://", 8)) {
                copy_str(cur, sizeof cur, loc);
            } else {       /* relative to the directory of the current path */
                size_t dl = strlen(path);
                while (dl && path[dl - 1] != '/') dl--;
                if (dl + strlen(loc) + 1 > sizeof cur) return PHEDAV_ERR_BUF;
                memmove(cur, path, dl);
                copy_str(cur + dl, sizeof cur - dl, loc);
            }
            continue;
        }
        return PHEDAV_OK;
    }
    return PHEDAV_ERR_REDIRECT;
}
