/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */
/* Offline unit tests: XML parser, HTTP client (scripted transport), multistatus reader, helpers, and mutation runs. */
#include "phedav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---------------------------------------------------------------------------------------------- xml */
typedef struct { char log[4096]; } xlog;

static void x_start(void *ctx, phedav_xml *x, const char *ns, const char *name, int d)
{
    xlog *l = (xlog *)ctx;
    char v[32];
    size_t n = strlen(l->log);
    snprintf(l->log + n, sizeof l->log - n, "<%s|%s@%d", ns, name, d);
    if (phedav_xml_attr(x, "name", v, sizeof v)) { n = strlen(l->log); snprintf(l->log + n, sizeof l->log - n, "[name=%s]", v); }
    n = strlen(l->log); snprintf(l->log + n, sizeof l->log - n, ">");
}
static void x_text(void *ctx, const char *s, size_t k)
{
    xlog *l = (xlog *)ctx;
    size_t n = strlen(l->log);
    if (n + k + 1 < sizeof l->log) { memcpy(l->log + n, s, k); l->log[n + k] = 0; }
}
static void x_end(void *ctx, const char *ns, const char *name, int d)
{
    xlog *l = (xlog *)ctx;
    size_t n = strlen(l->log);
    snprintf(l->log + n, sizeof l->log - n, "</%s|%s@%d>", ns, name, d);
}

static int xml_run(const char *doc, size_t step, xlog *l)
{
    static const phedav_xml_handler h = { x_start, x_text, x_end };
    static phedav_xml x;
    size_t n = strlen(doc), i;
    int r = 0;
    l->log[0] = 0;
    phedav_xml_init(&x, &h, l);
    for (i = 0; i < n && !r; i += step) r = phedav_xml_feed(&x, doc + i, (n - i < step) ? n - i : step);
    if (!r) r = phedav_xml_finish(&x);
    return r;
}

static void test_xml(void)
{
    xlog l, l2;
    size_t step;
    const char *doc =
        "<?xml version=\"1.0\"?><!DOCTYPE x><!-- c --><d:a xmlns:d=\"DAV:\" xmlns=\"urn:x\"><b d:q='1'>t&amp;&lt;&#65;&#x42;&#x20AC;</b>"
        "<d:c xmlns:d=\"urn:inner\"><d:e/></d:c><d:f>post</d:f><![CDATA[ <raw>]] ]]></d:a>";
    printf("xml\n");
    CHECK(xml_run(doc, 1000, &l) == 0);
    CHECK(strstr(l.log, "<DAV:|a@1>") != NULL);
    CHECK(strstr(l.log, "<urn:x|b@2>t&<AB\xE2\x82\xAC</urn:x|b@2>") != NULL);       /* default ns, entities, numeric refs, UTF-8 */
    CHECK(strstr(l.log, "<urn:inner|c@2>") != NULL && strstr(l.log, "<urn:inner|e@3></urn:inner|e@3>") != NULL);   /* prefix redefined inside */
    CHECK(strstr(l.log, "<DAV:|f@2>post</DAV:|f@2>") != NULL);                        /* ... and restored after the element */
    CHECK(strstr(l.log, " <raw>]] ") != NULL);                                        /* CDATA with ]] inside */
    for (step = 1; step < 9; step++) { CHECK(xml_run(doc, step, &l2) == 0); CHECK(!strcmp(l.log, l2.log)); }   /* chunking is invisible */
    CHECK(xml_run("<a xmlns=\"u\"><comp name=\"VEVENT\"/></a>", 3, &l) == 0 && strstr(l.log, "[name=VEVENT]"));
    CHECK(xml_run("<a><b></a>", 5, &l) != 0);                    /* mismatched end tag */
    CHECK(xml_run("<a>", 5, &l) != 0);                           /* unclosed */
    CHECK(xml_run("</a>", 5, &l) != 0);
    CHECK(xml_run("<a>&bogus;</a>", 5, &l) != 0);
    CHECK(xml_run("text<a/>", 5, &l) != 0);                      /* text outside the root */
    CHECK(xml_run("<p:a xmlns:p=\"DAV:\"><q:b/></p:a>", 7, &l) == 0 && strstr(l.log, "<|b@2>"));   /* unbound prefix = empty namespace */
    {
        char deep[512];
        int i;
        size_t o = 0;
        for (i = 0; i < PHEDAV_XML_MAX_DEPTH + 2; i++) o += (size_t)snprintf(deep + o, sizeof deep - o, "<a>");
        CHECK(xml_run(deep, 100, &l) == PHEDAV_XML_ERR_LIMIT);
    }
    CHECK(xml_run("<a x=\"1\" y=2/>", 100, &l) != 0);            /* unquoted attribute */
}

