/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

/*
 * Two-way sync of one collection. See README.md ("The sync loop") for the order of operations:
 *   1. gather the local changes (host callback) into the arena
 *   2. list the server: sync-collection (incremental, or full with an empty token), else ctag + etag listing
 *   3. reconcile each server change against the local changes (conflict policy), apply deletions locally, queue downloads
 *   4. fetch the queued items with multiget (or GET) and hand them to the local database
 *   5. upload the remaining local changes (PUT If-None-Match / If-Match, DELETE If-Match)
 *   6. persist the new token / ctag if everything succeeded
 */
#include "phedav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------------ arena lists */

typedef struct { uint16_t total; uint8_t a, b; } rec_hdr;     /* server rec: a = deleted, b = fetch (1 plain, 2 restore) ; local: a = kind, b = handled */

typedef struct {
    size_t off, end;
    int n;
} list;

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* appends a record "hdr, s1\0, s2\0" at the end of the arena (which must be the end of `l`); 0 or PHEDAV_ERR_BUF */
static int list_add(phedav *c, list *l, int a, int b, const char *s1, const char *s2)
{
    size_t n1 = strlen(s1) + 1, n2 = s2 ? strlen(s2) + 1 : 0;
    size_t total = (sizeof(rec_hdr) + n1 + n2 + 3) & ~(size_t)3;
    rec_hdr h;
    unsigned char *p;
    if (c->work_used + total > c->work_cap || total > 0xFFFF) return PHEDAV_ERR_BUF;
    p = c->work + c->work_used;
    h.total = (uint16_t)total; h.a = (uint8_t)a; h.b = (uint8_t)b;
    memcpy(p, &h, sizeof h);
    memcpy(p + sizeof h, s1, n1);
    if (s2) memcpy(p + sizeof h + n1, s2, n2);
    c->work_used += total;
    l->end = c->work_used;
    l->n++;
    return 0;
}

typedef struct { unsigned char *p; rec_hdr h; const char *s1, *s2; } cur;

static int list_next(phedav *c, const list *l, size_t *pos, cur *r)
{
    if (*pos >= l->end) return 0;
    r->p = c->work + *pos;
    memcpy(&r->h, r->p, sizeof r->h);
    r->s1 = (const char *)r->p + sizeof r->h;
    r->s2 = r->s1 + strlen(r->s1) + 1;
    *pos += r->h.total;
    return 1;
}

static void cur_set(cur *r, int a, int b)
{
    r->h.a = (uint8_t)a; r->h.b = (uint8_t)b;
    memcpy(r->p, &r->h, sizeof r->h);
}

/* ------------------------------------------------------------------------------------------------ context */

typedef struct {
    phedav *c;
    const phedav_collection *col;
    phedav_store *st;
    phedav_local *lo;
    phedav_stats *stats;
    const phedav_syncopts *opt;
    list loc, srv;
    int err;                      /* first fatal error from a callback */
    char new_ctag[PHEDAV_ETAG_MAX];
    int is_fetch_mg;              /* multiget: count responses */
    int mg_seen;
} sctx;

static int href_eq(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la && a[la - 1] == '/') la--;
    if (lb && b[lb - 1] == '/') lb--;
    return la == lb && !memcmp(a, b, la);
}

/* ------------------------------------------------------------------------------------------------ XML bodies */

static size_t xml_esc(char *dst, size_t cap, const char *s)
{
    size_t o = 0;
    for (; *s; s++) {
        const char *r = NULL;
        size_t rl = 1;
        char one[2];
        switch (*s) {
        case '&': r = "&amp;"; rl = 5; break;
        case '<': r = "&lt;"; rl = 4; break;
        case '>': r = "&gt;"; rl = 4; break;
        default: one[0] = *s; one[1] = 0; r = one; break;
        }
        if (o + rl + 1 > cap) return (size_t)-1;
        memcpy(dst + o, r, rl);
        o += rl;
    }
    dst[o] = 0;
    return o;
}

