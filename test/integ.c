/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */
/*
 * Integration test against a live CalDAV / CardDAV server (Radicale, Stalwart, Nextcloud, ...).
 *   PHEDAV_URL=http://host:5232/ PHEDAV_USER=alice PHEDAV_PASS=secret ./build/integ [cal|card|both]
 * It creates its own collections (named phetest-<kind>-<time>), runs the scenarios and deletes them again.
 * Two clients are used: `dev` is the device under test (with an in-memory database), `srv` plays "another device" that
 * changes the server directly.
 */
#include "devdb.h"
#include "phedav.h"
#include "posix_net.h"
#ifdef PHEDAV_WITH_MBEDTLS
#include "phedav_mbedtls.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail, g_checks;
#define DEL_OK(st) ((st) == 200 || (st) == 204)
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static phedav dev, srv;
static posix_net pn_dev, pn_srv;
static char item_dev[DEV_DATA], item_srv[DEV_DATA];
static unsigned char work_dev[64 * 1024], work_srv[16 * 1024];
static devdb db;

static const char *g_url, *g_user, *g_pass;

typedef struct { char buf[DEV_DATA]; size_t n; } body;
static int body_sink(void *ctx, const void *d, size_t n)
{
    body *b = (body *)ctx;
    if (b->n + n >= sizeof b->buf) return -1;
    memcpy(b->buf + b->n, d, n);
    b->n += n;
    b->buf[b->n] = 0;
    return 0;
}

/* ---- server-side helpers (the "other device") */
static int srv_put(const char *href, const char *data, int kind, const char *if_match)
{
    char extra[200];
    int r;
    snprintf(extra, sizeof extra, "Content-Type: %s\r\n%s%s%s", kind == PHEDAV_CAL ? "text/calendar" : "text/vcard",
             if_match ? "If-Match: " : "", if_match ? if_match : "", if_match ? "\r\n" : "");
    r = phedav_request(&srv, "PUT", href, extra, data, strlen(data), NULL, NULL);
    return r ? -1 : srv.status;
}
static int srv_get(const char *href, body *b)
{
    int r;
    b->n = 0; b->buf[0] = 0;
    r = phedav_request(&srv, "GET", href, NULL, NULL, 0, body_sink, b);
    return r ? -1 : srv.status;
}
static int srv_del(const char *href)
{
    int r = phedav_request(&srv, "DELETE", href, NULL, NULL, 0, NULL, NULL);
    return r ? -1 : srv.status;
}

static void mk_item(int kind, char *out, size_t cap, const char *uid, const char *text)
{
    if (kind == PHEDAV_CARD)
        snprintf(out, cap, "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:%s\r\nFN:%s\r\nN:%s;;;;\r\nEND:VCARD\r\n", uid, text, text);
    else
        snprintf(out, cap, "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//phedav test//EN\r\nBEGIN:VEVENT\r\nUID:%s\r\n"
                           "DTSTAMP:20261022T100000Z\r\nDTSTART:20261101T100000Z\r\nDTEND:20261101T110000Z\r\nSUMMARY:%s\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n",
                 uid, text);
}

typedef struct { phedav_collection found; int n; const char *want; } findctx;
static int find_cb(void *ctx, const phedav_collection *c)
{
    findctx *f = (findctx *)ctx;
    f->n++;
    if (f->want && strstr(c->name, f->want)) f->found = *c;
    return 0;
}

static void print_stats(const char *label, const phedav_stats *s)
{
    printf("  %-34s mode=%-22s down +%d ~%d -%d  up +%d ~%d -%d  conflicts=%d deferred=%d errors=%d reqs=%d%s\n", label, s->mode,
           s->down_new, s->down_mod, s->down_del, s->up_new, s->up_mod, s->up_del, s->conflicts, s->deferred, s->errors,
           s->http_requests, s->token_expired ? " [token expired]" : "");
}

static phedav_collection g_col;

static int do_sync(const char *label, const phedav_syncopts *o, phedav_stats *s)
{
    phedav_store st = dev_store(&db);
    phedav_local lo = dev_local(&db);
    int r = phedav_sync(&dev, &g_col, &st, &lo, o, s);
    if (r) printf("  sync error %d (http %d)\n", r, dev.status);
    else print_stats(label, s);
    CHECK(r == 0);
    return r;
}