/* ---------------------------------------------------------------------------------------------- fake transport */
typedef struct {
    const char *script;
    size_t len, pos, chunk;
    char sent[16384];
    size_t sentlen;
    int opens, closes;
} fake;

static int f_open(void *ctx, const char *host, unsigned port, int tls) { fake *f = (fake *)ctx; (void)host; (void)port; (void)tls; f->opens++; return 0; }
static int f_send(void *ctx, const void *p, size_t n)
{
    fake *f = (fake *)ctx;
    if (f->sentlen + n < sizeof f->sent) { memcpy(f->sent + f->sentlen, p, n); f->sentlen += n; f->sent[f->sentlen] = 0; }
    return (int)n;
}
static int f_recv(void *ctx, void *p, size_t n)
{
    fake *f = (fake *)ctx;
    size_t k = f->len - f->pos;
    if (k > f->chunk) k = f->chunk;
    if (k > n) k = n;
    memcpy(p, f->script + f->pos, k);
    f->pos += k;
    return (int)k;
}
static void f_close(void *ctx) { ((fake *)ctx)->closes++; }

static phedav_net fake_net(fake *f, const char *script, size_t chunk)
{
    phedav_net n;
    memset(f, 0, sizeof *f);
    f->script = script; f->len = strlen(script); f->chunk = chunk;
    n.ctx = f; n.open = f_open; n.send = f_send; n.recv = f_recv; n.close = f_close;
    return n;
}

typedef struct { char buf[4096]; size_t n; } collect;
static int c_sink(void *ctx, const void *d, size_t n)
{
    collect *c = (collect *)ctx;
    if (c->n + n < sizeof c->buf) { memcpy(c->buf + c->n, d, n); c->n += n; c->buf[c->n] = 0; }
    return 0;
}

static phedav g_c;                 /* a phedav is ~25 KB: keep it static */
static char g_item[4096];
static unsigned char g_work[8192];

