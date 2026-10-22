/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

/*
 * phedav_xml -- a small push (streaming) XML parser, enough for WebDAV multistatus bodies.
 *
 * Feed it bytes in chunks of any size (even one byte at a time). It resolves namespace prefixes (default namespace,
 * redefinition on inner elements, unbound prefixes give an empty namespace), decodes the five predefined entities and
 * numeric character references (to UTF-8), passes CDATA through as text, and skips comments, processing instructions and
 * a DOCTYPE. It does not validate, does not expand DTD entities and keeps no tree. No malloc: every limit is a macro.
 *
 * Callbacks: start(ns, local name, depth), text(decoded bytes, possibly split in several calls), end(...). Depth of the
 * root element is 1. The ns/name pointers are valid only during the call. Inside start() phedav_xml_attr() returns an
 * attribute of the element (no namespace handling for attributes; xmlns* attributes are not reported).
 */
#ifndef PHEDAV_XML_H
#define PHEDAV_XML_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PHEDAV_XML_MAX_DEPTH
#define PHEDAV_XML_MAX_DEPTH 24
#endif
#ifndef PHEDAV_XML_MAX_NS
#define PHEDAV_XML_MAX_NS 24      /* live namespace bindings (all open elements together) */
#endif
#ifndef PHEDAV_XML_POOL
#define PHEDAV_XML_POOL 1024      /* bytes for prefixes + URIs of the live bindings */
#endif
#ifndef PHEDAV_XML_NAMES
#define PHEDAV_XML_NAMES 512      /* bytes for the names of the open elements */
#endif
#ifndef PHEDAV_XML_TAGBUF
#define PHEDAV_XML_TAGBUF 1024    /* one start tag with all its attributes */
#endif

#define PHEDAV_XML_OK 0
#define PHEDAV_XML_ERR_SYNTAX (-1)
#define PHEDAV_XML_ERR_LIMIT (-2)   /* nesting, namespace pool or tag too large */

typedef struct phedav_xml phedav_xml;

typedef struct {
    void (*start)(void *ctx, phedav_xml *x, const char *ns, const char *name, int depth);
    void (*text)(void *ctx, const char *s, size_t n);
    void (*end)(void *ctx, const char *ns, const char *name, int depth);
} phedav_xml_handler;

struct phedav_xml {
    phedav_xml_handler h;
    void *ctx;
    int state, err;
    int depth;
    /* tag collection */
    char tag[PHEDAV_XML_TAGBUF];
    size_t taglen;
    char quote;
    const char *attrs;              /* set during start(): the attribute part of the tag */
    /* text */
    char text[96];
    size_t textlen;
    char ent[12];
    size_t entlen;
    int brk, prevq, cdata_in;
    /* namespaces */
    char pool[PHEDAV_XML_POOL];
    size_t pool_used;
    struct { const char *prefix, *uri; } ns[PHEDAV_XML_MAX_NS];
    int nns;
    char names[PHEDAV_XML_NAMES];
    size_t names_used;
    struct { const char *ns, *name; int ns_base; size_t pool_base; size_t names_base; } stack[PHEDAV_XML_MAX_DEPTH];
};

void phedav_xml_init(phedav_xml *x, const phedav_xml_handler *h, void *ctx);
/* returns PHEDAV_XML_OK or a sticky error */
int phedav_xml_feed(phedav_xml *x, const void *data, size_t n);
/* end of input: checks that every element was closed */
int phedav_xml_finish(phedav_xml *x);
/* value of attribute `name` of the element being started, copied into out (decoded for the predefined entities); 1 if found */
int phedav_xml_attr(const phedav_xml *x, const char *name, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
