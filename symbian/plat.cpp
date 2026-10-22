// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
//
// Symbian platform glue for the phedav console exe (Open C / P.I.P.S.): log file, default access point, the raw TCP stream
// (non-blocking connect and select() timeouts), optional TLS, and the mbedTLS entropy source.
// Same approach as phetail's plat_symbian.cpp (BSD sockets + setdefaultif).
#include <e32std.h>
#include <e32math.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "phedav_sym.h"
#ifdef PHEDAV_WITH_MBEDTLS
#include "phedav_mbedtls.h"
#endif

#define CONNECT_TIMEOUT_S 20
#define RECV_TIMEOUT_S 40
#define IAP_DEFAULT "ASUS"

void plat_log_open(void)
{
	mkdir("C:\\data", 0755);
	mkdir(PHEDAV_DIR, 0755);
	freopen(PHEDAV_LOG, "a", stdout);
	freopen(PHEDAV_LOG, "a", stderr);
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
}

// Open C has no default connection: without one every socket asks the user. The access point name comes from
// C:\data\phedav\iap.txt, else phetail's C:\data\phetail\iap.txt, else the built-in default.
int plat_net_init(void)
{
	struct ifreq ifr;
	static const char *files[] = { PHEDAV_DIR "\\iap.txt", "C:\\data\\phetail\\iap.txt" };
	int i, got = 0;
	memset(&ifr, 0, sizeof ifr);
	for (i = 0; i < 2 && !got; i++) {
		FILE *f = fopen(files[i], "r");
		if (f) {
			if (fgets(ifr.ifr_name, sizeof ifr.ifr_name, f)) got = 1;
			fclose(f);
		}
	}
	if (!got) strncpy(ifr.ifr_name, IAP_DEFAULT, sizeof ifr.ifr_name - 1);
	ifr.ifr_name[strcspn(ifr.ifr_name, "\r\n")] = 0;
	printf("plat: access point \"%s\" -> setdefaultif %d\n", ifr.ifr_name, setdefaultif(&ifr));
	return 0;
}

/* ---------- raw TCP stream ---------- */

static int g_sock = -1;

static int wait_fd(int s, int write, int secs)
{
	fd_set fs;
	struct timeval tv;
	FD_ZERO(&fs);
	FD_SET(s, &fs);
	tv.tv_sec = secs;
	tv.tv_usec = 0;
	return select(s + 1, write ? NULL : &fs, write ? &fs : NULL, NULL, &tv);
}

static int raw_open(void *ctx, const char *host, unsigned port)
{
	struct sockaddr_in a;
	int s, fl, r;
	(void)ctx;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons((unsigned short)port);
	if (inet_aton(host, &a.sin_addr) == 0) {
		struct hostent *h = gethostbyname(host);
		if (!h || h->h_addrtype != AF_INET) { printf("plat: cannot resolve %s\n", host); return -1; }
		memcpy(&a.sin_addr, h->h_addr_list[0], 4);
	}
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) { printf("plat: socket errno %d\n", errno); return -1; }
	fl = fcntl(s, F_GETFL, 0);
	if (fl >= 0 && fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0) {
		r = connect(s, (struct sockaddr *)&a, sizeof a);
		if (r != 0 && (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EALREADY)) {
			if (wait_fd(s, 1, CONNECT_TIMEOUT_S) > 0) {
				int err = 0;
				socklen_t el = sizeof err;
				getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
				r = err ? -1 : 0;
				if (err) errno = err;
			} else {
				errno = ETIMEDOUT;
				r = -1;
			}
		}
		fcntl(s, F_SETFL, fl);          /* back to blocking: recv/send are guarded by select() */
	} else {
		r = connect(s, (struct sockaddr *)&a, sizeof a);
	}
	if (r != 0) {
		printf("plat: connect %s:%u failed errno %d\n", host, port, errno);
		close(s);
		return -1;
	}
	g_sock = s;
	return 0;
}

static int raw_send(void *ctx, const void *buf, size_t len)
{
	const char *p = (const char *)buf;
	size_t left = len;
	(void)ctx;
	while (left) {
		int n;
		if (wait_fd(g_sock, 1, RECV_TIMEOUT_S) <= 0) return -1;
		n = send(g_sock, p, left > 4096 ? 4096 : left, 0);
		if (n < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		p += n;
		left -= (size_t)n;
	}
	return (int)len;
}

static int raw_recv(void *ctx, void *buf, size_t len)
{
	int n;
	(void)ctx;
	if (wait_fd(g_sock, 0, RECV_TIMEOUT_S) <= 0) return -1;      /* timeout */
	n = recv(g_sock, (char *)buf, len, 0);
	return n < 0 ? -1 : n;
}

static void raw_close(void *ctx)
{
	(void)ctx;
	if (g_sock >= 0) close(g_sock);
	g_sock = -1;
}

#ifdef PHEDAV_WITH_MBEDTLS
static phedav_tls g_tls;
static char g_ca[16384];

static int hexv(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

phedav_net plat_make_net(void)
{
	phedav_raw raw = { NULL, raw_open, raw_send, raw_recv, raw_close };
	const char *ca = NULL;
	unsigned char pin[32];
	int have_pin = 0, i;
	FILE *f = fopen(PHEDAV_DIR "\\ca.pem", "rb");
	if (f) {
		size_t k = fread(g_ca, 1, sizeof g_ca - 1, f);
		fclose(f);
		g_ca[k] = 0;
		if (k) ca = g_ca;
	}
	f = fopen(PHEDAV_DIR "\\pin.txt", "r");
	if (f) {
		char hex[80];
		if (fgets(hex, sizeof hex, f) && strlen(hex) >= 64) {
			have_pin = 1;
			for (i = 0; i < 32; i++) {
				int a = hexv(hex[2 * i]), b = hexv(hex[2 * i + 1]);
				if (a < 0 || b < 0) have_pin = 0;
				pin[i] = (unsigned char)(a * 16 + b);
			}
		}
		fclose(f);
	}
	printf("plat: tls trust: ca.pem %s, pin.txt %s\n", ca ? "yes" : "no", have_pin ? "yes" : "no");
	phedav_tls_init(&g_tls, raw, ca, have_pin ? pin : NULL);
	g_tls.debug = 1;
	return phedav_tls_net(&g_tls);
}
#else
static int pn_open(void *ctx, const char *host, unsigned port, int tls)
{
	if (tls) { printf("plat: this build has no TLS (https refused)\n"); return -1; }
	return raw_open(ctx, host, port);
}

phedav_net plat_make_net(void)
{
	phedav_net n;
	n.ctx = NULL; n.open = pn_open; n.send = raw_send; n.recv = raw_recv; n.close = raw_close;
	return n;
}
#endif

/* ---------- entropy for mbedTLS: Math::Random + the fast counter + tick count + time, hashed with a small mixer ---------- */

static TUint32 mix32(TUint32 h, TUint32 v)
{
	h ^= v;
	h *= 16777619u;
	h ^= h >> 15;
	return h;
}

extern "C" int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
	TUint32 h = 2166136261u;
	size_t i;
	TTime t;
	(void)data;
	t.UniversalTime();
	h = mix32(h, (TUint32)(t.Int64() & 0xFFFFFFFF));
	h = mix32(h, (TUint32)(t.Int64() >> 32));
	h = mix32(h, User::NTickCount());
	for (i = 0; i < len; i++) {
		h = mix32(h, User::FastCounter());
		h = mix32(h, Math::Random());
		output[i] = (unsigned char)(h >> 11);
	}
	*olen = len;
	return 0;
}
