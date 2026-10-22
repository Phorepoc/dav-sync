// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
//
// phedav.exe -- CardDAV (and later CalDAV) sync client for a Symbian^3 phone. Console exe, user capabilities only.
//   phedav.exe <url> <user> <password> [contacts|calendar] [collection-name-filter]
// Log: C:\data\phedav\phedav.log. State: C:\data\phedav\state_*.txt. Optional files in C:\data\phedav\:
//   iap.txt (access point name), ca.pem (PEM trust anchors for https), pin.txt (64 hex digits: SHA-256 of the server cert).
#include <e32base.h>
#include <e32std.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "phedav_sym.h"

#define ITEM_BUF (192 * 1024)
#define WORK_BUF (192 * 1024)
#define MAX_COLL 12

typedef struct {
	phedav_collection col[MAX_COLL];
	int n;
} found;

static int on_collection(void *ctx, const phedav_collection *c)
{
	found *f = (found *)ctx;
	printf("  collection %s  name=\"%s\" ctag=%s token=%s\n", c->href, c->name, c->ctag[0] ? c->ctag : "-", c->sync_token[0] ? "yes" : "no");
	if (f->n < MAX_COLL) f->col[f->n++] = *c;
	return 0;
}

static int run(int argc, char **argv)
{
	phedav *c;
	char *item, *work;
	found *f;
	phedav_stats st;
	phedav_syncopts opt;
	phedav_store store;
	phedav_local local;
	int r, kind, pick = 0, i;
	const char *what = argc > 4 ? argv[4] : "contacts";
	const char *filter = argc > 5 ? argv[5] : NULL;
	time_t now = time(NULL);

	printf("---- phedav %s start %s", PHEDAV_VERSION, ctime(&now));
	if (argc < 4) {
		printf("usage: phedav <url> <user> <password> [contacts|calendar] [name-filter]\n");
		return 2;
	}
	if (!strcmp(what, "calendar")) {
		printf("calendar sync is not implemented yet (contacts only in this build)\n");
		return 3;
	}
	kind = PHEDAV_CARD;
	c = (phedav *)calloc(1, sizeof *c);
	item = (char *)malloc(ITEM_BUF);
	work = (char *)malloc(WORK_BUF);
	f = (found *)calloc(1, sizeof *f);
	if (!c || !item || !work || !f) { printf("out of memory\n"); return 4; }
	plat_net_init();
	if (phedav_init(c, plat_make_net(), argv[2], argv[3], item, ITEM_BUF, work, WORK_BUF)) { printf("bad credentials\n"); return 2; }

	printf("discovering %s ...\n", argv[1]);
	r = phedav_discover(c, kind, argv[1], NULL, on_collection, f);
	printf("discover: %d (http %d), %d collection(s)\n", r, c->status, f->n);
	if (r < 0 || f->n == 0) {
		printf("nothing to sync (discovery failed or no address book)\n");
		return 5;
	}
	if (filter) {
		for (i = 0; i < f->n; i++) if (strstr(f->col[i].name, filter) || strstr(f->col[i].href, filter)) { pick = i; break; }
	}
	printf("syncing %s\n", f->col[pick].href);

	if (fstore_open(f->col[pick].href)) { printf("cannot open the state file\n"); return 6; }
	if (contacts_open()) { fstore_close(); return 7; }
	store = fstore_store();
	local = contacts_local();
	memset(&opt, 0, sizeof opt);
	r = phedav_sync(c, &f->col[pick], &store, &local, &opt, &st);
	printf("sync: result %d (http %d) mode=%s%s\n", r, c->status, st.mode, st.token_expired ? " [token expired]" : "");
	printf("  down +%d ~%d -%d   up +%d ~%d -%d   conflicts=%d deferred=%d errors=%d   http requests=%d\n",
	       st.down_new, st.down_mod, st.down_del, st.up_new, st.up_mod, st.up_del, st.conflicts, st.deferred, st.errors, st.http_requests);
	contacts_close();
	fstore_close();
	phedav_close(c);
	free(f); free(work); free(item); free(c);
	now = time(NULL);
	printf("---- done %s", ctime(&now));
	return r == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
	CTrapCleanup *cleanup = NULL;
	CActiveScheduler *sched = NULL;
	int rc;
	plat_log_open();
	if (!User::TrapHandler()) cleanup = CTrapCleanup::New();
	if (!CActiveScheduler::Current()) {
		sched = new CActiveScheduler;
		if (sched) CActiveScheduler::Install(sched);
	}
	rc = run(argc, argv);
	delete sched;
	delete cleanup;
	return rc;
}
