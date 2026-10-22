// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
//
// phedav_local for the phone's contacts: vCard import / export through CContactDatabase (Versit), change detection by
// signature. Local id = TContactItemId in decimal.
//
// The Contacts Model has no dirty bit and no tombstones, so every sync exports each card (UTF-8 vCard), hashes the text and
// compares it with the signature stored after the previous sync (store.cpp): no signature = added, other signature =
// modified, a stored signature whose id is gone = deleted. A write made by the core records the signature of the re-exported
// card, so it never counts as a local change. See SYMBIAN.md.
#include <e32base.h>
#include <e32std.h>
#include <s32mem.h>
#include <cntdb.h>
#include <cntitem.h>
#include <cntdef.h>
#include <vuid.h>
#include <vutil.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "phedav_sym.h"

static CContactDatabase *g_db;
static CBufFlat *g_buf;

int contacts_open(void)
{
	TRAPD(err, g_db = CContactDatabase::OpenL(); g_buf = CBufFlat::NewL(2048));
	if (err) printf("contacts: open failed %d\n", err);
	return err;
}

void contacts_close(void)
{
	delete g_buf;
	g_buf = NULL;
	delete g_db;
	g_db = NULL;
}

static unsigned long fnv(const TUint8 *p, TInt n)
{
	unsigned long h = 2166136261UL;
	while (n-- > 0) { h ^= *p++; h *= 16777619UL; }
	return h;
}

// exports one card into g_buf (UTF-8 vCard)
static void ExportOneL(TContactItemId aId)
{
	CContactIdArray *ids = CContactIdArray::NewLC();
	ids->AddL(aId);
	g_buf->Reset();
	RBufWriteStream ws(*g_buf);
	CleanupClosePushL(ws);
	g_db->ExportSelectedContactsL(TUid::Uid(KVersitEntityUidVCard), *ids, ws, 0,
	                              Versit::EUTF8CharSet, ETrue);
	ws.CommitL();
	CleanupStack::PopAndDestroy(2);
}

