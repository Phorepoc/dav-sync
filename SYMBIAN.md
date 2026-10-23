# From the C core to a Symbian^3 sync client (design note)

Status: design. The C core (`src/`) is done and tested on a host; nothing here has run on a phone yet. Class and method names
below were checked against the public 2010 platform headers (`epoc32/include/app/`).

## Shape
One Symbian EXE (a background "sync server" started by a timer, a push-style trigger or the user) links the portable core
plus a TLS transport and implements three small adapters:

| core interface | Symbian implementation |
|---|---|
| `phedav_net` (open/send/recv/close) | RSocketServ / RSocket (or RConnection via a chosen IAP) for the raw stream; the TLS adapter (`phedav_mbedtls`, or the platform TLS if it can do 1.2+) on top. `recv` must time out (`RSocket::RecvOneOrMore` + `RTimer`), so the core never blocks forever. |
| `phedav_store` (sync state) | a `CDictionaryFileStore` or a plain file in the server's private directory: per collection `{sync_token, ctag}` and rows `{href, etag, local_id}`. |
| `phedav_local` (the device database) | contacts: `CContactDatabase`; calendar: `CCalSession` + `CCalEntryView`. Details below. |

The core is blocking C; run it in its own thread (`RThread`) with a private heap and call it through a small active-object
wrapper (`CActive` that waits on the thread's `RThread::Logon`), so the UI and the sync server stay responsive. Cancellation =
close the socket from the wrapper; the core then returns `PHEDAV_ERR_IO`.

## Contacts (CardDAV) with the Contacts Model
Local id = `TContactItemId` as decimal text (fits `PHEDAV_ID_MAX`).

- **read(local_id)** -> vCard text: `CContactDatabase::ExportSelectedContactsL(const TUid& aFormat, const CContactIdArray&
  aSelectedContactIds, RWriteStream& aWriteStream, TInt aOption, TBool aExportPrivateFields)` (an overload also takes a
  `Versit::TVersitCharSet`; use UTF-8). Format UID: `TUid::Uid(KVersitEntityUidVCard)` (`vuid.h`; the default converter is
  `KUidVCardConvDefaultImpl`, `cntdb.h`). Write to an `RBufWriteStream` / `RDesWriteStream` over a `CBufFlat`, then copy into
  the core's buffer. Options: `CContactDatabase::EIncludeX | ETTFormat` as the product needs; `EExcludeUid` off, so the UID
  property travels (the core uses it to name the server resource).
- **write(local_id, text)** -> `CContactDatabase::ImportContactsL(const TUid& aFormat, RReadStream& aReadStream, TBool&
  aImportSuccessful, TInt aOption)` with an `RDesReadStream` over the text and `EImportSingleContact`. It returns the created
  `CContactItem`s; take `Id()` for `new_id`. Replace = import the new card, then `DeleteContactL(old_id)` (the Contacts Model has
  no in-place vCard merge; keep the old id stable for the user's speed dials and groups by copying the fields into the
  existing item with `ReadContactLC` / `UpdateContactLC` / `CommitContactL` when an exact match exists: a per-field mapping
  that the product can add later without changing the core).
- **remove** -> `CContactDatabase::DeleteContactL(TContactItemId)` (or `DeleteContactsL` for a batch).
- **changes** -> added/modified: `CContactDatabase::ContactsChangedSinceL(const TTime& aTime)` (returns a
  `CContactIdArray`; the time is the end of the last sync, minus a safety margin). Deleted: the database does not list
  tombstones, so either (a) keep a resident `CContactChangeNotifier::NewL(db, MContactDbObserver*)` and record
  `EContactDbObserverEventContactDeleted` (and `Added` / `Changed`) events in the state file, or (b) diff the stored
  `local_id` set against `CContactDatabase::SortedItemsL()` at sync time. Option (b) needs no resident process and is the
  first implementation; (a) is for push-style "sync on change". Skip templates, groups and the own card (events with those
  types are different: `EContactDbObserverEventGroupAdded` etc.). Contact groups can be mapped to CardDAV `KIND:group` vCard 4.0
  later; v1 syncs plain contacts.
- **ack** -> move the stored "last synced" timestamp for that id (state file), because the Contacts Model has no dirty bit.
- Writes made by the core must not count as local changes: record the id with the timestamp *after* `CommitContactL`, or have
  the observer ignore ids that the sync thread has marked as "in apply".

Photos: vCard 3.0 `PHOTO;ENCODING=b` can be 100 KB or more. Size the item buffer for it (256 KB) or accept that large items are
counted in `stats.errors` and skipped (the token stays put, the rest syncs).

## Calendar (CalDAV) with the Calendar API
One `CCalSession` per calendar file; one device calendar file per synced server collection (`CreateCalFileL(name)`, then
`OpenL(name)`), because the core syncs per collection. Local id = `TCalLocalUid` as decimal text.

- **read(local_id)** -> iCalendar text: `CCalEntryView::FetchL(TCalLocalUid)` then
  `CCalDataExchange::ExportL(TUid aDataFormat, RWriteStream&, RPointerArray<CCalEntry>&)` with `aDataFormat =
  KUidVCalendar` (`caldataformat.h`, `0x1020DB7B`; the same exporter writes iCalendar 2.0 text). A repeating event with
  exceptions is several `CCalEntry` with one UID (`CCalEntryView::FetchL(const TDesC8& aUid, RPointerArray<CCalEntry>&)`);
  export them in one stream so the server gets one resource.
- **write(local_id, text)** -> `CCalDataExchange::ImportL(KUidVCalendar, RReadStream&, RPointerArray<CCalEntry>&)`, then
  `CCalEntryView::StoreL(const RPointerArray<CCalEntry>&, TInt& aNumSuccessfulEntry)` for new entries and `UpdateL(...)` for
  entries that already exist (match by UID with `FetchL(uid, array)`; an update replaces the whole UID group). `new_id` =
  `CCalEntry::LocalUidL()` of the first entry after the store. `CCalDataExchange::NewL(CCalSession&)`.
- **remove** -> `CCalEntryView::DeleteL(const CDesC8Array& aUidList)` (by UID, removes the whole group) or `DeleteL(const
  RArray<TCalLocalUid>&, TInt&)`.
- **changes** -> added/modified: `CCalEntryView::GetIdsModifiedSinceDateL(const TCalTime&, RArray<TCalLocalUid>&)`
  (compare with `CCalEntry::LastModifiedDateL`; call `SetLastModifiedDateL()` is what the apps do on edit, so imported entries
  carry the server's DTSTAMP/LAST-MODIFIED, which is exactly what we want to avoid re-uploading: import with the modified time
  set to the sync time or filter by the stored etag mapping). Deleted: `CCalSession::StartChangeNotification(MCalChangeCallBack2&,
  const CCalChangeNotificationFilter&)` delivers `TCalChangeEntry { iEntryId, iChangeType = EChangeAdd | EChangeDelete |
  EChangeModify, iEntryType }` (`calchangecallback.h`), or diff the stored ids against a `CCalIterator` walk at sync time.
- Time zones: iCalendar `VTIMEZONE` is handled by the importer; floating times stay floating.

## Account setup (what the user sees)
A settings page "Calendar and contacts sync": server URL (`https://dav.example.org/`), user, app password, which collections
to sync (from `phedav_discover`, with the calendar's component set so task-only lists can be mapped to To-Do),
interval, and the conflict policy (server wins is the default). Credentials in the platform's secure store; the TLS trust
anchors as a PEM bundle shipped with the package (or the platform certificate store converted at install time); a pinned
fingerprint option for self-hosted servers.

## Order of work
1. TLS + transport + core in a console test EXE on the emulator-class target: `phedav_discover` and a `phedav_sync` against a
   Radicale on the LAN with an in-memory store (the host `test/devdb.c` is the model).
2. Contacts adapter (`read`, `write`, `remove`, diff-based `changes`) and a file-backed store. Round-trip test: 200 vCards
   with accents, multiple phones, photos.
3. Calendar adapter; recurring events with exceptions, all-day events, alarms (VALARM maps to `CCalAlarm`).
4. The settings UI and a timer/connection-aware scheduler (sync only on an active IAP; back off on `PHEDAV_ERR_IO`).
5. Observer-based "sync on change" (`CContactChangeNotifier`, `StartChangeNotification`).

Known limits of v1: Basic auth only (use app passwords; Digest and OAuth are out), no CardDAV groups, no free-busy or
scheduling (iTIP) inbox, no attachments beyond what fits the item buffer, no WebDAV-Push (polling or a hub-side trigger).

## Status of the first Symbian build (2026-10-23)
`symbian/` holds a console exe, `phedav.exe`, built for the stock phone (user capabilities NetworkServices, ReadUserData,
WriteUserData; Open C sockets like phetail; no TLS in this build, https is refused):

```
phedav.exe <url> <user> <password> [contacts|calendar] [collection-name-filter]
```
- `plat.cpp`: log `C:\data\phedav\phedav.log`; access point from `C:\data\phedav\iap.txt` (else phetail's `iap.txt`, else
  "ASUS"); non-blocking connect with a 20 s limit, `select()` timeouts of 40 s on send and receive; `mbedtls_hardware_poll`
  from `Math::Random` + `User::FastCounter` + tick count + time (ready for the TLS build).
- `store.cpp`: journalled state file `C:\data\phedav\state_<hash>.txt` (meta, item rows, contact signatures).
- `contacts.cpp`: `CContactDatabase::OpenL`, export with `ExportSelectedContactsL(KVersitEntityUidVCard, ..., EUTF8CharSet)`,
  import with `ImportContactsL(..., EImportSingleContact)`, delete with `DeleteContactL`. Change detection is by signature
  (hash of the exported vCard stored after the last sync) instead of timestamps: no signature = added, other = modified, a
  stored id that no longer exists = deleted. This also makes the core's own writes invisible.
- Calendar: argument accepted, not implemented. TLS: the adapter (`src/phedav_mbedtls.c`) is not part of this build yet
  (needs an mbedTLS config for Open C: no net_sockets, timing, file IO; `MBEDTLS_NO_PLATFORM_ENTROPY` +
  `MBEDTLS_ENTROPY_HARDWARE_ALT`).
Not run on a phone yet.

### Test safety and the TLS / calendar builds (2026-10-23)
- The exe REFUSES to run unless `--db <file>` (a separate contacts database or calendar file, created if missing, e.g.
  `c:phedavtest.cdb`, `c:phedavtest.cal`) or `--real-db` is given. The state file name includes the database name, so a test
  database never shares state with the real one.
- TLS: `phedav.mmp` compiles mbedTLS 3.6 with `symbian/phedav_mbedtls_cfg.h` (no net_sockets / timing / file IO / threading /
  asm, client only, entropy and the millisecond clock from `plat.cpp`). The mbedTLS sources are an external dependency not kept
  in this repo: unpack mbedtls-3.6.x into `.deps/mbedtls/` (`include/`, `library/`) before building. Trust: `ca.pem` and/or
  `pin.txt` in `C:\data\phedav\`; with neither, https is refused. Stack 256 KB.
- Calendar (`calendar.cpp`): one CalDAV resource = one UID group; local id = the iCalendar UID; `CCalIter` enumerates UIDs,
  `CCalEntryView::FetchL(uid, array)` + `CCalDataExchange::ExportL/ImportL(KUidVCalendar)`, `StoreL`, `DeleteL(CDesC8Array)`.
  Signature-based change detection, like the contacts adapter. Not run on a phone.

## Notes from testing on an x86 Symbian image (QEMU)
- **Text encoding.** The Contacts Model's Versit reads a property without `CHARSET` as 8-bit text, so a UTF-8 vCard 3.0 came out
  double encoded. `contacts.cpp` adds `;CHARSET=UTF-8` to every line with non-ASCII bytes on import, reads the card as 2.1 (Versit
  cuts a 3.0 value at the first comma) after dropping the `\,` escapes, and turns Versit's 2.1 quoted-printable export back into a
  vCard 3.0 (UTF-8, `TYPE=`, commas escaped; servers built on vobject cut unescaped commas) before the upload. The device needs
  `Z:\resource\charconv\basic.snm` (charconv.iby); without it Versit crashes on non-ASCII text.
- **Connect.** Open C can answer `EINPROGRESS` on a socket where `O_NONBLOCK` could not be set; `plat.cpp` polls `connect()`.
- **Self-test helper.** `--selftest "add:Name,edit:Name:NewFamily,del:Name,list"` (contacts; calendar: `add:uid:summary`,
  `edit:uid:summary`, `del:uid`, `list`) changes the `--db` test database before the sync, without recording signatures, so the
  sync sees real local changes. `--local-wins` sets the conflict policy to local (default: server wins).
