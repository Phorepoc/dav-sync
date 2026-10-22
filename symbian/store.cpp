// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
//
// File-backed sync state under C:\data\phedav\. One text file per collection (name = FNV-1a of the collection href):
//   M <tab> sync_token <tab> ctag             collection meta (last one wins)
//   I <tab> href <tab> etag <tab> local_id    item row (last one for an href wins)
//   D <tab> href                              item removed
//   S <tab> local_id <tab> sig                contact signature (hash of the exported vCard after the last sync)
//   X <tab> local_id                          signature removed
// Every mutation is appended and flushed (a crash loses nothing); fstore_open replays and rewrites the file compacted.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "phedav_sym.h"

#define MAX_ITEMS 2500
#define MAX_SIGS 2500

typedef struct { char id[PHEDAV_ID_MAX]; unsigned long sig; } sigrow;

static phedav_item_state *g_items;
static int g_nitems;
static sigrow *g_sigs;
static int g_nsigs;
static phedav_meta g_meta;
static FILE *g_f;
static char g_path[96];

static int find_item(const char *href)
{
	int i;
	for (i = 0; i < g_nitems; i++) if (!strcmp(g_items[i].href, href)) return i;
	return -1;
}

static int find_sig(const char *id)
{
	int i;
	for (i = 0; i < g_nsigs; i++) if (!strcmp(g_sigs[i].id, id)) return i;
	return -1;
}

static void mem_put(const phedav_item_state *it)
{
	int i = find_item(it->href);
	if (i < 0) {
		if (g_nitems >= MAX_ITEMS) return;
		i = g_nitems++;
	}
	g_items[i] = *it;
}

static void mem_del(const char *href)
{
	int i = find_item(href);
	if (i >= 0) g_items[i] = g_items[--g_nitems];
}

static void mem_sig(const char *id, unsigned long sig)
{
	int i = find_sig(id);
	if (i < 0) {
		if (g_nsigs >= MAX_SIGS) return;
		i = g_nsigs++;
		snprintf(g_sigs[i].id, sizeof g_sigs[i].id, "%s", id);
	}
	g_sigs[i].sig = sig;
}

static void mem_sigdel(const char *id)
{
	int i = find_sig(id);
	if (i >= 0) g_sigs[i] = g_sigs[--g_nsigs];
}

static void cp(char *dst, size_t cap, const char *s) { snprintf(dst, cap, "%s", s ? s : ""); }

int fstore_open(const char *coll, const char *tag)
{
	unsigned long h = 2166136261UL;
	const char *p;
	FILE *f;
	char line[1200];
	int i;
	for (p = coll; *p; p++) { h ^= (unsigned char)*p; h *= 16777619UL; }
	for (p = tag ? tag : ""; *p; p++) { h ^= (unsigned char)*p; h *= 16777619UL; }
	snprintf(g_path, sizeof g_path, PHEDAV_DIR "\\state_%08lx.txt", h);
	g_items = (phedav_item_state *)calloc(MAX_ITEMS, sizeof *g_items);
	g_sigs = (sigrow *)calloc(MAX_SIGS, sizeof *g_sigs);
	if (!g_items || !g_sigs) return -1;
	g_nitems = g_nsigs = 0;
	memset(&g_meta, 0, sizeof g_meta);
	f = fopen(g_path, "r");
	if (f) {
		while (fgets(line, sizeof line, f)) {
			char *fld[4] = { 0, 0, 0, 0 };
			int n = 0;
			char *q = line;
			line[strcspn(line, "\r\n")] = 0;
			while (n < 4) {
				fld[n++] = q;
				q = strchr(q, '\t');
				if (!q) break;
				*q++ = 0;
			}
			if (n < 1 || !fld[0][0]) continue;
			switch (fld[0][0]) {
			case 'M':
				if (n >= 3) { cp(g_meta.sync_token, sizeof g_meta.sync_token, fld[1]); cp(g_meta.ctag, sizeof g_meta.ctag, fld[2]); }
				break;
			case 'I':
				if (n >= 4) {
					phedav_item_state it;
					memset(&it, 0, sizeof it);
					cp(it.href, sizeof it.href, fld[1]);
					cp(it.etag, sizeof it.etag, fld[2]);
					cp(it.local_id, sizeof it.local_id, fld[3]);
					mem_put(&it);
				}
				break;
			case 'D': if (n >= 2) mem_del(fld[1]); break;
			case 'S': if (n >= 3) mem_sig(fld[1], strtoul(fld[2], NULL, 16)); break;
			case 'X': if (n >= 2) mem_sigdel(fld[1]); break;
			}
		}
		fclose(f);
	}
	/* compact: rewrite the replayed state, then keep the file open for appends */
	g_f = fopen(g_path, "w");
	if (!g_f) return -1;
	fprintf(g_f, "M\t%s\t%s\n", g_meta.sync_token, g_meta.ctag);
	for (i = 0; i < g_nitems; i++) fprintf(g_f, "I\t%s\t%s\t%s\n", g_items[i].href, g_items[i].etag, g_items[i].local_id);
	for (i = 0; i < g_nsigs; i++) fprintf(g_f, "S\t%s\t%lx\n", g_sigs[i].id, g_sigs[i].sig);
	fflush(g_f);
	printf("store: %s: %d items, %d signatures, token %s\n", g_path, g_nitems, g_nsigs, g_meta.sync_token[0] ? "yes" : "none");
	return 0;
}

