/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */
#include "phedav_xml.h"

#include <string.h>

enum { ST_TEXT, ST_TAG, ST_COMMENT, ST_CDATA, ST_PI, ST_DECL, ST_ENT };

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

void phedav_xml_init(phedav_xml *x, const phedav_xml_handler *h, void *ctx)
{
    memset(x, 0, sizeof *x);
    x->h = *h;
    x->ctx = ctx;
    x->state = ST_TEXT;
}

static void flush_text(phedav_xml *x)
{
    if (x->textlen && x->depth > 0 && x->h.text) x->h.text(x->ctx, x->text, x->textlen);
    x->textlen = 0;
}

static void text_put(phedav_xml *x, char c)
{
    if (x->textlen == sizeof x->text) flush_text(x);
    x->text[x->textlen++] = c;
}

static void text_utf8(phedav_xml *x, unsigned long cp)
{
    if (cp < 0x80) text_put(x, (char)cp);
    else if (cp < 0x800) { text_put(x, (char)(0xC0 | (cp >> 6))); text_put(x, (char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
        text_put(x, (char)(0xE0 | (cp >> 12))); text_put(x, (char)(0x80 | ((cp >> 6) & 0x3F)));
        text_put(x, (char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        text_put(x, (char)(0xF0 | (cp >> 18))); text_put(x, (char)(0x80 | ((cp >> 12) & 0x3F)));
        text_put(x, (char)(0x80 | ((cp >> 6) & 0x3F))); text_put(x, (char)(0x80 | (cp & 0x3F)));
    } else text_put(x, '?');
}

/* decodes the entity in e[0..n) (without & and ;); returns a code point, or -1 */
static long entity_value(const char *e, size_t n)
{
    if (n == 2 && !memcmp(e, "lt", 2)) return '<';
    if (n == 2 && !memcmp(e, "gt", 2)) return '>';
    if (n == 3 && !memcmp(e, "amp", 3)) return '&';
    if (n == 4 && !memcmp(e, "quot", 4)) return '"';
    if (n == 4 && !memcmp(e, "apos", 4)) return '\'';
    if (n >= 2 && e[0] == '#') {
        unsigned long v = 0;
        size_t i = 1;
        int base = 10;
        if (e[1] == 'x' || e[1] == 'X') { base = 16; i = 2; }
        if (i >= n) return -1;
        for (; i < n; i++) {
            int d;
            if (e[i] >= '0' && e[i] <= '9') d = e[i] - '0';
            else if (base == 16 && e[i] >= 'a' && e[i] <= 'f') d = e[i] - 'a' + 10;
            else if (base == 16 && e[i] >= 'A' && e[i] <= 'F') d = e[i] - 'A' + 10;
            else return -1;
            v = v * (unsigned long)base + (unsigned long)d;
            if (v > 0x10FFFF) return -1;
        }
        return (long)v;
    }
    return -1;
}

/* attribute iterator over `p`: name, '=', quoted value. Returns 1 per attribute, 0 at the end, -1 on a syntax error. */
static int next_attr(const char **pp, const char **name, size_t *nlen, const char **val, size_t *vlen)
{
    const char *p = *pp;
    char q;
    while (*p && is_ws(*p)) p++;
    if (!*p) { *pp = p; return 0; }
    *name = p;
    while (*p && *p != '=' && !is_ws(*p)) p++;
    *nlen = (size_t)(p - *name);
    while (*p && is_ws(*p)) p++;
    if (*p != '=' || *nlen == 0) return -1;
    p++;
    while (*p && is_ws(*p)) p++;
    q = *p;
    if (q != '"' && q != '\'') return -1;
    p++;
    *val = p;
    while (*p && *p != q) p++;
    if (*p != q) return -1;
    *vlen = (size_t)(p - *val);
    *pp = p + 1;
    return 1;
}

int phedav_xml_attr(const phedav_xml *x, const char *name, char *out, size_t cap)
{
    const char *p = x->attrs, *n, *v;
    size_t nl, vl, want = strlen(name);
    int r;
    if (!cap) return 0;
    out[0] = 0;
    if (!p) return 0;
    while ((r = next_attr(&p, &n, &nl, &v, &vl)) > 0) {
        if (nl == want && !memcmp(n, name, nl)) {
            size_t o = 0, i = 0;
            while (i < vl && o + 1 < cap) {
                if (v[i] == '&') {
                    size_t j = i + 1;
                    long cp = -1;
                    while (j < vl && v[j] != ';') j++;
                    if (j < vl) cp = entity_value(v + i + 1, j - i - 1);
                    if (cp >= 0 && cp < 128) { out[o++] = (char)cp; i = j + 1; continue; }
                }
                out[o++] = v[i++];
            }
            out[o] = 0;
            return 1;
        }
    }
    return 0;
}

static const char *ns_lookup(const phedav_xml *x, const char *prefix, size_t plen)
{
    int i;
    for (i = x->nns - 1; i >= 0; i--) {
        if (strlen(x->ns[i].prefix) == plen && !memcmp(x->ns[i].prefix, prefix, plen)) return x->ns[i].uri;
    }
    return "";
}

static int bind_ns(phedav_xml *x, const char *prefix, size_t plen, const char *uri, size_t ulen)
{
    char *dst;
    if (x->nns >= PHEDAV_XML_MAX_NS || x->pool_used + plen + ulen + 2 > sizeof x->pool) return PHEDAV_XML_ERR_LIMIT;
    dst = x->pool + x->pool_used;
    memcpy(dst, prefix, plen);
    dst[plen] = 0;
    memcpy(dst + plen + 1, uri, ulen);
    dst[plen + 1 + ulen] = 0;
    x->ns[x->nns].prefix = dst;
    x->ns[x->nns].uri = dst + plen + 1;
    x->nns++;
    x->pool_used += plen + ulen + 2;
    return 0;
}

static void pop_element(phedav_xml *x)
{
    int d = x->depth - 1;
    x->nns = x->stack[d].ns_base;
    x->pool_used = x->stack[d].pool_base;
    x->names_used = x->stack[d].names_base;
    x->depth--;
}

static int do_start(phedav_xml *x)
{
    char *t = x->tag;
    size_t n = x->taglen, nl = 0, plen;
    int selfclose = 0, r, d;
    const char *p, *an, *av, *colon = NULL, *nsuri;
    size_t anl, avl;
    size_t base_pool = x->pool_used;
    int base_ns = x->nns;
    size_t base_names = x->names_used;
    char *nm;

    if (n && t[n - 1] == '/') { selfclose = 1; n--; }
    t[n] = 0;
    while (nl < n && !is_ws(t[nl])) nl++;
    if (nl == 0) return PHEDAV_XML_ERR_SYNTAX;
    if (nl < n) { t[nl] = 0; p = t + nl + 1; } else p = t + n;
    if (x->depth >= PHEDAV_XML_MAX_DEPTH) return PHEDAV_XML_ERR_LIMIT;
    /* xmlns declarations first, so the element's own prefix sees them */
    {
        const char *q = p;
        while ((r = next_attr(&q, &an, &anl, &av, &avl)) > 0) {
            if (anl == 5 && !memcmp(an, "xmlns", 5)) {
                if ((r = bind_ns(x, "", 0, av, avl))) return r;
            } else if (anl > 6 && !memcmp(an, "xmlns:", 6)) {
                if ((r = bind_ns(x, an + 6, anl - 6, av, avl))) return r;
            }
        }
        if (r < 0) return PHEDAV_XML_ERR_SYNTAX;
    }
    {
        size_t i;
        for (i = 0; i < nl; i++) if (t[i] == ':') { colon = t + i; break; }
    }
    if (colon) { plen = (size_t)(colon - t); nsuri = ns_lookup(x, t, plen); colon++; }
    else { nsuri = ns_lookup(x, "", 0); colon = t; }
    nl = strlen(colon);
    if (x->names_used + nl + 1 > sizeof x->names) return PHEDAV_XML_ERR_LIMIT;
    nm = x->names + x->names_used;
    memcpy(nm, colon, nl + 1);
    x->names_used += nl + 1;
    d = x->depth;
    x->stack[d].ns = nsuri;
    x->stack[d].name = nm;
    x->stack[d].ns_base = base_ns;
    x->stack[d].pool_base = base_pool;
    x->stack[d].names_base = base_names;
    x->depth++;
    flush_text(x);
    x->attrs = p;
    if (x->h.start) x->h.start(x->ctx, x, nsuri, nm, x->depth);
    x->attrs = NULL;
    if (selfclose) {
        if (x->h.end) x->h.end(x->ctx, nsuri, nm, x->depth);
        pop_element(x);
    }
    return 0;
}

static int do_end(phedav_xml *x)
{
    /* x->tag = "/name" */
    const char *name = x->tag + 1, *colon;
    size_t n = x->taglen - 1;
    int d;
    while (n && is_ws(name[n - 1])) n--;
    if (x->depth == 0) return PHEDAV_XML_ERR_SYNTAX;
    d = x->depth - 1;
    colon = memchr(name, ':', n);
    if (colon) { n -= (size_t)(colon + 1 - name); name = colon + 1; }
    if (strlen(x->stack[d].name) != n || memcmp(x->stack[d].name, name, n)) return PHEDAV_XML_ERR_SYNTAX;
    flush_text(x);
    if (x->h.end) x->h.end(x->ctx, x->stack[d].ns, x->stack[d].name, x->depth);
    pop_element(x);
    return 0;
}

static int step(phedav_xml *x, char c)
{
    switch (x->state) {
    case ST_TEXT:
        if (c == '<') { x->state = ST_TAG; x->taglen = 0; x->quote = 0; }
        else if (c == '&') { x->state = ST_ENT; x->entlen = 0; }
        else if (x->depth > 0) text_put(x, c);
        else if (!is_ws(c) && (unsigned char)c != 0xEF && (unsigned char)c != 0xBB && (unsigned char)c != 0xBF)
            return PHEDAV_XML_ERR_SYNTAX;   /* character data outside the root (a UTF-8 BOM is tolerated) */
        return 0;
    case ST_ENT:
        if (c == ';') {
            long cp = entity_value(x->ent, x->entlen);
            if (cp < 0) return PHEDAV_XML_ERR_SYNTAX;
            if (x->depth > 0) text_utf8(x, (unsigned long)cp);
            x->state = ST_TEXT;
        } else if (x->entlen >= sizeof x->ent) return PHEDAV_XML_ERR_SYNTAX;
        else x->ent[x->entlen++] = c;
        return 0;
    case ST_TAG:
        if (x->quote) {
            if (c == x->quote) x->quote = 0;
        } else if (c == '"' || c == '\'') x->quote = c;
        else if (c == '>') {
            x->state = ST_TEXT;
            if (x->taglen == 0) return PHEDAV_XML_ERR_SYNTAX;
            return (x->tag[0] == '/') ? do_end(x) : do_start(x);
        }
        if (x->taglen + 1 >= sizeof x->tag) return PHEDAV_XML_ERR_LIMIT;
        x->tag[x->taglen++] = c;
        /* "<?", "<!--", "<![CDATA[", "<!DOCTYPE" are recognised by their first characters */
        if (!x->quote) {
            if (x->tag[0] == '?') { x->state = ST_PI; x->prevq = 0; return 0; }
            if (x->tag[0] == '!') {
                static const char cd[] = "![CDATA[";
                static const char cm[] = "!--";
                if (x->taglen == 3 && !memcmp(x->tag, cm, 3)) { x->state = ST_COMMENT; x->brk = 0; return 0; }
                if (x->taglen == 8 && !memcmp(x->tag, cd, 8)) { x->state = ST_CDATA; x->brk = 0; return 0; }
                if (x->taglen < 3 && !memcmp(x->tag, cm, x->taglen)) return 0;
                if (x->taglen < 8 && !memcmp(x->tag, cd, x->taglen)) return 0;
                x->state = ST_DECL;
            }
        }
        return 0;
    case ST_COMMENT:
        if (c == '-') x->brk++;
        else {
            if (c == '>' && x->brk >= 2) x->state = ST_TEXT;
            x->brk = 0;
        }
        return 0;
    case ST_CDATA:
        if (c == ']') { x->brk++; return 0; }
        if (c == '>' && x->brk >= 2) {
            while (x->brk-- > 2) text_put(x, ']');
            x->state = ST_TEXT;
            x->brk = 0;
            return 0;
        }
        while (x->brk > 0) {
            if (x->depth > 0) text_put(x, ']');
            x->brk--;
        }
        if (x->depth > 0) text_put(x, c);
        return 0;
    case ST_PI:
        if (c == '>' && x->prevq) x->state = ST_TEXT;
        x->prevq = (c == '?');
        return 0;
    case ST_DECL:
        if (c == '>') x->state = ST_TEXT;
        return 0;
    }
    return PHEDAV_XML_ERR_SYNTAX;
}

int phedav_xml_feed(phedav_xml *x, const void *data, size_t n)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i;
    if (x->err) return x->err;
    for (i = 0; i < n; i++) {
        int r = step(x, (char)p[i]);
        if (r) { x->err = r; return r; }
    }
    return 0;
}

int phedav_xml_finish(phedav_xml *x)
{
    if (x->err) return x->err;
    if (x->depth != 0 || x->state != ST_TEXT) x->err = PHEDAV_XML_ERR_SYNTAX;
    return x->err;
}
