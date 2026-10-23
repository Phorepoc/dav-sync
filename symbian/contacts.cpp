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
#include <cntfield.h>
#include <cntfldst.h>
#include <vuid.h>
#include <vutil.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "phedav_sym.h"

static CContactDatabase *g_db;
static CBufFlat *g_buf;

static void OpenDbL(const char *aName)
{
	if (!aName) { g_db = CContactDatabase::OpenL(); return; }
	TFileName fn;
	fn.Copy(TPtrC8((const TUint8 *)aName));
	TRAPD(e, g_db = CContactDatabase::OpenL(fn));
	if (e == KErrNotFound || e == KErrPathNotFound) { g_db = CContactDatabase::CreateL(fn); printf("contacts: created %s\n", aName); }
	else User::LeaveIfError(e);
}

int contacts_open(const char *aDbName)
{
	TRAPD(err, OpenDbL(aDbName); g_buf = CBufFlat::NewL(2048));
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

static void FreeCell(TAny *a) { User::Free(a); }

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

static size_t ToV30(const char *in, size_t len, char *out, size_t cap);

static int l_read(void *ctx, const char *coll, const char *id, char *buf, size_t cap, size_t *len)
{
	(void)ctx; (void)coll;
	TRAPD(err, ExportOneL((TContactItemId)atoi(id)));
	if (err) return -1;
	TInt n = g_buf->Size();
	char *raw = (char *)User::Alloc(n + 1);
	char *cv = (char *)User::Alloc(n * 2 + 1024);
	if (!raw || !cv) { User::Free(raw); User::Free(cv); return -1; }
	TPtr8 d((TUint8 *)raw, 0, n);
	g_buf->Read(0, d, n);
	raw[n] = 0;
	size_t cl = ToV30(raw, (size_t)n, cv, (size_t)n * 2 + 1024);
	int rc = -1;
	if (cl && cl <= cap) { memcpy(buf, cv, cl); *len = cl; rc = 0; }
	User::Free(raw); User::Free(cv);
	return rc;
}

static void DestroyItems(TAny *a)
{
	CArrayPtr<CContactItem> *items = (CArrayPtr<CContactItem> *)a;
	items->ResetAndDestroy();
	delete items;
}

/* ---------- text conversions around the Contacts Model's Versit (UTF-8 names) ----------
   Import: a vCard 3.0 is UTF-8, but Versit reads a property without CHARSET as 8-bit Latin-1 and the name comes out double
   encoded (u-umlaut as two Latin-1 characters). MarkUtf8 adds ;CHARSET=UTF-8 to every line that holds a byte >= 0x80 and has no CHARSET.
   Export: Versit writes vCard 2.1 (quoted-printable, bare TYPE words). ToV30 turns that into vCard 3.0 (UTF-8, TYPE=) so
   servers and other clients read it. The change-detection signature is taken over the raw export, not over this text. */
static size_t MarkUtf8(const char *in, size_t len, char *out, size_t cap)
{
	size_t o = 0, i = 0;
	while (i < len) {
		size_t e = i;
		int high = 0;
		while (e < len && in[e] != '\n') { if ((unsigned char)in[e] >= 0x80) high = 1; e++; }
		size_t ll = e - i;
		const char *colon = (const char *)memchr(in + i, ':', ll);
		int has_cs = 0;
		if (colon) { for (const char *q = in + i; q + 7 <= colon; q++) if (!strncmp(q, "CHARSET", 7)) { has_cs = 1; break; } }
		if (high && colon && !has_cs) {
			size_t head = (size_t)(colon - (in + i));
			if (o + ll + 16 >= cap) return 0;
			memcpy(out + o, in + i, head); o += head;
			memcpy(out + o, ";CHARSET=UTF-8", 14); o += 14;
			memcpy(out + o, colon, ll - head); o += ll - head;
		} else {
			if (o + ll + 1 >= cap) return 0;
			memcpy(out + o, in + i, ll); o += ll;
		}
		if (e < len) out[o++] = '\n';
		i = e + 1;
	}
	return o;
}

static int HexVal(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

static size_t ToV30(const char *in, size_t len, char *out, size_t cap)
{
	size_t o = 0, i = 0;
	while (i < len) {
		/* one logical line: physical lines joined while a quoted-printable line ends in '=' */
		char *line = (char *)User::Alloc(len - i + 2);
		if (!line) return 0;
		size_t n = 0;
		int qp = 0;
		for (;;) {
			size_t e = i;
			while (e < len && in[e] != '\n') e++;
			size_t pl = e - i;
			if (pl && in[i + pl - 1] == '\r') pl--;
			memcpy(line + n, in + i, pl); n += pl;
			i = e + 1;
			if (n && line[n - 1] == '=' && (memchr(line, ':', n) != NULL) && i < len) {
				line[n] = 0;
				const char *c = strchr(line, ':');
				if (c && (strstr(line, "QUOTED-PRINTABLE") && strstr(line, "QUOTED-PRINTABLE") < c)) { n--; continue; }   /* soft break */
			}
			break;
		}
		line[n] = 0;
		const char *colon = strchr(line, ':');
		if (!colon) { memcpy(out + o, line, n); o += n; out[o++] = '\r'; out[o++] = '\n'; User::Free(line); continue; }
		if (!strncmp(line, "VERSION:2.1", 11)) { memcpy(out + o, "VERSION:3.0\r\n", 13); o += 13; User::Free(line); continue; }
		/* head = NAME;param;param */
		size_t hlen = (size_t)(colon - line);
		char *head = (char *)User::Alloc(hlen + 1);
		if (!head) { User::Free(line); return 0; }
		memcpy(head, line, hlen); head[hlen] = 0;
		char *save = NULL;
		char *tok = head;
		int first = 1;
		for (;;) {
			char *semi = strchr(tok, ';');
			if (semi) *semi = 0;
			if (first) { size_t l = strlen(tok); memcpy(out + o, tok, l); o += l; first = 0; }
			else if (!strncmp(tok, "ENCODING=QUOTED-PRINTABLE", 25) || !strncmp(tok, "CHARSET=", 8)) { if (tok[0] == 'E') qp = 1; }
			else if (strchr(tok, '=')) { out[o++] = ';'; size_t l = strlen(tok); memcpy(out + o, tok, l); o += l; }
			else if (*tok) { memcpy(out + o, ";TYPE=", 6); o += 6; size_t l = strlen(tok); memcpy(out + o, tok, l); o += l; }
			if (!semi) break;
			tok = semi + 1;
		}
		(void)save;
		out[o++] = ':';
		for (const char *v = colon + 1; *v; v++) {
			if (qp && *v == '=' && HexVal(v[1]) >= 0 && HexVal(v[2]) >= 0) { out[o++] = (char)(HexVal(v[1]) * 16 + HexVal(v[2])); v += 2; }
			else out[o++] = *v;
		}
		out[o++] = '\r'; out[o++] = '\n';
		User::Free(head); User::Free(line);
		if (o + 512 >= cap) return 0;
	}
	return o;
}

static TContactItemId ImportL(const char *data, size_t len)
{
	char *mk = (char *)User::Alloc(len + len / 4 + 64);
	if (!mk) User::Leave(KErrNoMemory);
	CleanupStack::PushL(TCleanupItem(FreeCell, mk));
	size_t ml = MarkUtf8(data, len, mk, len + len / 4 + 64);
	if (!ml) User::Leave(KErrTooBig);
	TPtrC8 p((const TUint8 *)mk, (TInt)ml);
	RDesReadStream rs(p);
	CleanupClosePushL(rs);
	TBool ok = EFalse;
	CArrayPtr<CContactItem> *items = g_db->ImportContactsL(TUid::Uid(KVersitEntityUidVCard), rs, ok,
	                                                       CContactDatabase::EImportSingleContact);
	CleanupStack::PushL(TCleanupItem(DestroyItems, items));
	if (!ok || items->Count() < 1) User::Leave(KErrCorrupt);
	TContactItemId nid = (*items)[0]->Id();
	CleanupStack::PopAndDestroy(3);   // the items (cleanup item), the stream, the marked text
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

/* ---------- self-test helper (TEST databases only, --db): create / edit / delete / list cards without recording a signature,
   so the next sync sees them as local changes. Names are looked up in the exported vCard text (UTF-8 substring). ---------- */
static TContactItemId FindByNameL(const char *name)
{
	const CContactIdArray *ids = g_db->SortedItemsL();
	for (TInt i = 0; i < ids->Count(); i++) {
		TContactItemId id = (*ids)[i];
		if (id == g_db->OwnCardId() || !IsCardL(id)) continue;
		ExportOneL(id);
		TInt n = g_buf->Size();
		char *t = (char *)User::Alloc(n + 1);
		if (!t) User::Leave(KErrNoMemory);
		TPtr8 d((TUint8 *)t, 0, n);
		g_buf->Read(0, d, n);
		t[n] = 0;
		TBool hit = strstr(t, name) != NULL;      /* anywhere in the exported card (names with non-ASCII letters are quoted-printable there) */
		User::Free(t);
		if (hit) return id;
	}
	return KNullContactId;
}

static void SelfTestL(const char *op, const char *a, const char *b)
{
	if (!strcmp(op, "add")) {
		char card[512];
		sprintf(card, "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:%s\r\nN:%s;;;;\r\nTEL;TYPE=CELL:+4900000000\r\nEND:VCARD\r\n", a, a);
		TContactItemId nid = ImportL(card, strlen(card));
		printf("selftest: added \"%s\" as %d\n", a, (int)nid);
	} else if (!strcmp(op, "edit")) {      // a = name to find, b = new family name (ASCII)
		TContactItemId id = FindByNameL(a);
		if (id == KNullContactId) { printf("selftest: edit: \"%s\" not found\n", a); return; }
		CContactItem *it = g_db->OpenContactLX(id);
		CleanupStack::PushL(it);
		CContactItemFieldSet &fs = it->CardFields();
		TInt pos = fs.Find(KUidContactFieldFamilyName);
		if (pos < 0) User::Leave(KErrNotFound);
		TBuf<64> nn;
		nn.Copy(TPtrC8((const TUint8 *)b));
		fs[pos].TextStorage()->SetTextL(nn);
		g_db->CommitContactL(*it);
		CleanupStack::PopAndDestroy(2);   // it, the lock record
		printf("selftest: edited %d (\"%s\") family name -> \"%s\"\n", (int)id, a, b);
	} else if (!strcmp(op, "del")) {
		TContactItemId id = FindByNameL(a);
		if (id == KNullContactId) { printf("selftest: del: \"%s\" not found\n", a); return; }
		g_db->DeleteContactL(id);
		printf("selftest: deleted %d (\"%s\")\n", (int)id, a);
	} else if (!strcmp(op, "list")) {
		const CContactIdArray *ids = g_db->SortedItemsL();
		for (TInt i = 0; i < ids->Count(); i++) {
			TContactItemId id = (*ids)[i];
			if (id == g_db->OwnCardId() || !IsCardL(id)) continue;
			ExportOneL(id);
			TInt n = g_buf->Size();
			char *t = (char *)User::Alloc(n + 1);
			if (!t) User::Leave(KErrNoMemory);
			TPtr8 d((TUint8 *)t, 0, n);
			g_buf->Read(0, d, n);
			t[n] = 0;
			char line[300];
			int k = 0;
			for (const char *p = t; *p && k < 290; p++) { if (*p == 13) continue; line[k++] = (*p == 10) ? '|' : *p; }
			line[k] = 0;
			printf("selftest: card %d: %s\n", (int)id, line);      /* the exported vCard, UTF-8 bytes as stored */
			User::Free(t);
		}
	} else {
		printf("selftest: unknown op %s\n", op);
	}
}

int contacts_selftest(const char *op, const char *a, const char *b)
{
	TRAPD(err, SelfTestL(op, a, b));
	if (err) printf("selftest: %s failed %d\n", op, err);
	return err;
}
