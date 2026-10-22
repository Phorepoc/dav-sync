/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */
#include "phedav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS_DAV "DAV:"
#define NS_CAL "urn:ietf:params:xml:ns:caldav"
#define NS_CARD "urn:ietf:params:xml:ns:carddav"
#define NS_CS "http://calendarserver.org/ns/"

enum {
    K_NONE, K_OTHER, K_MULTISTATUS, K_RESPONSE, K_HREF, K_STATUS, K_PROPSTAT, K_PROP, K_CAPTURE, K_RESTYPE, K_CUP,
    K_CHS, K_DATA, K_COMPSET, K_ERROR
};

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* ------------------------------------------------------------------------------------------------ multistatus reader */

static void begin_capture(phedav_ms *m, char *dst, size_t cap, int depth)
{
    m->cap = dst;
    m->capcap = cap;
    m->caplen = 0;
    m->capkind = depth;
    if (cap) dst[0] = 0;
}

static void ms_start(void *ctx, phedav_xml *x, const char *ns, const char *name, int d)
{
    phedav *c = (phedav *)ctx;
    phedav_ms *m = &c->ms;
    int parent = (d >= 2 && d - 1 < (int)sizeof m->kind) ? m->kind[d - 1] : K_NONE;
    int k = K_OTHER;
    (void)x;
    if (d >= (int)sizeof m->kind) return;
    if (!strcmp(ns, NS_DAV)) {
        if (!strcmp(name, "multistatus")) k = K_MULTISTATUS;
        else if (!strcmp(name, "response") && parent == K_MULTISTATUS) {
            memset(&m->rec, 0, sizeof m->rec);
            m->in_resp = 1;
            c->item_len = 0;
            c->item_over = 0;
            k = K_RESPONSE;
        } else if (!strcmp(name, "href")) {
            k = K_HREF;
            if (parent == K_RESPONSE) begin_capture(m, m->rec.href, sizeof m->rec.href, d);
            else if (parent == K_CUP) begin_capture(m, m->tmp.principal, sizeof m->tmp.principal, d);
            else if (parent == K_CHS && !m->tmp.home[0]) begin_capture(m, m->tmp.home, sizeof m->tmp.home, d);
        } else if (!strcmp(name, "status")) {
            k = K_STATUS;
            if (parent == K_RESPONSE || parent == K_PROPSTAT) { m->in_status = 1; m->stlen = 0; m->stbuf[0] = 0; }
        } else if (!strcmp(name, "propstat")) { memset(&m->tmp, 0, sizeof m->tmp); m->ps_status = 0; m->in_ps = 1; k = K_PROPSTAT; }
        else if (!strcmp(name, "prop")) k = K_PROP;
        else if (!strcmp(name, "getetag")) { k = K_CAPTURE; begin_capture(m, m->tmp.etag, sizeof m->tmp.etag, d); }
        else if (!strcmp(name, "displayname")) { k = K_CAPTURE; begin_capture(m, m->tmp.name, sizeof m->tmp.name, d); }
        else if (!strcmp(name, "sync-token")) {
            k = K_CAPTURE;
            if (parent == K_MULTISTATUS) { if (m->top_token) { begin_capture(m, m->top_token, PHEDAV_TOKEN_MAX, d); m->top_seen = 1; } }
            else begin_capture(m, m->tmp.sync_token, sizeof m->tmp.sync_token, d);
        } else if (!strcmp(name, "resourcetype")) k = K_RESTYPE;
        else if (!strcmp(name, "collection") && parent == K_RESTYPE) m->tmp.flags |= PHEDAV_R_COLLECTION;
        else if (!strcmp(name, "current-user-principal")) k = K_CUP;
        else if (!strcmp(name, "error")) k = K_ERROR;
        else if (!strcmp(name, "valid-sync-token")) m->code_bad_token = 1;
    } else if (!strcmp(ns, NS_CAL)) {
        if (!strcmp(name, "calendar") && parent == K_RESTYPE) m->tmp.flags |= PHEDAV_R_CALENDAR;
        else if (!strcmp(name, "calendar-home-set")) k = K_CHS;
        else if (!strcmp(name, "calendar-data")) { k = K_DATA; m->capkind = d; c->item_len = 0; c->item_over = 0; }
        else if (!strcmp(name, "supported-calendar-component-set")) k = K_COMPSET;
        else if (!strcmp(name, "comp") && parent == K_COMPSET) {
            char v[24];
            if (phedav_xml_attr(x, "name", v, sizeof v)) {
                if (!strcmp(v, "VEVENT")) m->tmp.flags |= PHEDAV_R_VEVENT;
                else if (!strcmp(v, "VTODO")) m->tmp.flags |= PHEDAV_R_VTODO;
                else if (!strcmp(v, "VJOURNAL")) m->tmp.flags |= PHEDAV_R_VJOURNAL;
            }
        }
    } else if (!strcmp(ns, NS_CARD)) {
        if (!strcmp(name, "addressbook") && parent == K_RESTYPE) m->tmp.flags |= PHEDAV_R_ADDRESSBOOK;
        else if (!strcmp(name, "addressbook-home-set")) k = K_CHS;
        else if (!strcmp(name, "address-data")) { k = K_DATA; m->capkind = d; c->item_len = 0; c->item_over = 0; }
    } else if (!strcmp(ns, NS_CS)) {
        if (!strcmp(name, "getctag")) { k = K_CAPTURE; begin_capture(m, m->tmp.ctag, sizeof m->tmp.ctag, d); }
    }
    m->kind[d] = (unsigned char)k;
}

