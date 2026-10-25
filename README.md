# dav-sync

This is a small C library that syncs contacts and calendars with any standard CalDAV or CardDAV server, such as
Radicale, Nextcloud or Stalwart. We wrote it so old phones, starting with Symbian^3 and Phorepoc, can keep their
address book and calendar in sync with a server you run yourself.

It's plain C99, it never allocates memory, and it never calls the operating system directly. You give it a way to
send and receive bytes, and two small adapters, one to keep its sync state and one for the phone's own contact or
calendar database. That makes it easy to drop into unusual platforms.

It works today on Linux against real servers. The Symbian side is designed but not written yet, and `SYMBIAN.md`
explains the plan.

## Trying it

```sh
make check                                         # the unit tests
docker run -d --rm --name radicale -p 5232:5232 tomsquest/docker-radicale
make build/integ && PHEDAV_URL=http://127.0.0.1:5232/ PHEDAV_USER=alice PHEDAV_PASS=secret ./build/integ both
```

The second line starts a Radicale server, and the third runs a full sync against it. It creates its own test address
book and calendar and removes them afterwards. `sh test/run.sh` runs the complete suite, including a build with
AddressSanitizer and UBSan, and the TLS tests when mbedTLS 3.6 is in `.deps/mbedtls`.

The API is in `include/phedav.h`. The test program in `test/` is a complete example. It uses ordinary sockets and keeps
its contacts and events in memory.

## What it does

- It finds your address books and calendars by itself from the server's address, the standard way, through
  `/.well-known/caldav` and `/.well-known/carddav`.
- It syncs in both directions, and only what changed when the server supports that (RFC 6578). Older servers fall back
  to comparing the full list.
- When the same item changed on both sides, the server wins by default. You can make the phone win instead, or decide
  item by item.
- It speaks HTTP/1.1 with keep-alive, and TLS 1.2 or 1.3 through an optional mbedTLS adapter. The certificate is
  checked against your CA bundle, or pinned for a self-signed server.
- Contacts and events pass through untouched as vCard and iCalendar text. Converting them is up to the phone's side.

It needs about 25 KB for itself, a buffer for one contact or event (64 KB, or 256 KB if contacts have photos), and a
few hundred bytes per item while syncing.

Not supported yet: login methods other than Basic with an app password, contact groups, meeting invitations and
free/busy, and HTTP/2. So far it's been tested against Radicale 3.5, with Stalwart and Nextcloud next.

## Security

- Basic login sends your password with every request, so always use an **app password** and **https**. There's no
  switch to skip the certificate check. Redirects to another server, or from https to http, are refused unless you
  allow them.
- The XML and HTTP parsers work in fixed buffers, and every change is tested with thousands of mutated inputs under
  AddressSanitizer and UBSan (`make fuzz`).
- The library never stores your password. Only your own code holds it.
- Please report vulnerabilities privately, as described in `SECURITY.md`.

## License

Apache-2.0. The tests use Radicale, which is GPL, but only as a separate server.