static const char XH[] = "<?xml version=\"1.0\" encoding=\"utf-8\"?>";

/* ------------------------------------------------------------------------------------------------ server listing */

static int cb_list(phedav *c, void *ctx, const phedav_resource *r)
{
    sctx *s = (sctx *)ctx;
    (void)c;
    if (href_eq(r->href, s->col->href)) {       /* the collection itself */
        if (r->ctag[0]) copy_str(s->new_ctag, sizeof s->new_ctag, r->ctag);
        return 0;
    }
    if (r->status == 404 || r->status == 410) {
        if (list_add(c, &s->srv, 1, 0, r->href, "")) { s->err = PHEDAV_ERR_BUF; return PHEDAV_ERR_BUF; }
        return 0;
    }
    if (r->flags & PHEDAV_R_COLLECTION) return 0;
    if (r->status && (r->status < 200 || r->status >= 300)) return 0;
    if (list_add(c, &s->srv, 0, 0, r->href, r->etag)) { s->err = PHEDAV_ERR_BUF; return PHEDAV_ERR_BUF; }
    return 0;
}

/* returns 1 = ok (207), 0 = refused (expired token / unsupported), < 0 = hard error */
static int report_sync(sctx *s, const char *token, char *new_token)
{
    phedav *c = s->c;
    char *b = c->body;
    size_t o;
    int r;
    o = (size_t)snprintf(b, PHEDAV_BODY_MAX, "%s<d:sync-collection xmlns:d=\"DAV:\">", XH);
    if (token[0]) {
        o += (size_t)snprintf(b + o, PHEDAV_BODY_MAX - o, "<d:sync-token>");
        o += xml_esc(b + o, PHEDAV_BODY_MAX - o - 200, token);
        o += (size_t)snprintf(b + o, PHEDAV_BODY_MAX - o, "</d:sync-token>");
    } else o += (size_t)snprintf(b + o, PHEDAV_BODY_MAX - o, "<d:sync-token/>");
    snprintf(b + o, PHEDAV_BODY_MAX - o, "<d:sync-level>1</d:sync-level><d:prop><d:getetag/></d:prop></d:sync-collection>");
    s->srv.n = 0; s->srv.end = s->srv.off; s->c->work_used = s->srv.off;
    r = phedav_dav(c, "REPORT", s->col->href, 0, b, cb_list, s, new_token);
    if (r == PHEDAV_ERR_AUTH || r == PHEDAV_ERR_IO || r == PHEDAV_ERR_REDIRECT) return r;
    if (s->err) return s->err;
    if (r == 0 && c->status == 207) return 1;
    s->srv.n = 0; s->srv.end = s->srv.off; s->c->work_used = s->srv.off;
    return 0;
}

static int propfind_ctag(sctx *s)
{
    static const char body[] = "<?xml version=\"1.0\" encoding=\"utf-8\"?><d:propfind xmlns:d=\"DAV:\" xmlns:cs=\"http://calendarserver.org/ns/\">"
                               "<d:prop><cs:getctag/><d:getetag/></d:prop></d:propfind>";
    int r;
    s->new_ctag[0] = 0;
    r = phedav_dav(s->c, "PROPFIND", s->col->href, 0, body, cb_list, s, NULL);
    s->srv.n = 0; s->srv.end = s->srv.off; s->c->work_used = s->srv.off;
    if (r || s->c->status != 207) return r < 0 ? r : 0;
    return 1;
}

static int propfind_listing(sctx *s)
{
    static const char body[] = "<?xml version=\"1.0\" encoding=\"utf-8\"?><d:propfind xmlns:d=\"DAV:\" xmlns:cs=\"http://calendarserver.org/ns/\">"
                               "<d:prop><d:getetag/><cs:getctag/><d:resourcetype/></d:prop></d:propfind>";
    int r;
    s->new_ctag[0] = 0;
    s->srv.n = 0; s->srv.end = s->srv.off; s->c->work_used = s->srv.off;
    r = phedav_dav(s->c, "PROPFIND", s->col->href, 1, body, cb_list, s, NULL);
    if (r < 0) return r;
    if (s->err) return s->err;
    return s->c->status == 207 ? 1 : PHEDAV_ERR_HTTP;
}

