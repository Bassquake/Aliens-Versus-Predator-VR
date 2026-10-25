/* AvP internet relay.
 *
 * Hosts open a room and get a 5-character code; players join with the code. The
 * relay only forwards datagrams between a room's host and its clients - the game's
 * reliable/state protocol runs end to end through it, so nothing here needs to
 * understand a game packet. The protocol is in relay_proto.h, next to this file.
 *
 * Single-threaded, one UDP socket. Build:
 *   Linux:   cc -O2 -o avp-relay relay.c
 *   Windows: gcc -O2 -o avp-relay.exe relay.c -lws2_32   (or cl relay.c ws2_32.lib)
 * Run:       avp-relay [port]          (default 27017)
 *
 * Every client and host is identified by its source address after NAT. Lookups are
 * linear scans: at RELAY_MAX_ROOMS rooms that is a few thousand compares a packet,
 * which is nothing beside the syscall.
 */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET rsock;
#define RSOCK_BAD INVALID_SOCKET
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
typedef int rsock;
#define RSOCK_BAD (-1)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>

#include "relay_proto.h"

#define RELAY_MAX_ROOMS 256

typedef struct {
	struct sockaddr_in addr;
	uint32_t           tag;        /* 0 = slot free */
	uint32_t           last_ms;
} Client;

typedef struct {
	unsigned           code;       /* 0 = room free */
	struct sockaddr_in host;
	uint32_t           host_ms;
	uint32_t           next_tag;
	Client             cl[RELAY_ROOM_CLIENTS];
} Room;

static Room  rooms[RELAY_MAX_ROOMS];
static rsock sock = RSOCK_BAD;

static uint32_t now_ms(void)
{
#ifdef _WIN32
	return (uint32_t)GetTickCount();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint32_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
#endif
}