static void ms_text(void *ctx, const char *s, size_t n)
{
    phedav *c = (phedav *)ctx;
    phedav_ms *m = &c->ms;
    int d = c->xml.depth;
    if (d < (int)sizeof m->kind && m->kind[d] == K_DATA) {
        if (c->item && c->item_len + n <= c->item_cap) { memcpy(c->item + c->item_len, s, n); c->item_len += n; }
        else c->item_over = 1;
        return;
    }
    if (m->cap && m->capkind == d) {
        size_t room = m->capcap - 1 - m->caplen;
        if (n > room) n = room;
        memcpy(m->cap + m->caplen, s, n);
        m->caplen += n;
        m->cap[m->caplen] = 0;
    } else if (d < (int)sizeof m->kind && m->kind[d] == K_STATUS && m->in_status) {
        size_t room = sizeof m->stbuf - 1 - m->stlen;
        if (n > room) n = room;
        memcpy(m->stbuf + m->stlen, s, n);
        m->stlen += n;
        m->stbuf[m->stlen] = 0;
    }
}

static void trim(char *s)
{
    size_t n = strlen(s), a = 0;
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == '\t')) s[--n] = 0;
    while (s[a] == ' ' || s[a] == '\n' || s[a] == '\r' || s[a] == '\t') a++;
    if (a) memmove(s, s + a, n - a + 1);
}

static void merge_field(char *dst, const char *src, size_t cap)
{
    if (src[0]) copy_str(dst, cap, src);
}

static void ms_end(void *ctx, const char *ns, const char *name, int d)
{
    phedav *c = (phedav *)ctx;
    phedav_ms *m = &c->ms;
    (void)ns;
    if (d >= (int)sizeof m->kind) return;
    if (m->cap && m->capkind == d) {
        trim(m->cap);
        if (m->kind[d] == K_HREF) {
            char tmp[PHEDAV_HREF_MAX];
            phedav_href_norm(m->cap, tmp, sizeof tmp);
            copy_str(m->cap, m->capcap, tmp);
        }
        m->cap = NULL;
    }
    if (m->kind[d] == K_STATUS && m->in_status) {
        const char *sp = strchr(m->stbuf, ' ');
        int code = sp ? atoi(sp + 1) : 0;
        m->in_status = 0;
        /* in a propstat the status applies to the propstat; directly under a response it is the response status */
        if (d >= 2 && m->kind[d - 1] == K_PROPSTAT) m->ps_status = code;
        else m->rec.status = code;
    } else if (!strcmp(name, "propstat") && m->kind[d] == K_PROPSTAT) {
        if (m->ps_status == 0 || (m->ps_status >= 200 && m->ps_status < 300)) {
            phedav_resource *r = &m->rec;
            merge_field(r->etag, m->tmp.etag, sizeof r->etag);
            merge_field(r->ctag, m->tmp.ctag, sizeof r->ctag);
            merge_field(r->sync_token, m->tmp.sync_token, sizeof r->sync_token);
            merge_field(r->name, m->tmp.name, sizeof r->name);
            merge_field(r->principal, m->tmp.principal, sizeof r->principal);
            merge_field(r->home, m->tmp.home, sizeof r->home);
            r->flags |= m->tmp.flags;
        }
        m->in_ps = 0;
        m->ps_status = 0;
    } else if (m->kind[d] == K_RESPONSE) {
        m->in_resp = 0;
        if (c->item_len && !c->item_over) m->rec.flags |= PHEDAV_R_HASDATA;
        if (!m->aborted && m->cb) {
            int r = m->cb(c, m->cbctx, &m->rec);
            if (r) m->aborted = r;
        }
    }
}