/* ------------------------------------------------------------------------------------------------ local changes */

static int cb_local(void *ctx, const char *id, int kind)
{
    sctx *s = (sctx *)ctx;
    if (list_add(s->c, &s->loc, kind, 0, id, NULL)) { s->err = PHEDAV_ERR_BUF; return 1; }
    return 0;
}

/* finds the local change for an id */
static int find_chg(sctx *s, const char *id, cur *out)
{
    size_t pos = s->loc.off;
    cur r;
    while (list_next(s->c, &s->loc, &pos, &r)) {
        if (!strcmp(r.s1, id) && r.h.b == 0) { *out = r; return 1; }
    }
    return 0;
}

static int decide(sctx *s, const char *href, const char *local_id, int local_kind, int server_deleted)
{
    int pol = s->opt ? s->opt->conflict_policy : PHEDAV_USE_SERVER;
    if (s->lo->conflict) pol = s->lo->conflict(s->lo->ctx, s->col->href, href, local_id, local_kind, server_deleted);
    s->stats->conflicts++;
    return pol == PHEDAV_USE_LOCAL ? PHEDAV_USE_LOCAL : PHEDAV_USE_SERVER;
}

/* ------------------------------------------------------------------------------------------------ reconcile */

typedef struct { sctx *s; int found_in_srv; const char *href; } probe;

static int cb_mark_missing(void *ctx, const phedav_item_state *it)
{
    sctx *s = (sctx *)ctx;
    size_t pos = s->srv.off;
    cur r;
    list scan = s->srv;           /* scanning includes records appended earlier by this callback; they are flagged deleted */
    while (list_next(s->c, &scan, &pos, &r)) {
        if (!r.h.a && !strcmp(r.s1, it->href)) return 0;
    }
    /* not on the server any more: queue as deleted (appended after the scanned part, beyond `end`) */
    if (list_add(s->c, &s->srv, 1, 0, it->href, "")) { s->err = PHEDAV_ERR_BUF; return 1; }
    return 0;
}

static int reconcile(sctx *s, int complete)
{
    phedav *c = s->c;
    size_t pos;
    cur r;
    int e;
    if (complete) {
        /* deletion detection: stored items that the complete listing does not contain. The scan must see only the
         * original listing, so remember its end and let the deleted ones follow it. */
        if (s->st->item_each(s->st->ctx, s->col->href, cb_mark_missing, s) < 0) return PHEDAV_ERR_STORE;
        if (s->err) return s->err;
    }
    pos = s->srv.off;
    while (list_next(c, &s->srv, &pos, &r)) {
        phedav_item_state it;
        int found = s->st->item_by_href(s->st->ctx, s->col->href, r.s1, &it);
        cur ch;
        if (found < 0) return PHEDAV_ERR_STORE;
        if (r.h.a) {                                    /* deleted on the server */
            int have_ch;
            if (!found) continue;
            have_ch = find_chg(s, it.local_id, &ch);
            if (have_ch && ch.h.a == PHEDAV_CH_MOD && decide(s, r.s1, it.local_id, ch.h.a, 1) == PHEDAV_USE_LOCAL) {
                s->st->item_del(s->st->ctx, s->col->href, r.s1);      /* forget; the local version is uploaded as new */
                cur_set(&ch, PHEDAV_CH_ADD, 0);
                continue;
            }
            if (have_ch && ch.h.a == PHEDAV_CH_DEL) {                 /* deleted on both sides */
                s->lo->ack(s->lo->ctx, s->col->href, it.local_id);
                cur_set(&ch, ch.h.a, 1);
                s->st->item_del(s->st->ctx, s->col->href, r.s1);
                s->stats->down_del++;
                continue;
            }
            if (have_ch) { s->lo->ack(s->lo->ctx, s->col->href, it.local_id); cur_set(&ch, ch.h.a, 1); }
            if (s->lo->remove(s->lo->ctx, s->col->href, it.local_id) < 0) { s->stats->errors++; continue; }
            s->st->item_del(s->st->ctx, s->col->href, r.s1);
            s->stats->down_del++;
            continue;
        }
        if (found && r.s2[0] && !strcmp(it.etag, r.s2)) continue;      /* unchanged (our own upload, or already fetched) */
        if (found && find_chg(s, it.local_id, &ch)) {
            if (decide(s, r.s1, it.local_id, ch.h.a, 0) == PHEDAV_USE_LOCAL) {
                /* keep the local version: adopt the server's etag so that the upload (If-Match) overwrites it */
                copy_str(it.etag, sizeof it.etag, r.s2);
                if (ch.h.a == PHEDAV_CH_DEL) { /* deleted locally: DELETE with the new etag */ }
                if (s->st->item_put(s->st->ctx, s->col->href, &it) < 0) return PHEDAV_ERR_STORE;
                continue;
            }
            /* server wins: the local change is dropped */
            s->lo->ack(s->lo->ctx, s->col->href, it.local_id);
            cur_set(&ch, ch.h.a, 1);
            cur_set(&r, 0, ch.h.a == PHEDAV_CH_DEL ? 2 : 1);
            continue;
        }
        cur_set(&r, 0, 1);
    }
    e = 0;
    return e;
}

