/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */

/*
 * phedav -- a portable CalDAV / CardDAV sync core (C99, no malloc, no OS calls).
 *
 * Layers (each usable alone):
 *   1. phedav_xml   streaming XML (see phedav_xml.h)
 *   2. phedav_http  HTTP/1.1 over an abstract byte stream: keep-alive, Content-Length, chunked, redirects, Basic auth
 *   3. discovery    .well-known -> current-user-principal -> calendar-home-set / addressbook-home-set -> collections
 *   4. sync         RFC 6578 sync-collection (fallback: ctag + etag listing), multiget, PUT/DELETE with If-Match,
 *                   two-way, with a persisted href -> etag -> local id map kept by the host through callbacks
 *
 * Items stay opaque text (vCard 3.0/4.0, iCalendar). The host converts them to and from its own database in the `phedav_local`
 * callbacks (SYMBIAN.md names the platform classes).
 *
 * Memory: one `phedav` object (about 20 KB), a caller-owned item buffer (the largest single vCard / iCalendar object;
 * 64 KB is enough for contacts without photos, use 256 KB with photos) and a caller-owned work arena (the list of server
 * resources of one collection: about 150 bytes per item, plus 60 bytes per pending local change).
 * Everything is blocking; the transport's recv() should have a timeout.
 */
#ifndef PHEDAV_H
#define PHEDAV_H

#include <stddef.h>
#include <stdint.h>

#include "phedav_xml.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PHEDAV_VERSION "0.1"

#define PHEDAV_OK 0
#define PHEDAV_ERR_IO (-1)          /* transport failure */
#define PHEDAV_ERR_PROTO (-2)       /* malformed HTTP or XML */
#define PHEDAV_ERR_AUTH (-3)        /* 401 / 403 on a plain request */
#define PHEDAV_ERR_HTTP (-4)        /* another unexpected status; see phedav.status */
#define PHEDAV_ERR_BUF (-5)         /* a buffer is too small (item buffer, work arena, header) */
#define PHEDAV_ERR_REDIRECT (-6)    /* too many redirects, a downgrade, or a cross-origin redirect that is not allowed */
#define PHEDAV_ERR_DISCOVERY (-7)   /* no principal / home set found */
#define PHEDAV_ERR_STORE (-8)       /* a storage or local-database callback failed */
#define PHEDAV_ERR_ARG (-9)

#ifndef PHEDAV_HOST_MAX
#define PHEDAV_HOST_MAX 128
#endif
#ifndef PHEDAV_HREF_MAX
#define PHEDAV_HREF_MAX 384
#endif
#ifndef PHEDAV_ETAG_MAX
#define PHEDAV_ETAG_MAX 96
#endif
#ifndef PHEDAV_TOKEN_MAX
#define PHEDAV_TOKEN_MAX 192
#endif
#ifndef PHEDAV_NAME_MAX
#define PHEDAV_NAME_MAX 96
#endif
#ifndef PHEDAV_ID_MAX
#define PHEDAV_ID_MAX 48
#endif
#ifndef PHEDAV_BODY_MAX
#define PHEDAV_BODY_MAX 8192        /* a request body built by the core (PROPFIND / REPORT) */
#endif
#ifndef PHEDAV_MULTIGET_BATCH
#define PHEDAV_MULTIGET_BATCH 20
#endif

/* ---------------------------------------------------------------------------------------------------------------- */
/* transport                                                                                                          */

typedef struct {
    void *ctx;
    /* connects to host:port; tls != 0 = wrap in TLS (the host/adapter validates the certificate). Returns 0 or < 0. */
    int (*open)(void *ctx, const char *host, unsigned port, int tls);
    int (*send)(void *ctx, const void *p, size_t n);   /* sends all n bytes: returns n, or < 0 */
    int (*recv)(void *ctx, void *p, size_t n);         /* blocks; > 0 bytes read, 0 = end of stream, < 0 = error / timeout */
    void (*close)(void *ctx);
} phedav_net;

/* ---------------------------------------------------------------------------------------------------------------- */
/* client                                                                                                             */

/* body sink: called with each piece of a 2xx response body (the data is valid only during the call); return 0 to continue */
typedef int (*phedav_sink)(void *ctx, const void *data, size_t n);

/* a resource as reported in a multistatus response */
#define PHEDAV_R_COLLECTION 0x01u
#define PHEDAV_R_CALENDAR 0x02u
#define PHEDAV_R_ADDRESSBOOK 0x04u
#define PHEDAV_R_VEVENT 0x10u       /* supported-calendar-component-set */
#define PHEDAV_R_VTODO 0x20u
#define PHEDAV_R_VJOURNAL 0x40u
#define PHEDAV_R_HASDATA 0x100u     /* calendar-data / address-data was present: the text is in phedav.item */