static void logf_(const char *fmt, ...)
{
	char tbuf[32];
	time_t t = time(NULL);
	va_list ap;
	strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", localtime(&t));
	printf("%s ", tbuf);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

static const char *addr_str(const struct sockaddr_in *a)
{
	static char buf[2][32];
	static int k;
	char ip[INET_ADDRSTRLEN];
	k ^= 1;
	inet_ntop(AF_INET, (void *)&a->sin_addr, ip, sizeof ip);
	snprintf(buf[k], sizeof buf[k], "%s:%d", ip, ntohs(a->sin_port));
	return buf[k];
}

static const char *code_str(unsigned code)
{
	static char buf[RELAY_CODE_LEN + 1];
	relay_code_to_str(code, buf);
	return buf;
}

static void wr32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static uint32_t rd32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

/* Send an envelope; 'payload' may be NULL. Rewrites the header of 'buf' in place
   when forwarding, so a forwarded packet is never copied. */
static void send_env(const struct sockaddr_in *to, unsigned char *buf, int op, uint32_t val, int plen)
{
	wr32(buf, RELAY_MAGIC);
	buf[4] = (unsigned char)op;
	buf[5] = RELAY_VERSION;
	buf[6] = buf[7] = 0;
	wr32(buf + 8, val);
	sendto(sock, (const char *)buf, RELAY_HDR + plen, 0, (const struct sockaddr *)to, sizeof *to);
}

static void send_ctrl(const struct sockaddr_in *to, int op, uint32_t val)
{
	unsigned char b[RELAY_HDR];
	send_env(to, b, op, val, 0);
}

static Room *room_by_code(unsigned code)
{
	int i;
	if (!code) return NULL;
	for (i = 0; i < RELAY_MAX_ROOMS; i++)
		if (rooms[i].code == code) return &rooms[i];
	return NULL;
}

static Room *room_by_host(const struct sockaddr_in *a)
{
	int i;
	for (i = 0; i < RELAY_MAX_ROOMS; i++)
		if (rooms[i].code && same_addr(&rooms[i].host, a)) return &rooms[i];
	return NULL;
}

static Client *client_by_addr(const struct sockaddr_in *a, Room **out)
{
	int i, j;
	for (i = 0; i < RELAY_MAX_ROOMS; i++) {
		if (!rooms[i].code) continue;
		for (j = 0; j < RELAY_ROOM_CLIENTS; j++)
			if (rooms[i].cl[j].tag && same_addr(&rooms[i].cl[j].addr, a)) { *out = &rooms[i]; return &rooms[i].cl[j]; }
	}
	return NULL;
}

static unsigned random_code(void)
{
	for (;;) {
		unsigned c = (((unsigned)rand() << 15) ^ (unsigned)rand()) % RELAY_CODE_MAX + 1;
		if (!room_by_code(c)) return c;
	}
}

static int clients_in(const Room *r)
{
	int j, n = 0;
	for (j = 0; j < RELAY_ROOM_CLIENTS; j++) if (r->cl[j].tag) n++;
	return n;
}

static void room_close(Room *r, const char *why)
{
	logf_("room %s closed (%s), %d client(s)", code_str(r->code), why, clients_in(r));
	memset(r, 0, sizeof *r);
}

static void on_host(const struct sockaddr_in *from, unsigned want)
{
	Room *r = room_by_host(from);
	int i;
	if (!r) {
		/* A host address that is also a client somewhere has left that room. */
		Room *cr;
		Client *c = client_by_addr(from, &cr);
		if (c) memset(c, 0, sizeof *c);

		for (i = 0; i < RELAY_MAX_ROOMS && rooms[i].code; i++) {}
		if (i == RELAY_MAX_ROOMS) { send_ctrl(from, RELAY_ERROR, RELAY_ERR_BUSY); return; }
		r = &rooms[i];
		memset(r, 0, sizeof *r);
		/* Give the host the code it asks for if it is free: a relay restart then
		   costs nobody a new code. */
		r->code = (want && want <= RELAY_CODE_MAX && !room_by_code(want)) ? want : random_code();
		r->host = *from;
		r->next_tag = 1;
		logf_("room %s opened by %s%s", code_str(r->code), addr_str(from),
		      want == r->code ? " (re-opened)" : "");
	}
	r->host_ms = now_ms();
	send_ctrl(from, RELAY_ROOM, r->code);
}

/* host -> client */
static void on_host_data(Room *r, unsigned char *buf, int n, uint32_t tag)
{
	int j;
	r->host_ms = now_ms();
	for (j = 0; j < RELAY_ROOM_CLIENTS; j++) {
		if (r->cl[j].tag != tag) continue;
		send_env(&r->cl[j].addr, buf, RELAY_DATA, 0, n - RELAY_HDR);
		return;
	}
	/* Unknown tag: the client expired. The game will notice by its own timeout. */
}

/* client -> host. The first packet from a new address joins the room it names. */
static void on_client_data(const struct sockaddr_in *from, unsigned char *buf, int n, unsigned code)
{
	Room *r = NULL;
	Client *c = client_by_addr(from, &r);
	if (c && r->code != code) {                     /* now joining a different room */
		memset(c, 0, sizeof *c);
		c = NULL;
	}
	if (!c) {
		int j;
		r = room_by_code(code);
		if (!r) { send_ctrl(from, RELAY_ERROR, RELAY_ERR_NOROOM); return; }
		if (same_addr(&r->host, from)) return;      /* a host talking to itself */
		for (j = 0; j < RELAY_ROOM_CLIENTS && r->cl[j].tag; j++) {}
		if (j == RELAY_ROOM_CLIENTS) { send_ctrl(from, RELAY_ERROR, RELAY_ERR_FULL); return; }
		c = &r->cl[j];
		c->addr = *from;
		c->tag  = r->next_tag++;
		if (r->next_tag > 0xFFFF) r->next_tag = 1;  /* the game keeps tags in a port field */
		logf_("room %s: %s joined as tag %u (%d client(s))",
		      code_str(r->code), addr_str(from), c->tag, clients_in(r));
	}
	c->last_ms = now_ms();
	send_env(&r->host, buf, RELAY_DATA, c->tag, n - RELAY_HDR);
}

static void on_packet(const struct sockaddr_in *from, unsigned char *buf, int n)
{
	uint32_t val;
	int op;
	if (n < RELAY_HDR || rd32(buf) != RELAY_MAGIC) return;
	if (buf[5] != RELAY_VERSION) { send_ctrl(from, RELAY_ERROR, RELAY_ERR_VERSION); return; }
	op  = buf[4];
	val = rd32(buf + 8);

	switch (op) {
	case RELAY_HOST:
		on_host(from, val);
		break;
	case RELAY_DATA: {
		Room *r = room_by_host(from);
		if (r) on_host_data(r, buf, n, val);
		else   on_client_data(from, buf, n, val);
		break;
	}
	case RELAY_CLOSE: {
		Room *r = room_by_host(from);
		if (r) room_close(r, "host closed it");
		break;
	}
	case RELAY_PING:                                /* the game's "is it up?" check */
		send_ctrl(from, RELAY_PONG, val);
		break;
	case RELAY_LEAVE: {
		Room *r;
		Client *c = client_by_addr(from, &r);
		if (c) {
			logf_("room %s: tag %u left", code_str(r->code), c->tag);
			memset(c, 0, sizeof *c);
		}
		break;
	}
	default:
		break;
	}
}

static void expire(void)
{
	uint32_t now = now_ms();
	int i, j;
	for (i = 0; i < RELAY_MAX_ROOMS; i++) {
		Room *r = &rooms[i];
		if (!r->code) continue;
		if (now - r->host_ms > RELAY_IDLE_MS) { room_close(r, "host silent"); continue; }
		for (j = 0; j < RELAY_ROOM_CLIENTS; j++) {
			if (r->cl[j].tag && now - r->cl[j].last_ms > RELAY_IDLE_MS) {
				logf_("room %s: tag %u timed out", code_str(r->code), r->cl[j].tag);
				memset(&r->cl[j], 0, sizeof r->cl[j]);
			}
		}
	}
}

int main(int argc, char **argv)
{
	struct sockaddr_in a;
	int port = argc > 1 ? atoi(argv[1]) : RELAY_PORT;
	int rcv = 4 * 1024 * 1024;
	uint32_t last_expire = 0;

#ifdef _WIN32
	WSADATA w;
	WSAStartup(MAKEWORD(2, 2), &w);
#else
	signal(SIGPIPE, SIG_IGN);
#endif
	srand((unsigned)time(NULL) ^ (unsigned)now_ms());

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock == RSOCK_BAD) { perror("socket"); return 1; }
	setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char *)&rcv, sizeof rcv);
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = INADDR_ANY;
	a.sin_port = htons((unsigned short)port);
	if (bind(sock, (struct sockaddr *)&a, sizeof a) != 0) { perror("bind"); return 1; }