static int ms_sink(void *ctx, const void *data, size_t n)
{
    phedav *c = (phedav *)ctx;
    int r;
    if (c->ms.aborted) return c->ms.aborted;
    r = phedav_xml_feed(&c->xml, data, n);
    if (r) return PHEDAV_ERR_PROTO;
    return c->ms.aborted;
}

int phedav_dav(phedav *c, const char *method, const char *target, int depth, const char *xml,
               phedav_resource_cb cb, void *cbctx, char *top_token)
{
    static const phedav_xml_handler h = { ms_start, ms_text, ms_end };
    char extra[96];
    int r;
    memset(&c->ms, 0, sizeof c->ms);
    c->ms.cb = cb;
    c->ms.cbctx = cbctx;
    c->ms.top_token = top_token;
    c->ms.owner = c;
    if (top_token) top_token[0] = 0;
    phedav_xml_init(&c->xml, &h, c);
    snprintf(extra, sizeof extra, "Depth: %d\r\nContent-Type: application/xml; charset=utf-8\r\n", depth);
    r = phedav_request(c, method, target, extra, xml, strlen(xml), ms_sink, c);
    if (r) return r;
    if (c->ms.aborted) return c->ms.aborted;
    if (c->status == 207 && phedav_xml_finish(&c->xml)) return PHEDAV_ERR_PROTO;
    return PHEDAV_OK;
}

/* ------------------------------------------------------------------------------------------------ discovery */

static const char X_HEAD[] = "<?xml version=\"1.0\" encoding=\"utf-8\"?>";

typedef struct {
    char principal[PHEDAV_HREF_MAX];
    char home[PHEDAV_HREF_MAX];
} disc_state;

static int cb_principal(phedav *c, void *ctx, const phedav_resource *r)
{
    disc_state *d = (disc_state *)ctx;
    (void)c;
    if (r->home[0] && !d->home[0]) copy_str(d->home, sizeof d->home, r->home);
    if (r->principal[0] && !d->principal[0]) copy_str(d->principal, sizeof d->principal, r->principal);
    return 0;
}

typedef struct {
    int kind;
    const char *home;
    phedav_collection_cb cb;
    void *cbctx;
    int found;
} list_state;

static int href_eq(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la && a[la - 1] == '/') la--;
    if (lb && b[lb - 1] == '/') lb--;
    return la == lb && !memcmp(a, b, la);
}

static int cb_colls(phedav *c, void *ctx, const phedav_resource *r)
{
    list_state *s = (list_state *)ctx;
    phedav_collection col;
    unsigned want = (s->kind == PHEDAV_CAL) ? PHEDAV_R_CALENDAR : PHEDAV_R_ADDRESSBOOK;
    (void)c;
    if (!(r->flags & want) || href_eq(r->href, s->home)) return 0;
    memset(&col, 0, sizeof col);
    copy_str(col.href, sizeof col.href, r->href);
    copy_str(col.name, sizeof col.name, r->name);
    copy_str(col.ctag, sizeof col.ctag, r->ctag);
    copy_str(col.sync_token, sizeof col.sync_token, r->sync_token);
    col.kind = s->kind;
    col.comps = r->flags & (PHEDAV_R_VEVENT | PHEDAV_R_VTODO | PHEDAV_R_VJOURNAL);
    s->found++;
    return s->cb ? s->cb(s->cbctx, &col) : 0;
}