/* ------------------------------------------------------------------------------------------------ fetch */

static int apply_item(sctx *s, const phedav_resource *r, const char *data, size_t len)
{
    phedav *c = s->c;
    phedav_item_state it;
    int found, restore = 0;
    char newid[PHEDAV_ID_MAX];
    const char *etag = r->etag;
    size_t pos = s->srv.off;
    cur q;
    memset(&it, 0, sizeof it);
    found = s->st->item_by_href(s->st->ctx, s->col->href, r->href, &it);
    while (list_next(c, &s->srv, &pos, &q)) {
        if (!strcmp(q.s1, r->href)) {
            if (q.h.b == 2) restore = 1;
            if (!etag[0]) etag = q.s2;
            break;
        }
    }
    newid[0] = 0;
    if (s->lo->write(s->lo->ctx, s->col->href, (found > 0 && !restore) ? it.local_id : "", data, len, newid) < 0) {
        s->stats->errors++;
        return 0;
    }
    if (found > 0 && !restore && !newid[0]) copy_str(newid, sizeof newid, it.local_id);
    copy_str(it.href, sizeof it.href, r->href);
    copy_str(it.etag, sizeof it.etag, etag);
    copy_str(it.local_id, sizeof it.local_id, newid);
    if (s->st->item_put(s->st->ctx, s->col->href, &it) < 0) { s->err = PHEDAV_ERR_STORE; return PHEDAV_ERR_STORE; }
    if (found > 0) s->stats->down_mod++; else s->stats->down_new++;
    return 0;
}

static int cb_multiget(phedav *c, void *ctx, const phedav_resource *r)
{
    sctx *s = (sctx *)ctx;
    if (r->status == 404 || !r->href[0]) return 0;
    s->mg_seen++;
    if (!(r->flags & PHEDAV_R_HASDATA)) { s->stats->errors++; return 0; }    /* too large for the item buffer, or no data */
    return apply_item(s, r, c->item, c->item_len);
}

typedef struct { phedav *c; } getctx;

static int sink_get(void *ctx, const void *data, size_t n)
{
    phedav *c = (phedav *)ctx;
    if (c->item_len + n > c->item_cap) { c->item_over = 1; return 0; }
    memcpy(c->item + c->item_len, data, n);
    c->item_len += n;
    return 0;
}