static TInt SignatureOf(TContactItemId aId, unsigned long &aSig)
{
	TRAPD(err, ExportOneL(aId));
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

static int IsCardL(TContactItemId aId)
{
	CContactItem *it = g_db->ReadMinimalContactLC(aId);
	TUid type = it->Type();
	CleanupStack::PopAndDestroy(it);
	return type == KUidContactCard;
}

static void IdText(TContactItemId aId, char *out) { sprintf(out, "%d", (int)aId); }

static void ProbeL(TContactItemId aId)
{
	CContactItem *it = g_db->ReadMinimalContactLC(aId);
	CleanupStack::PopAndDestroy(it);
}

static int cb_missing(void *ctx, const char *id, unsigned long sig)
{
	// collected into an array of ids that no longer exist
	RArray<TInt> *gone = (RArray<TInt> *)ctx;
	(void)sig;
	TInt v = atoi(id);
	TRAPD(err, ProbeL((TContactItemId)v));
	if (err == KErrNotFound) gone->Append(v);
	return 0;
}

struct TScan { int (*cb)(void *, const char *, int); void *cbctx; TInt added, modified; TBool stop; };

static void ScanL(TScan &sc)
{
	const CContactIdArray *ids = g_db->SortedItemsL();
	TInt n = ids->Count();
	for (TInt i = 0; i < n && !sc.stop; i++) {
		TContactItemId id = (*ids)[i];
		if (id == g_db->OwnCardId() || !IsCardL(id)) continue;
		unsigned long sig = 0, old = 0;
		if (SignatureOf(id, sig) != KErrNone) continue;
		char txt[PHEDAV_ID_MAX];
		IdText(id, txt);
		if (!fsig_get(txt, &old)) { sc.added++; if (sc.cb(sc.cbctx, txt, PHEDAV_CH_ADD)) sc.stop = ETrue; }
		else if (old != sig) { sc.modified++; if (sc.cb(sc.cbctx, txt, PHEDAV_CH_MOD)) sc.stop = ETrue; }
	}
}

static int l_changes(void *ctx, const char *coll, int (*cb)(void *, const char *, int), void *cbctx)
{
	(void)ctx; (void)coll;
	TScan sc;
	sc.cb = cb; sc.cbctx = cbctx; sc.added = sc.modified = 0; sc.stop = EFalse;
	TRAPD(err, ScanL(sc));
	if (err) { printf("contacts: changes scan failed %d\n", err); return -1; }
	if (sc.stop) return 0;
	RArray<TInt> gone;
	fsig_each(cb_missing, &gone);
	TInt deleted = 0;
	for (TInt i = 0; i < gone.Count(); i++) {
		char txt[PHEDAV_ID_MAX];
		IdText(gone[i], txt);
		deleted++;
		if (cb(cbctx, txt, PHEDAV_CH_DEL)) break;
	}
	gone.Close();
	printf("contacts: local changes: %d added, %d modified, %d deleted\n", (int)sc.added, (int)sc.modified, (int)deleted);
	return 0;
}

static int l_read(void *ctx, const char *coll, const char *id, char *buf, size_t cap, size_t *len)
{
	(void)ctx; (void)coll;
	TRAPD(err, ExportOneL((TContactItemId)atoi(id)));
	if (err) return -1;
	TInt n = g_buf->Size();
	*len = (size_t)n;
	if ((size_t)n > cap) return -1;
	TPtr8 d((TUint8 *)buf, 0, n);
	g_buf->Read(0, d, n);
	return 0;
}

static void DestroyItems(TAny *a)
{
	CArrayPtr<CContactItem> *items = (CArrayPtr<CContactItem> *)a;
	items->ResetAndDestroy();
	delete items;
}

static TContactItemId ImportL(const char *data, size_t len)
{
	TPtrC8 p((const TUint8 *)data, (TInt)len);
	RDesReadStream rs(p);
	CleanupClosePushL(rs);
	TBool ok = EFalse;
	CArrayPtr<CContactItem> *items = g_db->ImportContactsL(TUid::Uid(KVersitEntityUidVCard), rs, ok,
	                                                       CContactDatabase::EImportSingleContact);
	CleanupStack::PushL(TCleanupItem(DestroyItems, items));
	if (!ok || items->Count() < 1) User::Leave(KErrCorrupt);
	TContactItemId nid = (*items)[0]->Id();
	CleanupStack::PopAndDestroy(2);   // the items (cleanup item), then the stream
	return nid;
}

static int l_write(void *ctx, const char *coll, const char *local_id, const char *data, size_t len, char *new_id)
{
	(void)ctx; (void)coll;
	TContactItemId nid = KNullContactId;
	TRAPD(err, nid = ImportL(data, len));
	if (err || nid == KNullContactId) { printf("contacts: import failed %d\n", err); return -1; }
	if (local_id[0]) {
		TRAP_IGNORE(g_db->DeleteContactL((TContactItemId)atoi(local_id)));
		fsig_del(local_id);
	}
	IdText(nid, new_id);
	unsigned long sig;
	if (SignatureOf(nid, sig) == KErrNone) fsig_set(new_id, sig);
	return 0;
}

static int l_remove(void *ctx, const char *coll, const char *id)
{
	(void)ctx; (void)coll;
	TRAPD(err, g_db->DeleteContactL((TContactItemId)atoi(id)));
	fsig_del(id);
	return (err == KErrNone || err == KErrNotFound) ? 0 : -1;
}

static int l_ack(void *ctx, const char *coll, const char *id)
{
	(void)ctx; (void)coll;
	unsigned long sig;
	if (SignatureOf((TContactItemId)atoi(id), sig) == KErrNone) fsig_set(id, sig);
	else fsig_del(id);             // gone: the deletion has been dealt with
	return 0;
}

phedav_local contacts_local(void)
{
	phedav_local l;
	memset(&l, 0, sizeof l);
	l.ctx = NULL;
	l.changes = l_changes;
	l.read = l_read;
	l.write = l_write;
	l.remove = l_remove;
	l.ack = l_ack;
	return l;
}