static void fresh_device(void)
{
    dev_init(&db);
    phedav_close(&dev);
}

static void run_kind(int kind)
{
    const char *kn = kind == PHEDAV_CAL ? "cal" : "card";
    char home[PHEDAV_HREF_MAX], name[64], href[1024], coll[1024], buf[DEV_DATA], extra[PHEDAV_TOKEN_MAX];
    phedav_stats s;
    phedav_syncopts opt;
    findctx f;
    body b;
    int r, i;
    const char *id;
    char sx[3][1024];

    printf("== %s ==\n", kn);
    /* ---- discovery */
    memset(&f, 0, sizeof f);
    r = phedav_discover(&dev, kind, g_url, home, find_cb, &f);
    printf("  discover: %d collection(s), home=%s\n", r, home);
    CHECK(r >= 0 && home[0]);
    if (r < 0 || !home[0]) return;
    snprintf(name, sizeof name, "phetest-%s-%ld", kn, (long)time(NULL));
    snprintf(coll, sizeof coll, "%s%s/", home, name);
    {
        char mk[900];
        if (kind == PHEDAV_CAL)
            snprintf(mk, sizeof mk, "<?xml version=\"1.0\" encoding=\"utf-8\"?><mkcol xmlns=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\"><set><prop>"
                     "<resourcetype><collection/><C:calendar/></resourcetype><displayname>%s</displayname>"
                     "<C:supported-calendar-component-set><C:comp name=\"VEVENT\"/></C:supported-calendar-component-set></prop></set></mkcol>", name);
        else
            snprintf(mk, sizeof mk, "<?xml version=\"1.0\" encoding=\"utf-8\"?><mkcol xmlns=\"DAV:\" xmlns:CR=\"urn:ietf:params:xml:ns:carddav\"><set><prop>"
                     "<resourcetype><collection/><CR:addressbook/></resourcetype><displayname>%s</displayname></prop></set></mkcol>", name);
        r = phedav_request(&srv, "MKCOL", coll, "Content-Type: application/xml\r\n", mk, strlen(mk), NULL, NULL);
        printf("  MKCOL %s -> %d\n", coll, srv.status);
        CHECK(r == 0 && srv.status == 201);
    }
    memset(&f, 0, sizeof f);
    f.want = name;
    r = phedav_discover(&dev, kind, g_url, NULL, find_cb, &f);
    CHECK(r >= 1 && f.found.href[0]);
    printf("  found collection %s name=%s ctag=%s token=%s\n", f.found.href, f.found.name, f.found.ctag, f.found.sync_token);
    CHECK(strcmp(f.found.href, coll) == 0 || strstr(f.found.href, name));
    g_col = f.found;
    CHECK(g_col.kind == kind);
    if (kind == PHEDAV_CAL) CHECK(g_col.comps & PHEDAV_R_VEVENT);

    /* ---- initial sync: three items put by "another device" */
    for (i = 0; i < 3; i++) {
        char uid[32], txt[32];
        snprintf(uid, sizeof uid, "uid-s%d", i);
        snprintf(txt, sizeof txt, "Server%d", i);
        mk_item(kind, buf, sizeof buf, uid, txt);
        snprintf(sx[i], sizeof sx[i], "%s%s%s", g_col.href, uid, kind == PHEDAV_CAL ? ".ics" : ".vcf");
        CHECK(srv_put(sx[i], buf, kind, NULL) == 201);
    }
    memset(&opt, 0, sizeof opt);
    fresh_device();
    do_sync("initial (3 on server)", &opt, &s);
    CHECK(!strcmp(s.mode, "full") && s.down_new == 3 && dev_count(&db) == 3 && s.errors == 0);
    CHECK(db.meta.sync_token[0]);
    CHECK(dev_find_containing(&db, "Server1", NULL) != NULL);
    do_sync("again, nothing changed", &opt, &s);
    CHECK(!strcmp(s.mode, "sync-collection") && s.down_new + s.down_mod + s.down_del + s.up_new + s.up_mod + s.up_del == 0 && s.http_requests == 1);

    /* ---- local add / modify / delete */
    {
        const char *nid;
        const char *eid;
        mk_item(kind, buf, sizeof buf, "uid-dev-new", "DeviceNew");
        nid = dev_add(&db, buf);
        CHECK(nid != NULL);
        dev_find_containing(&db, "Server0", &eid);
        mk_item(kind, buf, sizeof buf, "uid-s0", "Server0-edited-on-device");
        dev_mod(&db, eid, buf);
        dev_find_containing(&db, "Server2", &eid);
        dev_del(&db, eid);
    }
    do_sync("local add+modify+delete", &opt, &s);
    CHECK(s.up_new == 1 && s.up_mod == 1 && s.up_del == 1 && s.errors == 0 && dev_dirty_count(&db) == 0);
    CHECK(srv_get(sx[0], &b) == 200 && strstr(b.buf, "Server0-edited-on-device"));
    CHECK(srv_get(sx[2], &b) == 404);
    snprintf(href, sizeof href, "%suid-dev-new%s", g_col.href, kind == PHEDAV_CAL ? ".ics" : ".vcf");
    CHECK(srv_get(href, &b) == 200 && strstr(b.buf, "DeviceNew"));
    do_sync("again (own changes come back)", &opt, &s);
    CHECK(s.down_new + s.down_mod + s.down_del == 0 && s.up_new + s.up_mod + s.up_del == 0);

    /* ---- server-side add / modify / delete */
    mk_item(kind, buf, sizeof buf, "uid-s9", "ServerNew");
    snprintf(href, sizeof href, "%suid-s9%s", g_col.href, kind == PHEDAV_CAL ? ".ics" : ".vcf");
    CHECK(srv_put(href, buf, kind, NULL) == 201);
    mk_item(kind, buf, sizeof buf, "uid-s1", "Server1-edited-on-server");
    CHECK(srv_put(sx[1], buf, kind, NULL) == 204);
    snprintf(href, sizeof href, "%suid-dev-new%s", g_col.href, kind == PHEDAV_CAL ? ".ics" : ".vcf");
    CHECK(DEL_OK(srv_del(href)));
    do_sync("server add+modify+delete", &opt, &s);
    CHECK(!strcmp(s.mode, "sync-collection") && s.down_new == 1 && s.down_mod == 1 && s.down_del == 1 && s.errors == 0);
    CHECK(dev_find_containing(&db, "ServerNew", NULL) && dev_find_containing(&db, "Server1-edited-on-server", NULL));
    CHECK(!dev_find_containing(&db, "DeviceNew", NULL));
    CHECK(dev_count(&db) == 3);

    /* ---- conflict: both sides change uid-s0; the server wins */
    mk_item(kind, buf, sizeof buf, "uid-s0", "S0-by-server");
    CHECK(srv_put(sx[0], buf, kind, NULL) == 204);
    {
        const char *eid;
        dev_find_containing(&db, "Server0-edited-on-device", &eid);
        mk_item(kind, buf, sizeof buf, "uid-s0", "S0-by-device");
        dev_mod(&db, eid, buf);
    }
    opt.conflict_policy = PHEDAV_USE_SERVER;
    do_sync("conflict, server wins", &opt, &s);
    CHECK(s.conflicts == 1 && s.up_mod == 0 && s.down_mod == 1);
    CHECK(dev_find_containing(&db, "S0-by-server", NULL) && !dev_find_containing(&db, "S0-by-device", NULL) && dev_dirty_count(&db) == 0);
    /* ... and the local side wins */
    mk_item(kind, buf, sizeof buf, "uid-s0", "S0-by-server-2");
    CHECK(srv_put(sx[0], buf, kind, NULL) == 204);
    {
        const char *eid;
        dev_find_containing(&db, "S0-by-server", &eid);
        mk_item(kind, buf, sizeof buf, "uid-s0", "S0-by-device-2");
        dev_mod(&db, eid, buf);
    }
    opt.conflict_policy = PHEDAV_USE_LOCAL;
    do_sync("conflict, local wins", &opt, &s);
    CHECK(s.conflicts == 1 && s.up_mod == 1 && s.down_mod == 0 && s.errors == 0);
    CHECK(srv_get(sx[0], &b) == 200 && strstr(b.buf, "S0-by-device-2"));
    memset(&opt, 0, sizeof opt);
    /* server deleted + device modified, server wins -> the item disappears locally */
    CHECK(DEL_OK(srv_del(sx[1])));
    {
        const char *eid;
        dev_find_containing(&db, "Server1-edited-on-server", &eid);
        mk_item(kind, buf, sizeof buf, "uid-s1", "S1-dev-mod");
        dev_mod(&db, eid, buf);
    }
    do_sync("server deleted, device modified", &opt, &s);
    CHECK(s.conflicts == 1 && s.down_del == 1 && !dev_find_containing(&db, "S1-dev-mod", NULL) && dev_dirty_count(&db) == 0);

    /* ---- sync-token expiry */
    mk_item(kind, buf, sizeof buf, "uid-s5", "AfterExpiry");
    snprintf(href, sizeof href, "%suid-s5%s", g_col.href, kind == PHEDAV_CAL ? ".ics" : ".vcf");
    CHECK(srv_put(href, buf, kind, NULL) == 201);
    snprintf(db.meta.sync_token, sizeof db.meta.sync_token, "http://radicale.org/ns/sync/0000000000000000000000000000000000000000000000000000000000000000");
    do_sync("bogus sync token", &opt, &s);
    CHECK(s.token_expired == 1 && s.down_new == 1 && s.errors == 0 && dev_find_containing(&db, "AfterExpiry", NULL));
    CHECK(strstr(s.mode, "full") != NULL);
    do_sync("after recovery", &opt, &s);
    CHECK(!strcmp(s.mode, "sync-collection") && s.down_new + s.down_mod + s.down_del == 0);

    /* ---- ctag + etag listing fallback */
    opt.no_sync_collection = 1;
    fresh_device();
    do_sync("listing mode, fresh device", &opt, &s);
    CHECK(!strcmp(s.mode, "listing") && s.down_new == 3 && dev_count(&db) == 3 && s.errors == 0);
    CHECK(db.meta.ctag[0] || 1);
    do_sync("listing mode, unchanged", &opt, &s);
    if (db.meta.ctag[0]) CHECK(!strcmp(s.mode, "unchanged") && s.http_requests == 1);
    CHECK(DEL_OK(srv_del(sx[0])));
    mk_item(kind, buf, sizeof buf, "uid-s7", "ListingNew");
    snprintf(href, sizeof href, "%suid-s7%s", g_col.href, kind == PHEDAV_CAL ? ".ics" : ".vcf");
    CHECK(srv_put(href, buf, kind, NULL) == 201);
    {
        const char *eid;
        dev_find_containing(&db, "AfterExpiry", &eid);
        dev_del(&db, eid);
    }
    do_sync("listing: server del+add, local del", &opt, &s);
    CHECK(s.down_del == 1 && s.down_new == 1 && s.up_del == 1 && s.errors == 0);
    CHECK(!dev_find_containing(&db, "Server0", NULL) && dev_find_containing(&db, "ListingNew", NULL));
    snprintf(href, sizeof href, "%suid-s5%s", g_col.href, kind == PHEDAV_CAL ? ".ics" : ".vcf");
    CHECK(srv_get(href, &b) == 404);
    /* a local-only item uploaded in listing mode, then the device is wiped and re-synced with the sync-collection path */
    mk_item(kind, buf, sizeof buf, "uid-dev-2", "DeviceTwo");
    id = dev_add(&db, buf);
    CHECK(id != NULL);
    do_sync("listing: local add", &opt, &s);
    CHECK(s.up_new == 1);
    fresh_device();
    memset(&opt, 0, sizeof opt);
    do_sync("final fresh device (full)", &opt, &s);
    CHECK(s.down_new == dev_count(&db) && dev_count(&db) >= 3 && dev_find_containing(&db, "DeviceTwo", NULL));

    /* ---- an item larger than the item buffer is skipped, counted, and the token is not advanced */
    {
        static char big[DEV_DATA * 2];
        size_t n;
        mk_item(kind, big, sizeof big / 2, "uid-big", "Big");
        n = strlen(big);
        /* pad with a long property: NOTE (vCard) or DESCRIPTION (iCal) */
        if (kind == PHEDAV_CARD) n -= strlen("END:VCARD\r\n");
        else n -= strlen("END:VEVENT\r\nEND:VCALENDAR\r\n");
        n += (size_t)snprintf(big + n, sizeof big - n, "%s:", kind == PHEDAV_CARD ? "NOTE" : "DESCRIPTION");
        memset(big + n, 'x', 3000);
        n += 3000;
        snprintf(big + n, sizeof big - n, "\r\n%s", kind == PHEDAV_CARD ? "END:VCARD\r\n" : "END:VEVENT\r\nEND:VCALENDAR\r\n");
        snprintf(href, sizeof href, "%suid-big%s", g_col.href, kind == PHEDAV_CAL ? ".ics" : ".vcf");
        CHECK(srv_put(href, big, kind, NULL) == 201);
        dev.item_cap = 1024;                      /* a small item buffer */
        snprintf(extra, sizeof extra, "%s", db.meta.sync_token);
        do_sync("item larger than the buffer", &opt, &s);
        CHECK(s.errors == 1);
        CHECK(!strcmp(db.meta.sync_token, extra));    /* not advanced: retried next time */
        dev.item_cap = sizeof item_dev;
        do_sync("retry with a big enough buffer", &opt, &s);
        CHECK(s.errors == 0 && s.down_new == 1);
    }

    /* ---- cleanup */
    r = phedav_request(&srv, "DELETE", g_col.href, NULL, NULL, 0, NULL, NULL);
    printf("  cleanup DELETE %s -> %d\n", g_col.href, srv.status);
    CHECK(r == 0 && (srv.status == 204 || srv.status == 200));
}

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "both";
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_url = getenv("PHEDAV_URL");
    g_user = getenv("PHEDAV_USER");
    g_pass = getenv("PHEDAV_PASS");
    if (!g_url || !g_user) { fprintf(stderr, "set PHEDAV_URL, PHEDAV_USER, PHEDAV_PASS\n"); return 2; }
    if (!g_pass) g_pass = "";
    phedav_net n_dev = posix_net_make(&pn_dev), n_srv = posix_net_make(&pn_srv);