static int fetch_all(sctx *s)
{
    phedav *c = s->c;
    int cal = s->col->kind == PHEDAV_CAL;
    size_t pos = s->srv.off;
    cur r;
    for (;;) {
        char *b = c->body;
        size_t o, batch_pos = pos;
        int n = 0, requested = 0;
        const char *tag = cal ? "calendar" : "addressbook";
        const char *dtag = cal ? "calendar-data" : "address-data";
        const char *ns = cal ? "urn:ietf:params:xml:ns:caldav" : "urn:ietf:params:xml:ns:carddav";
        int rr;
        o = (size_t)snprintf(b, PHEDAV_BODY_MAX, "%s<c:%s-multiget xmlns:d=\"DAV:\" xmlns:c=\"%s\"><d:prop><d:getetag/><c:%s/></d:prop>",
                             XH, tag, ns, dtag);
        while (n < PHEDAV_MULTIGET_BATCH && list_next(c, &s->srv, &pos, &r)) {
            size_t k;
            if (r.h.a || !r.h.b) continue;
            if (o + strlen(r.s1) * 5 + 64 + 64 > PHEDAV_BODY_MAX) { pos = (size_t)((unsigned char *)r.p - c->work); break; }
            o += (size_t)snprintf(b + o, PHEDAV_BODY_MAX - o, "<d:href>");
            k = xml_esc(b + o, PHEDAV_BODY_MAX - o - 64, r.s1);
            if (k == (size_t)-1) return PHEDAV_ERR_BUF;
            o += k;
            o += (size_t)snprintf(b + o, PHEDAV_BODY_MAX - o, "</d:href>");
            n++;
        }
        (void)batch_pos;
        if (!n) return 0;
        snprintf(b + o, PHEDAV_BODY_MAX - o, "</c:%s-multiget>", tag);
        requested = n;
        s->mg_seen = 0;
        rr = phedav_dav(c, "REPORT", s->col->href, 1, b, cb_multiget, s, NULL);
        if (rr == PHEDAV_ERR_AUTH || rr == PHEDAV_ERR_IO) return rr;
        if (s->err) return s->err;
        if (rr == 0 && c->status == 207) {
            if (s->mg_seen < requested) s->stats->errors += requested - s->mg_seen;
        } else {
            /* no multiget: GET each href of this batch */
            size_t p2 = pos;
            cur q;
            int back = requested;
            /* walk back: re-scan from the start of the queue for entries not yet stored with their server etag */
            p2 = s->srv.off;
            while (back > 0 && list_next(c, &s->srv, &p2, &q)) {
                phedav_item_state it;
                phedav_resource res;
                if (q.h.a || !q.h.b) continue;
                if (s->st->item_by_href(s->st->ctx, s->col->href, q.s1, &it) > 0 && !strcmp(it.etag, q.s2)) continue;
                memset(&res, 0, sizeof res);
                copy_str(res.href, sizeof res.href, q.s1);
                c->item_len = 0; c->item_over = 0;
                rr = phedav_request(c, "GET", q.s1, NULL, NULL, 0, sink_get, c);
                if (rr == PHEDAV_ERR_AUTH || rr == PHEDAV_ERR_IO) return rr;
                if (rr || c->status != 200 || c->item_over) { s->stats->errors++; back--; continue; }
                copy_str(res.etag, sizeof res.etag, c->etag[0] ? c->etag : q.s2);
                if (apply_item(s, &res, c->item, c->item_len)) return s->err;
                back--;
            }
            return 0;      /* the whole queue was fetched with GET */
        }
    }
}

/* ------------------------------------------------------------------------------------------------ upload */

