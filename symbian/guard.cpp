// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
//
// Safety layer between the sync core and the platform adapters (contacts and calendar alike):
//  * mass-delete guard: a sync that would delete a lot (or everything the state remembers) is refused for deletions, because a
//    local store that momentarily reads empty must never be turned into deletions on the server (or the reverse: a server that
//    answers with a truncated listing into deletions on the phone). Limits: more than max_del deletions, or (state of >= 2 items)
//    all of them, or (state of >= 4 items) more than half of them. --allow-mass-delete lifts it, --max-delete N changes the limit.
//  * --dry-run: prints what would be uploaded, written locally and removed; nothing is sent to the server or written to the
//    local database or to the sync state.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "phedav_sym.h"

static phedav_local g_real;
static phedav_store g_rstore;
static int g_dry, g_allow, g_maxdel, g_down_del;

struct Item { char id[PHEDAV_ID_MAX]; int kind; };
struct Collect { Item *v; int n, cap; };

static int collect_cb(void *ctx, const char *id, int kind)
{
	Collect *c = (Collect *)ctx;
	if (c->n == c->cap) {
		int nc = c->cap ? c->cap * 2 : 64;
		Item *nv = (Item *)realloc(c->v, nc * sizeof(Item));
		if (!nv) return 1;
		c->v = nv; c->cap = nc;
	}
	strncpy(c->v[c->n].id, id, PHEDAV_ID_MAX - 1);
	c->v[c->n].id[PHEDAV_ID_MAX - 1] = 0;
	c->v[c->n].kind = kind;
	c->n++;
	return 0;
}

static int count_cb(void *ctx, const phedav_item_state *it) { (void)it; (*(int *)ctx)++; return 0; }

static int g_state_items(const char *coll)
{
	int n = 0;
	if (g_rstore.item_each) g_rstore.item_each(g_rstore.ctx, coll, count_cb, &n);
	return n;
}

static int w_changes(void *ctx, const char *coll, int (*cb)(void *, const char *, int), void *cbctx)
{
	Collect c = { NULL, 0, 0 };
	int r, i, dels = 0, state, block;
	(void)ctx;
	r = g_real.changes(g_real.ctx, coll, collect_cb, &c);
	if (r) { free(c.v); return r; }
	for (i = 0; i < c.n; i++) if (c.v[i].kind == PHEDAV_CH_DEL) dels++;
	state = g_state_items(coll);
	block = !g_allow && dels > 0 && (dels > g_maxdel || (state >= 2 && dels >= state) || (state >= 4 && dels * 2 > state));
	if (block)
		printf("guard: REFUSING %d local deletion(s) (state has %d item(s), limit %d): not sent to the server. Check the local database, or pass --allow-mass-delete\n",
		       dels, state, g_maxdel);
	for (i = 0; i < c.n; i++) {
		const char *k = c.v[i].kind == PHEDAV_CH_ADD ? "add" : c.v[i].kind == PHEDAV_CH_MOD ? "modify" : "delete";
		if (c.v[i].kind == PHEDAV_CH_DEL && block) continue;
		if (g_dry) { printf("dry-run: would upload %s of local item %s\n", k, c.v[i].id); continue; }
		if (cb(cbctx, c.v[i].id, c.v[i].kind)) break;
	}
	free(c.v);
	return 0;
}

static int w_read(void *ctx, const char *coll, const char *id, char *buf, size_t cap, size_t *len)
{ (void)ctx; return g_real.read(g_real.ctx, coll, id, buf, cap, len); }

static int w_write(void *ctx, const char *coll, const char *local_id, const char *data, size_t len, char *new_id)
{
	(void)ctx;
	if (g_dry) {
		printf("dry-run: would write %s item %s locally (%d bytes)\n", local_id[0] ? "changed" : "new", local_id[0] ? local_id : "-", (int)len);
		strcpy(new_id, local_id[0] ? local_id : "dry");
		return 0;
	}
	return g_real.write(g_real.ctx, coll, local_id, data, len, new_id);
}

static int w_remove(void *ctx, const char *coll, const char *id)
{
	(void)ctx;
	if (g_dry) { printf("dry-run: would remove local item %s\n", id); return 0; }
	g_down_del++;
	if (!g_allow && g_down_del > g_maxdel) {
		printf("guard: REFUSING to remove local item %s (%d server-side deletions in this sync, limit %d; --allow-mass-delete overrides)\n",
		       id, g_down_del, g_maxdel);
		return -1;
	}
	return g_real.remove(g_real.ctx, coll, id);
}

static int w_ack(void *ctx, const char *coll, const char *id)
{ (void)ctx; if (g_dry) return 0; return g_real.ack(g_real.ctx, coll, id); }

static int s_meta_set(void *c, const char *coll, const phedav_meta *m) { (void)c; (void)coll; (void)m; return 0; }
static int s_item_put(void *c, const char *coll, const phedav_item_state *it) { (void)c; (void)coll; (void)it; return 0; }
static int s_item_del(void *c, const char *coll, const char *href) { (void)c; (void)coll; (void)href; return 0; }

void guard_wrap(phedav_local *lo, phedav_store *st, int dry, int allow_mass, int max_del)
{
	g_real = *lo; g_rstore = *st;
	g_dry = dry; g_allow = allow_mass; g_maxdel = max_del > 0 ? max_del : 5; g_down_del = 0;
	lo->ctx = NULL;
	lo->changes = w_changes; lo->read = w_read; lo->write = w_write; lo->remove = w_remove; lo->ack = w_ack;
	if (dry) { st->meta_set = s_meta_set; st->item_put = s_item_put; st->item_del = s_item_del; }
}