typedef struct {
    char href[PHEDAV_HREF_MAX];     /* path (absolute URLs of our own origin are reduced to their path) */
    char etag[PHEDAV_ETAG_MAX];     /* opaque, quotes included */
    char ctag[PHEDAV_ETAG_MAX];
    char sync_token[PHEDAV_TOKEN_MAX];
    char name[PHEDAV_NAME_MAX];     /* displayname */
    char principal[PHEDAV_HREF_MAX];
    char home[PHEDAV_HREF_MAX];     /* calendar-home-set / addressbook-home-set */
    int status;                     /* response-level status code (sync-collection: 404 = deleted), 0 = none */
    unsigned flags;
} phedav_resource;

typedef struct phedav phedav;
/* called once per <response>; return 0 to continue, nonzero aborts the request */
typedef int (*phedav_resource_cb)(phedav *c, void *ctx, const phedav_resource *r);

/* internal state of the multistatus reader */
typedef struct {
    phedav_resource rec, tmp;       /* the current <response>, the current <propstat> */
    unsigned char kind[PHEDAV_XML_MAX_DEPTH + 2];
    char *cap; size_t capcap, caplen; int capkind;
    int ps_status, in_prop, in_resp, in_ps;
    char stbuf[40]; size_t stlen; int in_status;
    int code_bad_token;             /* DAV:valid-sync-token precondition seen */
    int aborted;
    phedav *owner;
    phedav_resource_cb cb; void *cbctx;
    char *top_token; int top_seen;
} phedav_ms;

struct phedav {
    phedav_net net;
    /* settings */
    char auth[PHEDAV_ID_MAX * 8];   /* "Basic ..." */
    int allow_cross_origin;         /* follow a redirect to another host (credentials are then sent there too) */
    const char *user_agent;
    /* current origin */
    int tls;
    char host[PHEDAV_HOST_MAX];
    unsigned port;
    /* connection */
    int connected, c_tls;
    char c_host[PHEDAV_HOST_MAX];
    unsigned c_port;
    unsigned char rbuf[2048];
    size_t rpos, rlen;
    unsigned requests;              /* HTTP requests sent (statistics) */
    /* last response */
    int status;
    char etag[PHEDAV_ETAG_MAX];     /* ETag header */
    char location[PHEDAV_HREF_MAX + PHEDAV_HOST_MAX];
    long content_length;
    int chunked, conn_close;
    /* request scratch */
    char hdr[2560];
    char body[PHEDAV_BODY_MAX];
    /* host buffers */
    char *item; size_t item_cap, item_len; int item_over;   /* the calendar-data / address-data text of the current response */
    unsigned char *work; size_t work_cap, work_used;
    /* multistatus parsing state (internal) */
    phedav_ms ms;
    phedav_xml xml;
};

/* Fills in a client. `user`/`pass` may be NULL (no Authorization header). `item_buf` holds one vCard/iCalendar object,
 * `work` is the arena (see above). Returns PHEDAV_OK or PHEDAV_ERR_ARG. */
int phedav_init(phedav *c, phedav_net net, const char *user, const char *pass,
                char *item_buf, size_t item_cap, void *work, size_t work_cap);
/* sets the origin from "http://host[:port]/..." or "https://host[:port]/..."; returns the path part (or "/") in path_out */
int phedav_set_origin(phedav *c, const char *url, char *path_out, size_t cap);
void phedav_close(phedav *c);

/* One HTTP request. `target` is a path ("/dav/x") or an absolute URL. `extra` = complete header lines ("Depth: 1\r\n").
 * 2xx bodies go to `sink` (non-2xx bodies are discarded; the status is in c->status). Redirects are followed (301, 302,
 * 307, 308 keep method and body; 303 becomes GET). Returns PHEDAV_OK (any HTTP status that was received, including 4xx/5xx
 * -- inspect c->status) or a transport / protocol error. 401 gives PHEDAV_ERR_AUTH. */
int phedav_request(phedav *c, const char *method, const char *target, const char *extra,
                   const void *body, size_t blen, phedav_sink sink, void *sctx);

/* PROPFIND / REPORT with an XML body; the multistatus is parsed and cb is called per <response>. The top-level
 * <sync-token> (sync-collection) is copied to `top_token` (PHEDAV_TOKEN_MAX, may be NULL). c->status is the HTTP status. */
int phedav_dav(phedav *c, const char *method, const char *target, int depth, const char *xml,
               phedav_resource_cb cb, void *cbctx, char *top_token);

/* ---------------------------------------------------------------------------------------------------------------- */
/* discovery                                                                                                          */

enum { PHEDAV_CAL = 1, PHEDAV_CARD = 2 };

typedef struct {
    char href[PHEDAV_HREF_MAX];
    char name[PHEDAV_NAME_MAX];
    char ctag[PHEDAV_ETAG_MAX];
    char sync_token[PHEDAV_TOKEN_MAX];  /* the server's current token for the collection (if it reports one) */
    int kind;                       /* PHEDAV_CAL / PHEDAV_CARD */
    unsigned comps;                 /* PHEDAV_R_VEVENT | VTODO | VJOURNAL for calendars (0 = not reported = assume all) */
} phedav_collection;

typedef int (*phedav_collection_cb)(void *ctx, const phedav_collection *col);

