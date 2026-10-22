/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef DEVDB_H
#define DEVDB_H
#include "phedav.h"

/* An in-memory "device": a tiny contacts / calendar database with dirty tracking, plus the sync state store.
 * Single collection per instance (the collection key is ignored). */
#define DEV_MAX 64
#define DEV_DATA 16384

typedef struct {
    int used;
    char id[PHEDAV_ID_MAX];
    char data[DEV_DATA];
    int dirty;                  /* 0, PHEDAV_CH_ADD / MOD / DEL (a DEL keeps a tombstone) */
} dev_item;

typedef struct {
    dev_item items[DEV_MAX];
    int next_id;
    phedav_meta meta;
    phedav_item_state st[DEV_MAX];
    int st_used[DEV_MAX];
} devdb;

void dev_init(devdb *d);
phedav_store dev_store(devdb *d);
phedav_local dev_local(devdb *d);
/* user actions */
const char *dev_add(devdb *d, const char *data);          /* returns the new id */
void dev_mod(devdb *d, const char *id, const char *data);
void dev_del(devdb *d, const char *id);
int dev_count(const devdb *d);                            /* live (non-tombstone) items */
/* the data of the first live item that contains `needle`, or NULL; its id goes to id_out */
const char *dev_find_containing(const devdb *d, const char *needle, const char **id_out);
int dev_dirty_count(const devdb *d);
#endif
