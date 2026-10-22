// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
//
// phedav_local for the phone's calendar: iCalendar import / export through CCalDataExchange (KUidVCalendar), entries through
// CCalEntryView. The item is a UID group (a recurring event with its exceptions is several CCalEntry with one UID and is one
// CalDAV resource); the local id is the iCalendar UID itself (up to 47 characters; longer UIDs are skipped and logged).
//
// Change detection by signature, as for contacts: every UID (CCalIter) is exported, hashed and compared with the signature
// stored after the last sync; a stored UID that no longer exists is a deletion. The core's own writes record their signature.
#include <e32base.h>
#include <e32std.h>
#include <s32mem.h>
#include <calsession.h>
#include <calentry.h>
#include <calentryview.h>
#include <caliterator.h>
#include <caldataexchange.h>
#include <caldataformat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "phedav_sym.h"

static CCalSession *g_ses;
static CCalEntryView *g_view;
static CCalDataExchange *g_exch;
static CBufFlat *g_buf;

static void OpenL(const char *aFile)
{
	g_ses = CCalSession::NewL();
	TFileName fn;
	fn.Copy(TPtrC8((const TUint8 *)aFile));
	TRAPD(e, g_ses->CreateCalFileL(fn));
	if (e == KErrNone) printf("calendar: created %s\n", aFile);
	else if (e != KErrAlreadyExists) User::Leave(e);
	g_ses->OpenL(fn);
	g_view = CCalEntryView::NewL(*g_ses);
	g_exch = CCalDataExchange::NewL(*g_ses);
	g_buf = CBufFlat::NewL(2048);
}

int calendar_open(const char *aFile)
{
	if (!aFile) { printf("calendar: --real-db is not supported for the calendar yet (use --db c:name.cal)\n"); return KErrNotSupported; }
	TRAPD(err, OpenL(aFile));
	if (err) printf("calendar: open %s failed %d\n", aFile, err);
	return err;
}

void calendar_close(void)
{
	delete g_buf; g_buf = NULL;
	delete g_exch; g_exch = NULL;
	delete g_view; g_view = NULL;
	delete g_ses; g_ses = NULL;
}

static unsigned long fnv(const TUint8 *p, TInt n)
{
	unsigned long h = 2166136261UL;
	while (n-- > 0) { h ^= *p++; h *= 16777619UL; }
	return h;
}

static void ResetAndDestroyArr(TAny *a)
{
	RPointerArray<CCalEntry> *arr = (RPointerArray<CCalEntry> *)a;
	arr->ResetAndDestroy();
	arr->Close();
}

// exports the UID group into g_buf; aCount = number of entries found (0 = no such UID)
static void ExportGroupL(const TDesC8 &aUid, TInt &aCount)
{
	RPointerArray<CCalEntry> arr;
	CleanupStack::PushL(TCleanupItem(ResetAndDestroyArr, &arr));
	g_view->FetchL(aUid, arr);
	aCount = arr.Count();
	g_buf->Reset();
	if (aCount > 0) {
		RBufWriteStream ws(*g_buf);
		CleanupClosePushL(ws);
		g_exch->ExportL(KUidVCalendar, ws, arr);
		ws.CommitL();
		CleanupStack::PopAndDestroy();   // ws
	}
	CleanupStack::PopAndDestroy();       // arr
}

static TInt SignatureOf(const TDesC8 &aUid, unsigned long &aSig, TInt &aCount)
{
	TRAPD(err, ExportGroupL(aUid, aCount));
	if (err) return err;
	TInt n = g_buf->Size();
	TUint8 *tmp = (TUint8 *)User::Alloc(n ? n : 1);
	if (!tmp) return KErrNoMemory;
	TPtr8 d(tmp, 0, n);
	g_buf->Read(0, d, n);
	aSig = fnv(tmp, n);
	User::Free(tmp);
	return KErrNone;
}

static TPtrC8 UidDes(const char *id) { return TPtrC8((const TUint8 *)id, (TInt)strlen(id)); }

struct TScan { int (*cb)(void *, const char *, int); void *cbctx; TInt added, modified; TBool stop; };

static void ScanL(TScan &sc)
{
	CCalIter *it = CCalIter::NewL(*g_ses);
	CleanupStack::PushL(it);
	// copy the UIDs first: the signature export fetches entries while the iterator is alive
	RArray<TBuf8<PHEDAV_ID_MAX> > uids;
	CleanupClosePushL(uids);
	const TDesC8 *u = &it->FirstL();
	while (u->Length() > 0) {
		if (u->Length() < PHEDAV_ID_MAX) { TBuf8<PHEDAV_ID_MAX> b(*u); uids.AppendL(b); }
		else printf("calendar: UID longer than %d skipped\n", PHEDAV_ID_MAX - 1);
		u = &it->NextL();
	}
	for (TInt i = 0; i < uids.Count() && !sc.stop; i++) {
		unsigned long sig = 0, old = 0;
		TInt cnt = 0;
		if (SignatureOf(uids[i], sig, cnt) != KErrNone || cnt == 0) continue;
		char txt[PHEDAV_ID_MAX];
		memcpy(txt, uids[i].Ptr(), uids[i].Length());
		txt[uids[i].Length()] = 0;
		if (!fsig_get(txt, &old)) { sc.added++; if (sc.cb(sc.cbctx, txt, PHEDAV_CH_ADD)) sc.stop = ETrue; }
		else if (old != sig) { sc.modified++; if (sc.cb(sc.cbctx, txt, PHEDAV_CH_MOD)) sc.stop = ETrue; }
	}
	CleanupStack::PopAndDestroy(2);   // uids, it
}