void fstore_close(void)
{
	if (g_f) fclose(g_f);
	g_f = NULL;
	free(g_items);
	free(g_sigs);
	g_items = NULL;
	g_sigs = NULL;
}

/* ---- phedav_store */
static int s_meta_get(void *c, const char *coll, phedav_meta *m) { (void)c; (void)coll; *m = g_meta; return 0; }

static int s_meta_set(void *c, const char *coll, const phedav_meta *m)
{
	(void)c; (void)coll;
	g_meta = *m;
	if (g_f) { fprintf(g_f, "M\t%s\t%s\n", m->sync_token, m->ctag); fflush(g_f); }
	return 0;
}

static int s_by_href(void *c, const char *coll, const char *href, phedav_item_state *out)
{
	int i = find_item(href);
	(void)c; (void)coll;
	if (i < 0) return 0;
	*out = g_items[i];
	return 1;
}

static int s_by_local(void *c, const char *coll, const char *id, phedav_item_state *out)
{
	int i;
	(void)c; (void)coll;
	for (i = 0; i < g_nitems; i++) if (!strcmp(g_items[i].local_id, id)) { *out = g_items[i]; return 1; }
	return 0;
}

static int s_put(void *c, const char *coll, const phedav_item_state *it)
{
	(void)c; (void)coll;
	if (find_item(it->href) < 0 && g_nitems >= MAX_ITEMS) return -1;
	mem_put(it);
	if (g_f) { fprintf(g_f, "I\t%s\t%s\t%s\n", it->href, it->etag, it->local_id); fflush(g_f); }
	return 0;
}

static int s_del(void *c, const char *coll, const char *href)
{
	(void)c; (void)coll;
	mem_del(href);
	if (g_f) { fprintf(g_f, "D\t%s\n", href); fflush(g_f); }
	return 0;
}

static int s_each(void *c, const char *coll, int (*cb)(void *, const phedav_item_state *), void *cbctx)
{
	int i;
	(void)c; (void)coll;
	for (i = 0; i < g_nitems; i++) {
		phedav_item_state copy = g_items[i];
		if (cb(cbctx, &copy)) break;
	}
	return 0;
}

phedav_store fstore_store(void)
{
	phedav_store s;
	s.ctx = NULL; s.meta_get = s_meta_get; s.meta_set = s_meta_set; s.item_by_href = s_by_href; s.item_by_local = s_by_local;
	s.item_put = s_put; s.item_del = s_del; s.item_each = s_each;
	return s;
}

/* ---- signatures */
int fsig_get(const char *id, unsigned long *sig)
{
	int i = find_sig(id);
	if (i < 0) return 0;
	*sig = g_sigs[i].sig;
	return 1;
}

void fsig_set(const char *id, unsigned long sig)
{
	mem_sig(id, sig);
	if (g_f) { fprintf(g_f, "S\t%s\t%lx\n", id, sig); fflush(g_f); }
}

void fsig_del(const char *id)
{
	mem_sigdel(id);
	if (g_f) { fprintf(g_f, "X\t%s\n", id); fflush(g_f); }
}

void fsig_each(int (*cb)(void *, const char *, unsigned long), void *ctx)
{
	int i;
	for (i = 0; i < g_nsigs; i++) {
		sigrow r = g_sigs[i];
		if (cb(ctx, r.id, r.sig)) break;
	}
}