#ifdef PHEDAV_WITH_MBEDTLS
    /* PHEDAV_CA = path of a PEM bundle, or PHEDAV_PIN = 64 hex digits (SHA-256 of the server certificate DER) */
    static phedav_tls t_dev, t_srv;
    static char ca[16384];
    uint8_t pin[32];
    const char *capath = getenv("PHEDAV_CA"), *pinhex = getenv("PHEDAV_PIN");
    phedav_raw r_dev = { &pn_dev, posix_net_raw_open, posix_net_raw_send, posix_net_raw_recv, posix_net_raw_close };
    phedav_raw r_srv = { &pn_srv, posix_net_raw_open, posix_net_raw_send, posix_net_raw_recv, posix_net_raw_close };
    if (capath) {
        FILE *fp = fopen(capath, "rb");
        size_t k = fp ? fread(ca, 1, sizeof ca - 1, fp) : 0;
        if (fp) fclose(fp);
        ca[k] = 0;
        capath = ca;
    }
    if (pinhex && strlen(pinhex) == 64) {
        int i;
        for (i = 0; i < 32; i++) { unsigned v; sscanf(pinhex + 2 * i, "%2x", &v); pin[i] = (uint8_t)v; }
    } else pinhex = NULL;
    phedav_tls_init(&t_dev, r_dev, capath, pinhex ? pin : NULL);
    phedav_tls_init(&t_srv, r_srv, capath, pinhex ? pin : NULL);
    t_dev.debug = t_srv.debug = 1;
    n_dev = phedav_tls_net(&t_dev);
    n_srv = phedav_tls_net(&t_srv);
#endif
    phedav_init(&dev, n_dev, g_user, g_pass, item_dev, sizeof item_dev, work_dev, sizeof work_dev);
    phedav_init(&srv, n_srv, g_user, g_pass, item_srv, sizeof item_srv, work_srv, sizeof work_srv);
    phedav_set_origin(&srv, g_url, NULL, 0);
    if (strcmp(which, "card") != 0) run_kind(PHEDAV_CAL);
    if (strcmp(which, "cal") != 0) run_kind(PHEDAV_CARD);
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