/* url = server root ("https://dav.example.org/") or any URL below it, or a direct principal/home URL.
 * Fills the home href in home_out (PHEDAV_HREF_MAX, may be NULL) and calls cb for each collection of `kind`. */
int phedav_discover(phedav *c, int kind, const char *url, char *home_out, phedav_collection_cb cb, void *cbctx);

/* ---------------------------------------------------------------------------------------------------------------- */
/* sync state (host-persisted) and the local database (host-implemented)                                              */

typedef struct {
    char sync_token[PHEDAV_TOKEN_MAX];
    char ctag[PHEDAV_ETAG_MAX];
} phedav_meta;

typedef struct {
    char href[PHEDAV_HREF_MAX];
    char etag[PHEDAV_ETAG_MAX];
    char local_id[PHEDAV_ID_MAX];
} phedav_item_state;

/* Persisted state, one namespace per collection (key = the collection href). All return 0 on success; lookups return 1 =
 * found, 0 = absent, < 0 = error. A flat file, an SQLite table or a Symbian CDictionaryFileStore all fit. */
typedef struct {
    void *ctx;
    int (*meta_get)(void *ctx, const char *coll, phedav_meta *m);                 /* absent = leave m zeroed, return 0 */
    int (*meta_set)(void *ctx, const char *coll, const phedav_meta *m);
    int (*item_by_href)(void *ctx, const char *coll, const char *href, phedav_item_state *out);
    int (*item_by_local)(void *ctx, const char *coll, const char *local_id, phedav_item_state *out);
    int (*item_put)(void *ctx, const char *coll, const phedav_item_state *it);    /* insert or replace by href */
    int (*item_del)(void *ctx, const char *coll, const char *href);
    /* iterate every item of the collection; stop (and return) when cb returns nonzero */
    int (*item_each)(void *ctx, const char *coll, int (*cb)(void *cbctx, const phedav_item_state *it), void *cbctx);
} phedav_store;

enum { PHEDAV_CH_ADD = 1, PHEDAV_CH_MOD = 2, PHEDAV_CH_DEL = 3 };

/* The device's own database. A write()/remove() issued by the core is NOT a local change (the host must not report it back). */
typedef struct {
    void *ctx;
    /* calls cb(local_id, kind) for every change not yet acknowledged; stops when cb returns nonzero */
    int (*changes)(void *ctx, const char *coll, int (*cb)(void *cbctx, const char *local_id, int kind), void *cbctx);
    /* serialises the item as vCard / iCalendar text into buf; *len = size (may exceed cap: then PHEDAV_ERR_BUF) */
    int (*read)(void *ctx, const char *coll, const char *local_id, char *buf, size_t cap, size_t *len);
    /* creates (local_id = "") or replaces an item from text; new_id receives the id (PHEDAV_ID_MAX) */
    int (*write)(void *ctx, const char *coll, const char *local_id, const char *data, size_t len, char *new_id);
    int (*remove)(void *ctx, const char *coll, const char *local_id);
    /* the change for local_id has been dealt with (uploaded, or superseded by the server's version) */
    int (*ack)(void *ctx, const char *coll, const char *local_id);
    /* optional: both sides changed the same item. Return PHEDAV_USE_SERVER or PHEDAV_USE_LOCAL (NULL = use the policy) */
    int (*conflict)(void *ctx, const char *coll, const char *href, const char *local_id, int local_kind, int server_deleted);
} phedav_local;

enum { PHEDAV_USE_SERVER = 0, PHEDAV_USE_LOCAL = 1 };

typedef struct {
    int conflict_policy;            /* PHEDAV_USE_SERVER (default 0) or PHEDAV_USE_LOCAL */
    int no_sync_collection;         /* force the ctag + etag listing path (servers without RFC 6578) */
    int download_only;              /* do not upload local changes */
    int force_full;                 /* ignore the stored token and ctag: full listing, deletion detection */
} phedav_syncopts;

typedef struct {
    const char *mode;               /* "sync-collection", "full", "listing", "unchanged" */
    int token_expired;              /* the stored sync token was rejected and a full listing was done */
    int down_new, down_mod, down_del;
    int up_new, up_mod, up_del;
    int conflicts;                  /* resolved by policy */
    int deferred;                   /* upload refused with 412: retried on the next sync */
    int errors;                     /* items skipped (too large, callback failed, ...); the token is not advanced */
    int http_requests;
} phedav_stats;

/* One two-way sync of one collection. */
int phedav_sync(phedav *c, const phedav_collection *col, phedav_store *st, phedav_local *lo,
                const phedav_syncopts *opt, phedav_stats *stats);

/* ---------------------------------------------------------------------------------------------------------------- */
/* helpers                                                                                                            */

/* RFC 4648 base64 encode into out (NUL-terminated); returns the length or 0 if cap is too small */
size_t phedav_base64(const void *in, size_t n, char *out, size_t cap);
/* extracts the UID property value of a vCard / iCalendar text (first match, unfolded); returns its length, 0 if none */
size_t phedav_item_uid(const char *text, size_t n, char *out, size_t cap);
/* reduces an absolute URL to its path; path hrefs are copied unchanged */
void phedav_href_norm(const char *in, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