int phedav_discover(phedav *c, int kind, const char *url, char *home_out, phedav_collection_cb cb, void *cbctx)
{
    char path[PHEDAV_HREF_MAX], cand[3][PHEDAV_HREF_MAX], xml[900];
    int ncand = 0, i, r;
    disc_state d;
    list_state ls;
    const char *wk = (kind == PHEDAV_CAL) ? "/.well-known/caldav" : "/.well-known/carddav";
    const char *ns = (kind == PHEDAV_CAL) ? NS_CAL : NS_CARD;
    const char *hs = (kind == PHEDAV_CAL) ? "calendar-home-set" : "addressbook-home-set";

    if (kind != PHEDAV_CAL && kind != PHEDAV_CARD) return PHEDAV_ERR_ARG;
    if ((r = phedav_set_origin(c, url, path, sizeof path))) return r;
    if (strcmp(path, "/") != 0) copy_str(cand[ncand++], PHEDAV_HREF_MAX, path);
    copy_str(cand[ncand++], PHEDAV_HREF_MAX, wk);
    copy_str(cand[ncand++], PHEDAV_HREF_MAX, "/");

    snprintf(xml, sizeof xml, "%s<d:propfind xmlns:d=\"DAV:\" xmlns:c=\"%s\"><d:prop><d:current-user-principal/><c:%s/></d:prop></d:propfind>",
             X_HEAD, ns, hs);
    memset(&d, 0, sizeof d);
    for (i = 0; i < ncand && !d.home[0] && !d.principal[0]; i++) {
        r = phedav_dav(c, "PROPFIND", cand[i], 0, xml, cb_principal, &d, NULL);
        if (r == PHEDAV_ERR_AUTH) return r;
        if (r < 0 && r != PHEDAV_ERR_HTTP) { if (r == PHEDAV_ERR_IO) return r; continue; }
    }
    if (!d.home[0] && d.principal[0]) {
        r = phedav_dav(c, "PROPFIND", d.principal, 0, xml, cb_principal, &d, NULL);
        if (r) return r;
    }
    if (!d.home[0]) return PHEDAV_ERR_DISCOVERY;
    if (home_out) copy_str(home_out, PHEDAV_HREF_MAX, d.home);

    snprintf(xml, sizeof xml,
             "%s<d:propfind xmlns:d=\"DAV:\" xmlns:c=\"%s\" xmlns:cs=\"" NS_CS "\"><d:prop><d:resourcetype/><d:displayname/>"
             "<cs:getctag/><d:sync-token/>%s</d:prop></d:propfind>",
             X_HEAD, ns, kind == PHEDAV_CAL ? "<c:supported-calendar-component-set/>" : "");
    ls.kind = kind; ls.home = d.home; ls.cb = cb; ls.cbctx = cbctx; ls.found = 0;
    r = phedav_dav(c, "PROPFIND", d.home, 1, xml, cb_colls, &ls, NULL);
    if (r) return r;
    if (c->status != 207) return PHEDAV_ERR_HTTP;
    return ls.found;
}

/* ------------------------------------------------------------------------------------------------ helpers */

size_t phedav_item_uid(const char *text, size_t n, char *out, size_t cap)
{
    size_t i = 0, o = 0;
    if (!cap) return 0;
    out[0] = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && text[e] != '\n') e++;
        if (e - i >= 4 && (text[i] == 'U' || text[i] == 'u') && (text[i + 1] == 'I' || text[i + 1] == 'i') &&
            (text[i + 2] == 'D' || text[i + 2] == 'd') && (text[i + 3] == ':' || text[i + 3] == ';')) {
            size_t p = i + 3;
            while (p < e && text[p] != ':') p++;
            p++;
            for (;;) {
                size_t q = (e > p && text[e - 1] == '\r') ? e - 1 : e;
                for (; p < q && o + 1 < cap; p++) out[o++] = text[p];
                /* unfold: a following line that starts with space or tab continues the value */
                if (e + 1 < n && (text[e + 1] == ' ' || text[e + 1] == '\t')) {
                    p = e + 2;
                    e = p;
                    while (e < n && text[e] != '\n') e++;
                    continue;
                }
                break;
            }
            out[o] = 0;
            return o;
        }
        i = e + 1;
    }
    return 0;
}