static void make_href(const phedav_collection *col, const char *local_id, const char *data, size_t len, char *out, size_t cap)
{
    char uid[80], name[96];
    size_t i, o = 0, n = phedav_item_uid(data, len, uid, sizeof uid);
    const char *ext = col->kind == PHEDAV_CAL ? ".ics" : ".vcf";
    size_t hl = strlen(col->href);
    if (n == 0) { snprintf(uid, sizeof uid, "phe-%s", local_id); n = strlen(uid); }
    for (i = 0; i < n && o + 1 < sizeof name; i++) {
        char ch = uid[i];
        int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '@';
        name[o++] = ok ? ch : '_';
    }
    name[o] = 0;
    if (hl + 100 > cap) hl = cap - 100;       /* a collection href this long is cut: the PUT will then fail cleanly */
    snprintf(out, cap, "%.*s%s%s%s", (int)hl, col->href, (hl && col->href[hl - 1] == '/') ? "" : "/", name, ext);
}

static int upload_all(sctx *s)
{
    phedav *c = s->c;
    size_t pos = s->loc.off;
    cur r;
    const char *ctype = s->col->kind == PHEDAV_CAL ? "text/calendar; charset=utf-8" : "text/vcard; charset=utf-8";
    while (list_next(c, &s->loc, &pos, &r)) {
        phedav_item_state it;
        int found, rr, kind = r.h.a;
        char extra[PHEDAV_ETAG_MAX + 120];
        if (r.h.b) continue;
        found = s->st->item_by_local(s->st->ctx, s->col->href, r.s1, &it);
        if (found < 0) return PHEDAV_ERR_STORE;
        if (kind == PHEDAV_CH_DEL) {
            if (!found) { s->lo->ack(s->lo->ctx, s->col->href, r.s1); continue; }
            if (it.etag[0]) snprintf(extra, sizeof extra, "If-Match: %s\r\n", it.etag); else extra[0] = 0;
            rr = phedav_request(c, "DELETE", it.href, extra, NULL, 0, NULL, NULL);
            if (rr == PHEDAV_ERR_AUTH || rr == PHEDAV_ERR_IO) return rr;
            if (rr == 0 && (c->status == 200 || c->status == 204 || c->status == 404)) {
                s->st->item_del(s->st->ctx, s->col->href, it.href);
                s->lo->ack(s->lo->ctx, s->col->href, r.s1);
                s->stats->up_del++;
            } else if (rr == 0 && c->status == 412) s->stats->deferred++;
            else s->stats->errors++;
            continue;
        }
        {
            size_t len = 0;
            char href[PHEDAV_HREF_MAX];
            int is_new = (kind == PHEDAV_CH_ADD) || !found;
            if (s->lo->read(s->lo->ctx, s->col->href, r.s1, c->item, c->item_cap, &len) < 0 || len > c->item_cap) {
                s->stats->errors++;
                continue;
            }
            if (is_new) {
                make_href(s->col, r.s1, c->item, len, href, sizeof href);
                snprintf(extra, sizeof extra, "Content-Type: %s\r\nIf-None-Match: *\r\n", ctype);
            } else {
                copy_str(href, sizeof href, it.href);
                if (it.etag[0]) snprintf(extra, sizeof extra, "Content-Type: %s\r\nIf-Match: %s\r\n", ctype, it.etag);
                else snprintf(extra, sizeof extra, "Content-Type: %s\r\n", ctype);
            }
            rr = phedav_request(c, "PUT", href, extra, c->item, len, NULL, NULL);
            if (rr == PHEDAV_ERR_AUTH || rr == PHEDAV_ERR_IO) return rr;
            if (rr == 0 && (c->status == 200 || c->status == 201 || c->status == 204)) {
                phedav_item_state ns;
                memset(&ns, 0, sizeof ns);
                copy_str(ns.href, sizeof ns.href, href);
                copy_str(ns.etag, sizeof ns.etag, c->etag);
                copy_str(ns.local_id, sizeof ns.local_id, r.s1);
                if (s->st->item_put(s->st->ctx, s->col->href, &ns) < 0) return PHEDAV_ERR_STORE;
                s->lo->ack(s->lo->ctx, s->col->href, r.s1);
                if (is_new) s->stats->up_new++; else s->stats->up_mod++;
            } else if (rr == 0 && c->status == 412) s->stats->deferred++;
            else s->stats->errors++;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------ the sync */

int phedav_sync(phedav *c, const phedav_collection *col, phedav_store *st, phedav_local *lo,
                const phedav_syncopts *opt, phedav_stats *stats)
{
    sctx s;
    phedav_meta meta, nm;
    phedav_stats local_stats;
    char new_token[PHEDAV_TOKEN_MAX];
    int r, have = 0, complete = 0, unchanged = 0, use_sc, forced;
    unsigned req0 = c->requests;
    phedav_syncopts dopt;

    if (!stats) stats = &local_stats;
    memset(stats, 0, sizeof *stats);
    stats->mode = "none";
    if (!opt) { memset(&dopt, 0, sizeof dopt); opt = &dopt; }
    if (!c->work || !c->item || !st || !lo || !col) return PHEDAV_ERR_ARG;
    memset(&s, 0, sizeof s);
    s.c = c; s.col = col; s.st = st; s.lo = lo; s.stats = stats; s.opt = opt;
    memset(&meta, 0, sizeof meta);
    if (st->meta_get(st->ctx, col->href, &meta) < 0) return PHEDAV_ERR_STORE;
    forced = opt->force_full;
    use_sc = !opt->no_sync_collection;
    new_token[0] = 0;

    c->work_used = 0;
    s.loc.off = s.loc.end = c->work_used;
    if (lo->changes(lo->ctx, col->href, cb_local, &s) < 0) return PHEDAV_ERR_STORE;
    if (s.err) return s.err;
    s.srv.off = s.srv.end = c->work_used;

    if (use_sc && !forced && meta.sync_token[0]) {
        r = report_sync(&s, meta.sync_token, new_token);
        if (r < 0) return r;
        if (r == 1) { have = 1; stats->mode = "sync-collection"; }
        else { stats->token_expired = 1; meta.sync_token[0] = 0; }
    }
    if (!have && use_sc) {
        r = report_sync(&s, "", new_token);
        if (r < 0) return r;
        if (r == 1) { have = 1; complete = 1; stats->mode = stats->token_expired ? "full (token expired)" : "full"; }
    }
    if (!have) {
        new_token[0] = 0;
        if (!forced && meta.ctag[0]) {
            r = propfind_ctag(&s);
            if (r < 0) return r;
            if (r == 1 && s.new_ctag[0] && !strcmp(s.new_ctag, meta.ctag)) { unchanged = 1; stats->mode = "unchanged"; }
        }
        if (!unchanged) {
            r = propfind_listing(&s);
            if (r < 0) return r;
            have = 1; complete = 1; stats->mode = "listing";
        }
    }

    if (have) {
        if ((r = reconcile(&s, complete))) return r;
        if ((r = fetch_all(&s))) return r;
    }
    if (!opt->download_only && (r = upload_all(&s))) return r;

    /* persist: only when every item went through, so a failed one is retried (items already stored are skipped by etag) */
    if (!stats->errors) {
        memset(&nm, 0, sizeof nm);
        if (unchanged) nm = meta;
        else if (have && stats->mode[0] == 'f' && new_token[0]) copy_str(nm.sync_token, sizeof nm.sync_token, new_token);
        else if (have && !strcmp(stats->mode, "sync-collection") && new_token[0]) copy_str(nm.sync_token, sizeof nm.sync_token, new_token);
        else if (have && !strcmp(stats->mode, "sync-collection")) copy_str(nm.sync_token, sizeof nm.sync_token, meta.sync_token);
        if (!nm.sync_token[0] || !use_sc) copy_str(nm.ctag, sizeof nm.ctag, unchanged ? meta.ctag : s.new_ctag);
        if (st->meta_set(st->ctx, col->href, &nm) < 0) return PHEDAV_ERR_STORE;
    }
    stats->http_requests = (int)(c->requests - req0);
    return PHEDAV_OK;
}