static int cb_missing(void *ctx, const char *id, unsigned long sig)
{
	RArray<TBuf8<PHEDAV_ID_MAX> > *gone = (RArray<TBuf8<PHEDAV_ID_MAX> > *)ctx;
	unsigned long s;
	TInt cnt = 0;
	(void)sig;
	if (SignatureOf(UidDes(id), s, cnt) == KErrNone && cnt == 0) {
		TBuf8<PHEDAV_ID_MAX> b(UidDes(id));
		gone->Append(b);
	}
	return 0;
}

static int l_changes(void *ctx, const char *coll, int (*cb)(void *, const char *, int), void *cbctx)
{
	(void)ctx; (void)coll;
	TScan sc;
	sc.cb = cb; sc.cbctx = cbctx; sc.added = sc.modified = 0; sc.stop = EFalse;
	TRAPD(err, ScanL(sc));
	if (err) { printf("calendar: changes scan failed %d\n", err); return -1; }
	if (sc.stop) return 0;
	RArray<TBuf8<PHEDAV_ID_MAX> > gone;
	fsig_each(cb_missing, &gone);
	TInt deleted = 0;
	for (TInt i = 0; i < gone.Count(); i++) {
		char txt[PHEDAV_ID_MAX];
		memcpy(txt, gone[i].Ptr(), gone[i].Length());
		txt[gone[i].Length()] = 0;
		deleted++;
		if (cb(cbctx, txt, PHEDAV_CH_DEL)) break;
	}
	gone.Close();
	printf("calendar: local changes: %d added, %d modified, %d deleted\n", (int)sc.added, (int)sc.modified, (int)deleted);
	return 0;
}

static int l_read(void *ctx, const char *coll, const char *id, char *buf, size_t cap, size_t *len)
{
	(void)ctx; (void)coll;
	TInt cnt = 0;
	TRAPD(err, ExportGroupL(UidDes(id), cnt));
	if (err || cnt == 0) return -1;
	TInt n = g_buf->Size();
	*len = (size_t)n;
	if ((size_t)n > cap) return -1;
	TPtr8 d((TUint8 *)buf, 0, n);
	g_buf->Read(0, d, n);
	return 0;
}

static void DeleteUidL(const TDesC8 &aUid)
{
	CDesC8ArrayFlat *l = new (ELeave) CDesC8ArrayFlat(1);
	CleanupStack::PushL(l);
	l->AppendL(aUid);
	g_view->DeleteL(*l);
	CleanupStack::PopAndDestroy(l);
}

// imports the text, replaces the stored group with the same UID, returns the UID in aUid
static void ImportL(const char *data, size_t len, TBuf8<PHEDAV_ID_MAX> &aUid)
{
	RPointerArray<CCalEntry> arr;
	CleanupStack::PushL(TCleanupItem(ResetAndDestroyArr, &arr));
	TPtrC8 p((const TUint8 *)data, (TInt)len);
	RDesReadStream rs(p);
	CleanupClosePushL(rs);
	g_exch->ImportL(KUidVCalendar, rs, arr);
	CleanupStack::PopAndDestroy();       // rs
	if (arr.Count() < 1) User::Leave(KErrCorrupt);
	const TDesC8 &uid = arr[0]->UidL();
	if (uid.Length() >= PHEDAV_ID_MAX) User::Leave(KErrTooBig);
	aUid.Copy(uid);
	TRAP_IGNORE(DeleteUidL(aUid));       // KErrNotFound when it is new
	TInt ok = 0;
	g_view->StoreL(arr, ok);
	if (ok < arr.Count()) User::Leave(KErrGeneral);
	CleanupStack::PopAndDestroy();       // arr
}

static int l_write(void *ctx, const char *coll, const char *local_id, const char *data, size_t len, char *new_id)
{
	(void)ctx; (void)coll;
	TBuf8<PHEDAV_ID_MAX> uid;
	TRAPD(err, ImportL(data, len, uid));
	if (err) { printf("calendar: import failed %d\n", err); return -1; }
	memcpy(new_id, uid.Ptr(), uid.Length());
	new_id[uid.Length()] = 0;
	if (local_id[0] && strcmp(local_id, new_id) != 0) {       // the server changed the UID: drop the old group
		TRAP_IGNORE(DeleteUidL(UidDes(local_id)));
		fsig_del(local_id);
	}
	unsigned long sig;
	TInt cnt = 0;
	if (SignatureOf(uid, sig, cnt) == KErrNone && cnt > 0) fsig_set(new_id, sig);
	return 0;
}

static int l_remove(void *ctx, const char *coll, const char *id)
{
	(void)ctx; (void)coll;
	TRAPD(err, DeleteUidL(UidDes(id)));
	fsig_del(id);
	return (err == KErrNone || err == KErrNotFound) ? 0 : -1;
}

static int l_ack(void *ctx, const char *coll, const char *id)
{
	(void)ctx; (void)coll;
	unsigned long sig;
	TInt cnt = 0;
	if (SignatureOf(UidDes(id), sig, cnt) == KErrNone && cnt > 0) fsig_set(id, sig);
	else fsig_del(id);
	return 0;
}

phedav_local calendar_local(void)
{
	phedav_local l;
	memset(&l, 0, sizeof l);
	l.changes = l_changes;
	l.read = l_read;
	l.write = l_write;
	l.remove = l_remove;
	l.ack = l_ack;
	return l;
}