static void test_http(void)
{
    fake f;
    collect co;
    int chunk;
    printf("http\n");
    CHECK(phedav_base64("user:pass", 9, g_item, sizeof g_item) == 12 && !strcmp(g_item, "dXNlcjpwYXNz"));
    CHECK(phedav_base64("a", 1, g_item, sizeof g_item) == 4 && !strcmp(g_item, "YQ=="));
    CHECK(phedav_base64("ab", 2, g_item, sizeof g_item) == 4 && !strcmp(g_item, "YWI="));
    CHECK(phedav_base64("", 0, g_item, sizeof g_item) == 0 && g_item[0] == 0);

    for (chunk = 1; chunk <= 7; chunk += 3) {
        int r;
        /* Content-Length, then a chunked response on the same keep-alive connection, then a redirect, then close-delimited */
        const char *script =
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nETag: \"abc\"\r\n\r\nhello"
            "HTTP/1.1 207 Multi-Status\r\nTransfer-Encoding: chunked\r\n\r\n4;ext=1\r\nWiki\r\n5\r\npedia\r\n0\r\nTrailer: x\r\n\r\n"
            "HTTP/1.1 301 Moved\r\nLocation: /other/\r\nContent-Length: 0\r\n\r\n"
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"
            "HTTP/1.0 200 OK\r\n\r\nuntil-close";
        phedav_init(&g_c, fake_net(&f, script, (size_t)chunk), "user", "pass", g_item, sizeof g_item, g_work, sizeof g_work);
        phedav_set_origin(&g_c, "http://example.org:8080/", NULL, 0);
        co.n = 0; co.buf[0] = 0;
        r = phedav_request(&g_c, "GET", "/a", NULL, NULL, 0, c_sink, &co);
        CHECK(r == 0 && g_c.status == 200 && !strcmp(co.buf, "hello") && !strcmp(g_c.etag, "\"abc\""));
        CHECK(strstr(f.sent, "GET /a HTTP/1.1\r\nHost: example.org:8080\r\n") == f.sent);
        CHECK(strstr(f.sent, "Authorization: Basic dXNlcjpwYXNz\r\n") != NULL);
        co.n = 0;
        r = phedav_request(&g_c, "PROPFIND", "/b", "Depth: 1\r\n", "<x/>", 4, c_sink, &co);
        CHECK(r == 0 && g_c.status == 207 && !strcmp(co.buf, "Wikipedia"));
        CHECK(strstr(f.sent, "Content-Length: 4\r\n\r\n<x/>") != NULL);
        co.n = 0;
        r = phedav_request(&g_c, "GET", "/c", NULL, NULL, 0, c_sink, &co);     /* follows the redirect */
        CHECK(r == 0 && g_c.status == 200 && !strcmp(co.buf, "ok"));
        CHECK(strstr(f.sent, "GET /other/ HTTP/1.1") != NULL);
        CHECK(f.opens == 1);                                                    /* one connection for all of it */
        co.n = 0;
        r = phedav_request(&g_c, "GET", "/d", NULL, NULL, 0, c_sink, &co);
        CHECK(r == 0 && !strcmp(co.buf, "until-close") && f.closes >= 1);
    }
    {
        int r;
        phedav_init(&g_c, fake_net(&f, "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n", 5), "u", "p", g_item, sizeof g_item, g_work, sizeof g_work);
        phedav_set_origin(&g_c, "https://example.org/", NULL, 0);
        r = phedav_request(&g_c, "GET", "/", NULL, NULL, 0, NULL, NULL);
        CHECK(r == PHEDAV_ERR_AUTH);
        phedav_init(&g_c, fake_net(&f, "HTTP/1.1 302 Found\r\nLocation: http://evil.example/x\r\nContent-Length: 0\r\n\r\n", 5), "u", "p", g_item, sizeof g_item, g_work, sizeof g_work);
        phedav_set_origin(&g_c, "https://example.org/", NULL, 0);
        r = phedav_request(&g_c, "GET", "/", NULL, NULL, 0, NULL, NULL);
        CHECK(r == PHEDAV_ERR_REDIRECT);              /* cross-origin (and a downgrade) is refused */
        phedav_init(&g_c, fake_net(&f, "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n"
                                       "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n"
                                       "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n"
                                       "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n"
                                       "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n"
                                       "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n"
                                       "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n", 50), "u", "p", g_item, sizeof g_item, g_work, sizeof g_work);
        phedav_set_origin(&g_c, "http://example.org/", NULL, 0);
        r = phedav_request(&g_c, "GET", "/", NULL, NULL, 0, NULL, NULL);
        CHECK(r == PHEDAV_ERR_REDIRECT);
        phedav_init(&g_c, fake_net(&f, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort", 50), "u", "p", g_item, sizeof g_item, g_work, sizeof g_work);
        phedav_set_origin(&g_c, "http://example.org/", NULL, 0);
        r = phedav_request(&g_c, "GET", "/", NULL, NULL, 0, NULL, NULL);
        CHECK(r == PHEDAV_ERR_IO);                    /* truncated body */
        phedav_init(&g_c, fake_net(&f, "garbage\r\n\r\n", 50), "u", "p", g_item, sizeof g_item, g_work, sizeof g_work);
        phedav_set_origin(&g_c, "http://example.org/", NULL, 0);
        r = phedav_request(&g_c, "GET", "/", NULL, NULL, 0, NULL, NULL);
        CHECK(r == PHEDAV_ERR_PROTO);
    }
    {
        char out[64];
        phedav_href_norm("https://h.example:8443/dav/x/", out, sizeof out);
        CHECK(!strcmp(out, "/dav/x/"));
        phedav_href_norm("/already/path", out, sizeof out);
        CHECK(!strcmp(out, "/already/path"));
        CHECK(phedav_item_uid("BEGIN:VCARD\r\nFN:A\r\nUID:abc-\r\n 123\r\nEND:VCARD\r\n", 44, out, sizeof out) == 7 && !strcmp(out, "abc-123"));
        CHECK(phedav_item_uid("BEGIN:VEVENT\nuid;x=y:u1\nEND:VEVENT\n", 35, out, sizeof out) == 2 && !strcmp(out, "u1"));
        CHECK(phedav_item_uid("BEGIN:VCARD\nFN:A\nEND:VCARD\n", 27, out, sizeof out) == 0);
    }
}

/* ---------------------------------------------------------------------------------------------- multistatus */
static const char MS_SYNC[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
    "<multistatus xmlns=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\">\r\n"
    " <response><href>/u/cal/a.ics</href><propstat><prop><getetag>\"e1\"</getetag></prop><status>HTTP/1.1 200 OK</status></propstat></response>\r\n"
    " <response><href>http://h.example/u/cal/b%20c.ics</href><propstat><prop><getetag>&quot;e2&quot;</getetag></prop>"
    "<status>HTTP/1.1 200 OK</status></propstat></response>\r\n"
    " <response><href>/u/cal/gone.ics</href><status>HTTP/1.1 404 Not Found</status></response>\r\n"
    " <response><href>/u/cal/x.ics</href><propstat><prop><getetag>\"zz\"</getetag></prop><status>HTTP/1.1 200 OK</status></propstat>"
    "<propstat><prop><displayname/></prop><status>HTTP/1.1 404 Not Found</status></propstat></response>\r\n"
    " <sync-token>http://x/sync/42</sync-token>\r\n"
    "</multistatus>";

typedef struct { char out[2048]; } msout;
static int ms_cb(phedav *c, void *ctx, const phedav_resource *r)
{
    msout *m = (msout *)ctx;
    size_t n = strlen(m->out);
    (void)c;
    snprintf(m->out + n, sizeof m->out - n, "[%s|%s|%d|%x|%s]", r->href, r->etag, r->status, r->flags, r->name);
    return 0;
}

static void test_multistatus(void)
{
    fake f;
    msout m;
    char tok[PHEDAV_TOKEN_MAX];
    char resp[4096];
    int chunk, r;
    printf("multistatus\n");
    for (chunk = 1; chunk <= 64; chunk *= 8) {
        snprintf(resp, sizeof resp, "HTTP/1.1 207 Multi-Status\r\nContent-Length: %u\r\n\r\n%s", (unsigned)strlen(MS_SYNC), MS_SYNC);
        phedav_init(&g_c, fake_net(&f, resp, (size_t)chunk), "u", "p", g_item, sizeof g_item, g_work, sizeof g_work);
        phedav_set_origin(&g_c, "http://h.example/", NULL, 0);
        m.out[0] = 0;
        r = phedav_dav(&g_c, "REPORT", "/u/cal/", 0, "<x/>", ms_cb, &m, tok);
        CHECK(r == 0 && g_c.status == 207);
        CHECK(strstr(m.out, "[/u/cal/a.ics|\"e1\"|0|0|]") != NULL);
        CHECK(strstr(m.out, "[/u/cal/b%20c.ics|\"e2\"|0|0|]") != NULL);          /* absolute URL reduced, entities decoded */
        CHECK(strstr(m.out, "[/u/cal/gone.ics||404|0|]") != NULL);
        CHECK(strstr(m.out, "[/u/cal/x.ics|\"zz\"|0|0|]") != NULL);              /* the 404 propstat contributes nothing */
        CHECK(!strcmp(tok, "http://x/sync/42"));
    }
    {
        static const char col[] =
            "<d:multistatus xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\" xmlns:cs=\"http://calendarserver.org/ns/\">"
            "<d:response><d:href>/u/work/</d:href><d:propstat><d:prop><d:resourcetype><d:collection/><c:calendar/></d:resourcetype>"
            "<d:displayname>Work &amp; Home</d:displayname><cs:getctag>c-7</cs:getctag>"
            "<c:supported-calendar-component-set><c:comp name=\"VEVENT\"/><c:comp name=\"VTODO\"/></c:supported-calendar-component-set>"
            "<d:current-user-principal><d:href>/u/</d:href></d:current-user-principal>"
            "<c:calendar-home-set><d:href>/u/</d:href><d:href>/second/</d:href></c:calendar-home-set>"
            "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>";
        snprintf(resp, sizeof resp, "HTTP/1.1 207 Multi-Status\r\nContent-Length: %u\r\n\r\n%s", (unsigned)strlen(col), col);
        phedav_init(&g_c, fake_net(&f, resp, 9), "u", "p", g_item, sizeof g_item, g_work, sizeof g_work);
        phedav_set_origin(&g_c, "http://h.example/", NULL, 0);
        m.out[0] = 0;
        r = phedav_dav(&g_c, "PROPFIND", "/u/", 1, "<x/>", ms_cb, &m, NULL);
        CHECK(r == 0);
        /* flags: collection 1 | calendar 2 | VEVENT 0x10 | VTODO 0x20 = 0x33 */
        CHECK(strstr(m.out, "[/u/work/||0|33|Work & Home]") != NULL);
    }
    {
        static const char data[] =
            "<d:multistatus xmlns:d=\"DAV:\" xmlns:card=\"urn:ietf:params:xml:ns:carddav\"><d:response><d:href>/ab/1.vcf</d:href>"
            "<d:propstat><d:prop><d:getetag>\"1\"</d:getetag><card:address-data>BEGIN:VCARD\r\nFN:A &amp; B\r\nEND:VCARD\r\n</card:address-data>"
            "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>";
        snprintf(resp, sizeof resp, "HTTP/1.1 207 Multi-Status\r\nTransfer-Encoding: chunked\r\n\r\n%x\r\n%s\r\n0\r\n\r\n", (unsigned)strlen(data), data);
        phedav_init(&g_c, fake_net(&f, resp, 4), "u", "p", g_item, sizeof g_item, g_work, sizeof g_work);
        phedav_set_origin(&g_c, "http://h.example/", NULL, 0);
        m.out[0] = 0;
        r = phedav_dav(&g_c, "REPORT", "/ab/", 1, "<x/>", ms_cb, &m, NULL);
        CHECK(r == 0 && strstr(m.out, "[/ab/1.vcf|\"1\"|0|100|]") != NULL);
        CHECK(g_c.item_len == strlen("BEGIN:VCARD\r\nFN:A & B\r\nEND:VCARD\r\n") && !memcmp(g_c.item, "BEGIN:VCARD\r\nFN:A & B", 21));
        /* a data element larger than the item buffer is flagged, not truncated silently */
        phedav_init(&g_c, fake_net(&f, resp, 4), "u", "p", g_item, 10, g_work, sizeof g_work);
        phedav_set_origin(&g_c, "http://h.example/", NULL, 0);
        m.out[0] = 0;
        r = phedav_dav(&g_c, "REPORT", "/ab/", 1, "<x/>", ms_cb, &m, NULL);
        CHECK(r == 0 && strstr(m.out, "|0|0|") != NULL && g_c.item_over);
    }
}

/* ---------------------------------------------------------------------------------------------- mutation runs */
static unsigned rng = 12345u;
static unsigned rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static void test_mutation(void)
{
    int it;
    char doc[sizeof MS_SYNC];
    fake f;
    msout m;
    char resp[4096], tok[PHEDAV_TOKEN_MAX];
    printf("mutation (xml + http)\n");
    for (it = 0; it < 4000; it++) {
        size_t len = strlen(MS_SYNC), k;
        int muts = 1 + (int)(rnd() % 6);
        xlog *l = (xlog *)malloc(sizeof *l);
        memcpy(doc, MS_SYNC, len + 1);
        for (k = 0; k < (size_t)muts; k++) {
            size_t pos = rnd() % len;
            switch (rnd() % 4) {
            case 0: doc[pos] = (char)(rnd() & 0xFF); if (!doc[pos]) doc[pos] = ' '; break;
            case 1: doc[pos] = "<>&;\"'/!?[]-"[rnd() % 12]; break;
            case 2: len = pos ? pos : 1; doc[len] = 0; break;
            default: if (len > 8 && pos + 2 < len) { memmove(doc + pos, doc + pos + 1, len - pos); len--; } break;
            }
        }
        doc[len] = 0;
        xml_run(doc, 1 + rnd() % 40, l);
        free(l);
        if (it % 8 == 0) {
            snprintf(resp, sizeof resp, "HTTP/1.1 207 X\r\nContent-Length: %u\r\n\r\n%s", (unsigned)strlen(doc), doc);
            phedav_init(&g_c, fake_net(&f, resp, 1 + rnd() % 50), "u", "p", g_item, sizeof g_item, g_work, sizeof g_work);
            phedav_set_origin(&g_c, "http://h.example/", NULL, 0);
            m.out[0] = 0;
            phedav_dav(&g_c, "REPORT", "/u/", 0, "<x/>", ms_cb, &m, tok);
        }
    }
    /* HTTP framing mutations: random bytes where headers should be */
    for (it = 0; it < 2000; it++) {
        char h[200];
        size_t i, n = (size_t)snprintf(h, sizeof h, "HTTP/1.1 207 X\r\nTransfer-Encoding: chunked\r\nContent-Length: 7\r\nLocation: /x\r\n\r\n4\r\nabcd\r\n0\r\n\r\n");
        for (i = 0; i < 4; i++) h[rnd() % n] = (char)(1 + rnd() % 126);
        phedav_init(&g_c, fake_net(&f, h, 1 + rnd() % 9), "u", "p", g_item, sizeof g_item, g_work, sizeof g_work);
        phedav_set_origin(&g_c, "http://h.example/", NULL, 0);
        phedav_request(&g_c, "GET", "/", NULL, NULL, 0, c_sink, &(collect){ .n = 0 });
    }
    CHECK(1);
}

int main(void)
{
    test_xml();
    test_http();
    test_multistatus();
    test_mutation();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
