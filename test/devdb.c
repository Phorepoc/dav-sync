/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */
#include "devdb.h"

#include <stdio.h>
#include <string.h>

void dev_init(devdb *d) { memset(d, 0, sizeof *d); }

static dev_item *by_id(devdb *d, const char *id)
{
    int i;
    for (i = 0; i < DEV_MAX; i++) if (d->items[i].used && !strcmp(d->items[i].id, id)) return &d->items[i];
    return NULL;
}

const char *dev_add(devdb *d, const char *data)
{
    int i;
    for (i = 0; i < DEV_MAX; i++) {
        if (!d->items[i].used) {
            d->items[i].used = 1;
            snprintf(d->items[i].id, sizeof d->items[i].id, "L%d", ++d->next_id);
            snprintf(d->items[i].data, DEV_DATA, "%s", data);
            d->items[i].dirty = PHEDAV_CH_ADD;
            return d->items[i].id;
        }
    }
    return NULL;
}

void dev_mod(devdb *d, const char *id, const char *data)
{
    dev_item *it = by_id(d, id);
    if (!it) return;
    snprintf(it->data, DEV_DATA, "%s", data);
    if (it->dirty == 0) it->dirty = PHEDAV_CH_MOD;
}

void dev_del(devdb *d, const char *id)
{
    dev_item *it = by_id(d, id);
    if (!it) return;
    if (it->dirty == PHEDAV_CH_ADD) it->used = 0;
    else it->dirty = PHEDAV_CH_DEL;
}

int dev_count(const devdb *d)
{
    int i, n = 0;
    for (i = 0; i < DEV_MAX; i++) if (d->items[i].used && d->items[i].dirty != PHEDAV_CH_DEL) n++;
    return n;
}

int dev_dirty_count(const devdb *d)
{
    int i, n = 0;
    for (i = 0; i < DEV_MAX; i++) if (d->items[i].used && d->items[i].dirty) n++;
    return n;
}

const char *dev_find_containing(const devdb *d, const char *needle, const char **id_out)
{
    int i;
    for (i = 0; i < DEV_MAX; i++) {
        if (d->items[i].used && d->items[i].dirty != PHEDAV_CH_DEL && strstr(d->items[i].data, needle)) {
            if (id_out) *id_out = d->items[i].id;
            return d->items[i].data;
        }
    }
    return NULL;
}

/* ---- store */
static int s_meta_get(void *ctx, const char *coll, phedav_meta *m) { (void)coll; *m = ((devdb *)ctx)->meta; return 0; }
static int s_meta_set(void *ctx, const char *coll, const phedav_meta *m) { (void)coll; ((devdb *)ctx)->meta = *m; return 0; }

static int s_by_href(void *ctx, const char *coll, const char *href, phedav_item_state *out)
{
    devdb *d = (devdb *)ctx;
    int i;
    (void)coll;
    for (i = 0; i < DEV_MAX; i++) if (d->st_used[i] && !strcmp(d->st[i].href, href)) { *out = d->st[i]; return 1; }
    return 0;
}

static int s_by_local(void *ctx, const char *coll, const char *id, phedav_item_state *out)
{
    devdb *d = (devdb *)ctx;
    int i;
    (void)coll;
    for (i = 0; i < DEV_MAX; i++) if (d->st_used[i] && !strcmp(d->st[i].local_id, id)) { *out = d->st[i]; return 1; }
    return 0;
}

static int s_put(void *ctx, const char *coll, const phedav_item_state *it)
{
    devdb *d = (devdb *)ctx;
    int i, free_i = -1;
    (void)coll;
    for (i = 0; i < DEV_MAX; i++) {
        if (d->st_used[i] && !strcmp(d->st[i].href, it->href)) { d->st[i] = *it; return 0; }
        if (!d->st_used[i] && free_i < 0) free_i = i;
    }
    if (free_i < 0) return -1;
    d->st[free_i] = *it;
    d->st_used[free_i] = 1;
    return 0;
}

static int s_del(void *ctx, const char *coll, const char *href)
{
    devdb *d = (devdb *)ctx;
    int i;
    (void)coll;
    for (i = 0; i < DEV_MAX; i++) if (d->st_used[i] && !strcmp(d->st[i].href, href)) d->st_used[i] = 0;
    return 0;
}

static int s_each(void *ctx, const char *coll, int (*cb)(void *, const phedav_item_state *), void *cbctx)
{
    devdb *d = (devdb *)ctx;
    int i;
    (void)coll;
    for (i = 0; i < DEV_MAX; i++) {
        if (d->st_used[i]) {
            phedav_item_state c = d->st[i];
            if (cb(cbctx, &c)) break;
        }
    }
    return 0;
}

phedav_store dev_store(devdb *d)
{
    phedav_store s;
    s.ctx = d; s.meta_get = s_meta_get; s.meta_set = s_meta_set; s.item_by_href = s_by_href; s.item_by_local = s_by_local;
    s.item_put = s_put; s.item_del = s_del; s.item_each = s_each;
    return s;
}

/* ---- local */
static int l_changes(void *ctx, const char *coll, int (*cb)(void *, const char *, int), void *cbctx)
{
    devdb *d = (devdb *)ctx;
    int i;
    (void)coll;
    for (i = 0; i < DEV_MAX; i++) {
        if (d->items[i].used && d->items[i].dirty) { if (cb(cbctx, d->items[i].id, d->items[i].dirty)) break; }
    }
    return 0;
}

static int l_read(void *ctx, const char *coll, const char *id, char *buf, size_t cap, size_t *len)
{
    devdb *d = (devdb *)ctx;
    dev_item *it = by_id(d, id);
    (void)coll;
    if (!it) return -1;
    *len = strlen(it->data);
    if (*len > cap) return -1;
    memcpy(buf, it->data, *len);
    return 0;
}

static int l_write(void *ctx, const char *coll, const char *id, const char *data, size_t len, char *new_id)
{
    devdb *d = (devdb *)ctx;
    dev_item *it;
    int i;
    (void)coll;
    if (len >= DEV_DATA) return -1;
    if (id[0] && (it = by_id(d, id)) != NULL) {
        memcpy(it->data, data, len);
        it->data[len] = 0;
        it->dirty = 0;
        strcpy(new_id, it->id);
        return 0;
    }
    for (i = 0; i < DEV_MAX; i++) {
        if (!d->items[i].used) {
            it = &d->items[i];
            it->used = 1;
            snprintf(it->id, sizeof it->id, "L%d", ++d->next_id);
            memcpy(it->data, data, len);
            it->data[len] = 0;
            it->dirty = 0;
            strcpy(new_id, it->id);
            return 0;
        }
    }
    return -1;
}

static int l_remove(void *ctx, const char *coll, const char *id)
{
    devdb *d = (devdb *)ctx;
    dev_item *it = by_id(d, id);
    (void)coll;
    if (it) it->used = 0;
    return 0;
}

static int l_ack(void *ctx, const char *coll, const char *id)
{
    devdb *d = (devdb *)ctx;
    dev_item *it = by_id(d, id);
    (void)coll;
    if (!it) return 0;
    if (it->dirty == PHEDAV_CH_DEL) it->used = 0;
    else it->dirty = 0;
    return 0;
}

phedav_local dev_local(devdb *d)
{
    phedav_local l;
    memset(&l, 0, sizeof l);
    l.ctx = d; l.changes = l_changes; l.read = l_read; l.write = l_write; l.remove = l_remove; l.ack = l_ack;
    return l;
}
