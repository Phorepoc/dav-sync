// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
//
// phedav.exe -- CardDAV (and later CalDAV) sync client for a Symbian^3 phone. Console exe, user capabilities only.
//   phedav.exe <url> <user> <password> [contacts|calendar] --db <file> | --real-db [--filter <name>]
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
	const char *filter = NULL, *dbname = NULL, *selftest = NULL;
	int local_wins = 0;
	int real_db = 0, cal;
	for (i = 5; i < argc; i++) {
		if (!strcmp(argv[i], "--db") && i + 1 < argc) dbname = argv[++i];
		else if (!strcmp(argv[i], "--selftest") && i + 1 < argc) selftest = argv[++i];
		else if (!strcmp(argv[i], "--local-wins")) local_wins = 1;
		else if (!strcmp(argv[i], "--real-db")) real_db = 1;
		else if (!strcmp(argv[i], "--filter") && i + 1 < argc) filter = argv[++i];
		else { printf("unknown argument %s\n", argv[i]); return 2; }
	}
	time_t now = time(NULL);

	printf("---- phedav %s start %s", PHEDAV_VERSION, ctime(&now));
	if (argc < 4) {
		printf("usage: phedav <url> <user> <password> [contacts|calendar] --db <file> | --real-db  [--filter <name>]\n");
		return 2;
	}
	/* SAFETY (test builds): never touch the phone's real contacts unless asked to explicitly */
	if (!dbname && !real_db) {
		printf("REFUSED: pass --db <file> (a separate test database, e.g. --db c:phedavtest.cdb) or --real-db\n");
		return 2;
	}
	if (dbname && real_db) { printf("REFUSED: --db and --real-db together\n"); return 2; }
	printf("%s database: %s\n", what, dbname ? dbname : "THE REAL DEFAULT DATABASE");
	cal = !strcmp(what, "calendar");
	if (!cal && strcmp(what, "contacts")) { printf("unknown data type %s\n", what); return 2; }
	kind = cal ? PHEDAV_CAL : PHEDAV_CARD;
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

	if (fstore_open(f->col[pick].href, dbname)) { printf("cannot open the state file\n"); return 6; }
	if (cal ? calendar_open(dbname) : contacts_open(dbname)) { fstore_close(); return 7; }
	if (selftest) {      /* --selftest "op:a[:b],op:a[:b],..." runs on the --db TEST database before the sync */
		char ops[400], *p, *q;
		if (!dbname) { printf("REFUSED: --selftest needs --db\n"); return 2; }
		printf("selftest: ops \"%s\"\n", selftest);
		strncpy(ops, selftest, sizeof ops - 1); ops[sizeof ops - 1] = 0;
		for (p = ops; p && *p; p = q) {
			char *a, *b = NULL;
			q = strchr(p, ','); if (q) *q++ = 0;
			a = strchr(p, ':'); if (a) { *a++ = 0; b = strchr(a, ':'); if (b) *b++ = 0; }
			if (cal) calendar_selftest(p, a ? a : "", b ? b : ""); else contacts_selftest(p, a ? a : "", b ? b : "");
		}
	}
	store = fstore_store();
	local = cal ? calendar_local() : contacts_local();
	memset(&opt, 0, sizeof opt);
	opt.conflict_policy = local_wins ? PHEDAV_USE_LOCAL : PHEDAV_USE_SERVER;
	r = phedav_sync(c, &f->col[pick], &store, &local, &opt, &st);
	printf("sync: result %d (http %d) mode=%s%s\n", r, c->status, st.mode, st.token_expired ? " [token expired]" : "");
	printf("  down +%d ~%d -%d   up +%d ~%d -%d   conflicts=%d deferred=%d errors=%d   http requests=%d\n",
	       st.down_new, st.down_mod, st.down_del, st.up_new, st.up_mod, st.up_del, st.conflicts, st.deferred, st.errors, st.http_requests);
	if (cal) calendar_close(); else contacts_close();
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
	/* phedav --args <file>: the arguments come from a text file (whitespace separated; "#" starts a comment line) -- a launcher with a
	   short command-line / list-file limit can then start it with one short line */
	if (argc >= 3 && !strcmp(argv[1], "--args")) {
		static char text[3000];
		static char *av[64];
		FILE *f = fopen(argv[2], "rb");
		int n = 0, ac = 0;
		char *p;
		if (!f) { printf("cannot open the argument file %s\n", argv[2]); return 2; }
		n = (int)fread(text, 1, sizeof text - 1, f);
		fclose(f);
		text[n] = 0;
		av[ac++] = argv[0];
		for (p = text; *p && ac < 63; ) {
			while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
			if (*p == '#') { while (*p && *p != '\n') p++; continue; }
			if (!*p) break;
			av[ac++] = p;
			while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
			if (*p) *p++ = 0;
		}
		argc = ac; argv = av;
	}
	rc = run(argc, argv);
	delete sched;
	delete cleanup;
	return rc;
}