#ifdef _WIN32
	{ u_long nb1 = 1; ioctlsocket(sock, FIONBIO, &nb1); }
	{
		/* Otherwise an ICMP port-unreachable from a departed peer surfaces as a
		   WSAECONNRESET on the next recvfrom. */
		#ifndef SIO_UDP_CONNRESET
		#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
		#endif
		BOOL off = FALSE; DWORD nb = 0;
		WSAIoctl(sock, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &nb, NULL, NULL);
	}
#else
	fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
#endif
	logf_("avp-relay listening on UDP %d (protocol %d, %d rooms max)", port, RELAY_VERSION, RELAY_MAX_ROOMS);

	for (;;) {
		unsigned char buf[RELAY_HDR + RELAY_MAX_PAYLOAD];
		struct sockaddr_in from;
		socklen_t fl = sizeof from;
		fd_set rs;
		struct timeval tv = { 1, 0 };
		int n;

		FD_ZERO(&rs);
		FD_SET(sock, &rs);
		if (select((int)sock + 1, &rs, NULL, NULL, &tv) > 0) {
			for (;;) {                                  /* drain: the socket is non-blocking */
				fl = sizeof from;
				n = (int)recvfrom(sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
				if (n < 0) break;
				on_packet(&from, buf, n);
			}
		}
		if (now_ms() - last_expire >= 1000) { expire(); last_expire = now_ms(); }
	}
}
