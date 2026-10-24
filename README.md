# Phorepoc dav-sync

Repository: `Phorepoc/dav-sync`. The library and its API keep the component name `phedav` (`phedav.h`, `phedav_sync`, `libphedav`).

A small, portable C99 CalDAV / CardDAV sync core for old and unusual phones (a Symbian^3 phone, the Phorepoc OS), so they can
sync contacts and calendars with any standard server: Radicale, Nextcloud, Stalwart, SabreDAV-based servers, or a personal hub.
Status: **working on a host**; the platform integration is a design note (`SYMBIAN.md`).

No malloc, no OS calls in the core. The host provides a byte-stream transport and two small adapters (sync state, local
database). Items stay opaque vCard 3.0/4.0 and iCalendar text; the platform layer converts them with its own APIs.

## Quick start
It is a library: you give it a byte-stream transport and two small adapters, and call `phedav_sync` on a collection. The host build in `test/` is a complete example (POSIX sockets, an in-memory
database) and the quickest way to see it work against a real server:
```
make check                                         # unit tests (XML, HTTP, sync logic; 96 checks)
docker run -d --rm --name radicale -p 5232:5232 tomsquest/docker-radicale     # any CalDAV / CardDAV server will do
make build/integ && PHEDAV_URL=http://127.0.0.1:5232/ PHEDAV_USER=alice PHEDAV_PASS=secret ./build/integ both   # creates and deletes its own test collections
```
`sh test/run.sh` runs the whole integration suite (Radicale in Docker, ASan/UBSan build, and TLS with a CA and with a pinned certificate when mbedTLS 3.6 is in `.deps/mbedtls`).
The API is in `include/phedav.h`; the Symbian port is described in `SYMBIAN.md`.

## What it does
- **HTTP/1.1** over an abstract transport: keep-alive, Content-Length and chunked bodies, redirects (301/302/307/308 keep the
  method, 303 becomes GET; cross-origin and https-to-http are refused unless allowed), Basic auth (use app passwords).
- **TLS 1.2 and 1.3** through an optional mbedTLS 3.6 adapter (`phedav_mbedtls`): CA-bundle verification with host name, or a
  pinned certificate hash for self-signed servers. There is no "insecure" switch.
- **Discovery**: `/.well-known/caldav|carddav` -> `current-user-principal` -> `calendar-home-set` / `addressbook-home-set` ->
  collections (name, ctag, sync-token, supported components).
- **Sync** of one collection (`phedav_sync`): RFC 6578 `sync-collection` (incremental, or a full listing with an empty token),
  with a fallback to ctag + etag listing for servers without it; multiget for the changed items (GET if multiget is refused);
  `PUT` with `If-None-Match: *` / `If-Match`, `DELETE` with `If-Match`; an expired sync token falls back to a full listing
  with deletion detection automatically.
- **State** through callbacks: per collection `{sync_token, ctag}` and rows `{href, etag, local_id}`.
- **Conflicts**: server wins by default; `PHEDAV_USE_LOCAL` or a per-item callback instead. Covers modified/modified,
  deleted/modified in both directions.
- **XML**: a ~300-line streaming parser (`phedav_xml`), namespace-aware (prefixes, default namespace, redefinition),
  entities, CDATA; no external library.

## The sync loop
```
phedav_sync(collection):
  1  gather local changes                      (local.changes: added / modified / deleted since the last ack)
  2  list the server                           sync-collection with the stored token  -> changed + deleted hrefs
        token refused / empty                  -> sync-collection with an empty token -> full listing (deletions inferred)
        sync-collection unsupported            -> PROPFIND ctag; unchanged? stop the download, else PROPFIND Depth 1 listing
  3  reconcile each server change with the local changes
        server changed, local untouched        -> fetch
        both changed / one deleted             -> conflict policy (server wins | local wins | callback)
  4  fetch queued items (multiget, 20 per request) -> local.write, store.item_put (etag = server's)
  5  upload remaining local changes            PUT new (If-None-Match: *), PUT changed (If-Match: etag), DELETE (If-Match)
        412 Precondition Failed                -> counted as `deferred`, retried at the next sync
  6  if no item failed: store the new token (or ctag)
```
Our own uploads come back in the next `sync-collection` with the etag we stored, so they are recognised and skipped (one request
for "nothing changed"). An item that does not fit the item buffer is counted in `stats.errors` and the token is not advanced, so
it is retried after the buffer is enlarged.

## Layout
```
include/phedav.h          public API (transport, client, discovery, store/local interfaces, sync)
include/phedav_xml.h      streaming XML
include/phedav_mbedtls.h  TLS adapter (optional)
src/                      phedav_xml.c  phedav_http.c  phedav_dav.c (multistatus, discovery)  phedav_sync.c  phedav_mbedtls.c
test/unit.c               offline: XML, HTTP (scripted transport), multistatus, UID/base64, mutation runs
test/integ.c              live server: discovery, initial sync, add/modify/delete both ways, conflicts, token expiry, listing mode
test/devdb.[ch]           an in-memory device database + sync state (the model for a real adapter)
test/run.sh               Docker run: Radicale (plain and TLS) + the ASan/UBSan build, on a private network
SYMBIAN.md                how it becomes a Symbian^3 sync client (exact Contacts Model / Calendar API calls)
```

## Build and test
```
make check                       # unit tests
make fuzz                        # the same under AddressSanitizer + UBSan (includes 6000 mutation runs)
sh test/run.sh                   # on a Docker host: Radicale integration (141 checks), then TLS with CA and pinned cert
make MBEDTLS=/path/to/mbedtls-3.6 tls integ-tls     # the TLS adapter and the integration test with it
```
Memory: one `phedav` object (about 25 KB), the caller's item buffer (one vCard / iCalendar object; 64 KB, 256 KB with photos)
and a work arena (about 150 bytes per server item of the collection plus 60 per pending local change).

## Not (yet)
Digest and OAuth (Basic with app passwords only), CardDAV groups, scheduling (iTIP) and free-busy, WebDAV-Push, IPv6 literal
URLs, HTTP/2. Tested servers: Radicale 3.5. Stalwart and Nextcloud are the next ones.

## Security
- Basic authentication only, so use **app passwords** and **https**: with the mbedTLS adapter the server certificate is verified against a CA bundle (with host name) or pinned by SHA-256;
  there is no "insecure" switch. Redirects from https to http and across origins are refused unless the caller allows them.
- The XML and HTTP parsers work in fixed buffers and are exercised by mutation tests under AddressSanitizer and UBSan on every change (`make fuzz`).
- The core never allocates and never touches the OS, so a port cannot be surprised by hidden I/O. Credentials are only held in the caller's structures; the library does not store them.
- Report vulnerabilities privately: see `SECURITY.md`.

## License

Apache-2.0 (see `LICENSE`; third-party notes in `NOTICE`). The Radicale used in the tests is GPL and runs as an external server only.
