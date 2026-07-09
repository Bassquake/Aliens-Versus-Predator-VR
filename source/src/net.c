#ifdef _WIN32
/* winsock2 MUST be included before any windows.h that the game headers below
   might pull in, otherwise it clashes with the legacy winsock.h. */
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef __ANDROID__
#include <android/log.h>   /* stderr doesn't reach logcat on this build; log directly */
#define NET_LOG(...) __android_log_print(ANDROID_LOG_INFO, "AVP_NET", __VA_ARGS__)
#else
#define NET_LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

#include "fixer.h"

#include "3dc.h"
#include "inline.h"
#include "module.h"
#include "stratdef.h"
#include "equipmnt.h"

#include "pldnet.h"
#include "net.h"
#include "avp_menus.h"   /* SESSION_DESC / SessionData for the join session list */


DPID AVPDPNetID;
int QuickStartMultiplayer=1;
DPNAME AVPDPplayerName;
int glpDP; /* directplay object */

/* ============================================================================
 * LAN multiplayer transport  (milestone 1: raw framed TCP, host/join, relay).
 *
 * Star topology: the host binds+listens; clients connect. Every message is
 * length-framed. When a client sends to DPID_ALLPLAYERS the host relays it to
 * all OTHER clients and enqueues a local copy, reproducing DirectPlay's "one
 * send reaches everyone but the sender" behaviour over point-to-point sockets.
 *
 * Sockets are pumped on demand from DpExtRecv(), which the engine calls every
 * frame ([Minimal]NetCollectMessages), so no reader thread is needed yet.
 *
 * This milestone just moves bytes + establishes connections. Player roster and
 * DPSYS_CREATE/DESTROY system messages are milestone 2.
 * ========================================================================== */
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET NetSocket;
#define NET_BAD INVALID_SOCKET
#define net_closesock closesocket
static int net_wouldblock(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }
static int net_inprogress(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <ifaddrs.h>
#include <net/if.h>
typedef int NetSocket;
#define NET_BAD (-1)
#define net_closesock close
static int net_wouldblock(void) { return errno == EWOULDBLOCK || errno == EAGAIN; }
static int net_inprogress(void) { return errno == EINPROGRESS; }
#endif

#define NET_CONNECT_TIMEOUT_MS   2000   /* async connect: give up after this */
#define NET_HANDSHAKE_TIMEOUT_MS 3000   /* async connect: WELCOME must arrive within this */

static uint32_t net_now_ms(void) {
#ifdef _WIN32
	return (uint32_t)GetTickCount();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint32_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
#endif
}

/* Asynchronous join state (driven each frame by DirectPlay_ConnectingToSession). */
typedef enum { NET_JS_NONE = 0, NET_JS_CONNECTING, NET_JS_HANDSHAKE, NET_JS_DONE, NET_JS_FAILED } NetJoinState;

#define NET_PORT        27015       /* TCP port used for host/join */
#define NET_MAXCONNS    8           /* host: max simultaneous clients */
#define NET_HOST_DPID   1           /* host's fixed player id */
#define NET_FRAMEMAX    (16*1024)   /* max single message payload */
#define NET_HDR_SIZE    12          /* wire header: len(4)+from(4)+to(4), LE */
#define NET_CTRL_FROM   (-1)        /* reserved 'from' => internal control frame */
#define NETCTRL_WELCOME 1           /* host->client: [op][assigned DPID] */
#define NETCTRL_HELLO   2           /* client->host: [op][player name bytes] */

/* LAN auto-discovery (UDP broadcast): clients broadcast a QUERY; hosts reply
   with their session name + player count. The host's IP is taken from the reply
   packet's source address, so no manual IP entry is needed. */
#define NET_DISC_PORT   27016
#define NET_DISC_MAGIC  0x41565044  /* 'AVPD' */
#define NETDISC_QUERY   1
#define NETDISC_REPLY   2
#define NET_DISC_HDR    16          /* magic(4)+op(4)+tcpPort(4)+playerCount(4), name follows */

typedef enum { NET_OFF = 0, NET_HOST, NET_CLIENT } NetRole;

typedef struct {
	NetSocket     sock;
	DPID          dpid;                              /* remote player's id */
	unsigned char in[NET_HDR_SIZE + NET_FRAMEMAX];   /* inbound reassembly buffer */
	int           inlen;                             /* bytes currently buffered */
} NetConn;

static NetRole   net_role     = NET_OFF;
static NetSocket net_listen   = NET_BAD;             /* host only */
static NetConn   net_conns[NET_MAXCONNS];            /* host: clients; client: [0]=host link */
static int       net_nconns   = 0;
static DPID      net_next_dpid = NET_HOST_DPID + 1;  /* host assigns to joiners */

/* received-message FIFO: filled by net_pump(), drained by DpExtRecv() */
typedef struct NetMsg {
	struct NetMsg *next;
	DPID           from, to;
	int            size;
	unsigned char  data[1];                          /* flexible array (size bytes) */
} NetMsg;
static NetMsg       *net_qhead = NULL, *net_qtail = NULL;
static unsigned char net_scratch[NET_FRAMEMAX];      /* buffer handed back by DpExtRecv */

/* discovery state */
static NetSocket net_udp = NET_BAD;                  /* host: responder; browsing: broadcaster */
static int       net_browsing = 0;
static struct sockaddr_in net_disc_addr[MAX_NO_OF_SESSIONS];   /* discovered host addresses */
static char      net_disc_names[MAX_NO_OF_SESSIONS][40];
static int       net_disc_count = 0;
static int       net_browse_tick = 0;
static char      net_host_sessionname[64] = "AvP Game";

/* async join state */
static NetJoinState net_join_state = NET_JS_NONE;
static NetSocket    net_join_sock  = NET_BAD;
static uint32_t     net_join_start = 0;

static void net_wr32(unsigned char *p, int32_t v) {
	p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8);
	p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24);
}
static int32_t net_rd32(const unsigned char *p) {
	return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24));
}

static void net_setnonblock(NetSocket s) {
#ifdef _WIN32
	u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
#else
	int fl = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

/* Send a full buffer over a non-blocking socket, spinning briefly on
   EWOULDBLOCK. Fine for AvP's small messages; proper outbound queuing later. */
static int net_send_all(NetSocket s, const unsigned char *buf, int len) {
	int off = 0, spins = 0;
	while (off < len) {
		int n = (int)send(s, (const char *)buf + off, len - off, 0);
		if (n > 0) { off += n; spins = 0; continue; }
		if (n < 0 && net_wouldblock()) { if (++spins > 1000000) return -1; continue; }
		return -1;
	}
	return 0;
}

static int net_send_frame(NetSocket s, DPID from, DPID to, const void *data, int size) {
	unsigned char hdr[NET_HDR_SIZE];
	if (size < 0 || size > NET_FRAMEMAX) return -1;
	net_wr32(hdr,   size);
	net_wr32(hdr+4, from);
	net_wr32(hdr+8, to);
	if (net_send_all(s, hdr, NET_HDR_SIZE) != 0) return -1;
	if (size && net_send_all(s, (const unsigned char *)data, size) != 0) return -1;
	return 0;
}

static void net_enqueue(DPID from, DPID to, const unsigned char *data, int size) {
	NetMsg *m = (NetMsg *)malloc(sizeof(NetMsg) + (size > 0 ? (size_t)size : 0));
	if (!m) return;
	m->next = NULL; m->from = from; m->to = to; m->size = size;
	if (size) memcpy(m->data, data, (size_t)size);
	if (net_qtail) net_qtail->next = m; else net_qhead = m;
	net_qtail = m;
}

/* Synthesize the DirectPlay system messages the engine expects. These structs
   carry a POINTER to the player name (DPNAME.lpszShortNameA); ProcessSystemMessage
   dereferences it immediately after DpExtRecv returns, so we point it at a small
   ring of persistent name buffers. */
#define NET_NAMESLOTS 32
static char net_nameslots[NET_NAMESLOTS][NET_PLAYERNAMELENGTH];
static int  net_nameslot_next = 0;

static void net_enqueue_create(DPID id, const char *name) {
	DPMSG_CREATEPLAYERORGROUP m;
	char *slot = net_nameslots[net_nameslot_next];
	net_nameslot_next = (net_nameslot_next + 1) % NET_NAMESLOTS;
	strncpy(slot, (name && name[0]) ? name : "player", NET_PLAYERNAMELENGTH - 1);
	slot[NET_PLAYERNAMELENGTH - 1] = '\0';
	memset(&m, 0, sizeof m);
	m.dwType = DPSYS_CREATEPLAYERORGROUP;
	m.dpId = id;
	m.dwPlayerType = DPPLAYERTYPE_PLAYER;
	m.dpnName.dwSize = sizeof(DPNAME);
	m.dpnName.lpszShortNameA = slot;
	m.dpnName.lpszLongNameA  = slot;
	net_enqueue(DPID_SYSMSG, AVPDPNetID, (const unsigned char *)&m, (int)sizeof m);
	fprintf(stderr, "net: + player DPID %d '%s'\n", (int)id, slot);
}

static void net_enqueue_destroy(DPID id) {
	DPMSG_DESTROYPLAYERORGROUP m;
	memset(&m, 0, sizeof m);
	m.dwType = DPSYS_DESTROYPLAYERORGROUP;
	m.dpId = id;
	m.dwPlayerType = DPPLAYERTYPE_PLAYER;
	net_enqueue(DPID_SYSMSG, AVPDPNetID, (const unsigned char *)&m, (int)sizeof m);
	fprintf(stderr, "net: - player DPID %d\n", (int)id);
}

static void net_enqueue_sessionlost(void) {
	DPMSG_GENERIC m;
	memset(&m, 0, sizeof m);
	m.dwType = DPSYS_SESSIONLOST;
	net_enqueue(DPID_SYSMSG, AVPDPNetID, (const unsigned char *)&m, (int)sizeof m);
}

static void net_conn_close(int i) {
	DPID id = net_conns[i].dpid;
	net_closesock(net_conns[i].sock);
	net_conns[i] = net_conns[net_nconns - 1];   /* compact */
	net_nconns--;
	if (net_role == NET_HOST)        net_enqueue_destroy(id);       /* host: player left */
	else if (net_role == NET_CLIENT) net_enqueue_sessionlost();     /* client: host lost */
}

/* Act on one fully-received frame from connection index 'ci'. */
static void net_handle_frame(int ci, DPID from, DPID to, unsigned char *data, int size) {
	if (from == NET_CTRL_FROM) {                    /* internal control channel */
		int op = (size >= 4) ? net_rd32(data) : 0;
		if (op == NETCTRL_WELCOME && size >= 8 && net_role == NET_CLIENT) {
			AVPDPNetID = net_rd32(data + 4);
			fprintf(stderr, "net: assigned DPID %d by host\n", (int)AVPDPNetID);
		} else if (op == NETCTRL_HELLO && net_role == NET_HOST) {
			/* client sent its player name -> register it so the host's game and
			   the roster / GameDescription include this player */
			char nm[NET_PLAYERNAMELENGTH];
			int nlen = size - 4;
			if (nlen < 0) nlen = 0;
			if (nlen > NET_PLAYERNAMELENGTH - 1) nlen = NET_PLAYERNAMELENGTH - 1;
			memcpy(nm, data + 4, (size_t)nlen);
			nm[nlen] = '\0';
			net_enqueue_create(net_conns[ci].dpid, nm);
		}
		return;
	}
	if (net_role == NET_HOST) {
		int j;
		net_enqueue(from, to, data, size);          /* host's own game sees it */
		for (j = 0; j < net_nconns; j++) {          /* relay to the other clients */
			if (j == ci) continue;
			if (to == DPID_ALLPLAYERS || to == net_conns[j].dpid)
				net_send_frame(net_conns[j].sock, from, to, data, size);
		}
	} else {
		net_enqueue(from, to, data, size);          /* client: it's for us */
	}
}

/* Drain readable bytes from one connection, extracting complete frames.
   Extraction is interleaved with reading: we pull out every complete frame
   before reading more, so a backlog larger than the buffer (a burst, or a pump
   delayed by a frame hitch) is processed in chunks instead of overflowing the
   16 KB buffer and getting force-closed — which used to masquerade as the peer
   leaving and rejoining ("has left" / "has connected" spam). */
static void net_pump_conn(int ci) {
	NetConn *c = &net_conns[ci];
	for (;;) {
		int off = 0, space, n;

		/* Extract all complete frames currently buffered, then compact. */
		while (c->inlen - off >= NET_HDR_SIZE) {
			int32_t size = net_rd32(c->in + off);
			int32_t from = net_rd32(c->in + off + 4);
			int32_t to   = net_rd32(c->in + off + 8);
			if (size < 0 || size > NET_FRAMEMAX) { net_conn_close(ci); return; }
			if (c->inlen - off - NET_HDR_SIZE < size) break;    /* wait for the rest */
			net_handle_frame(ci, from, to, c->in + off + NET_HDR_SIZE, size);
			off += NET_HDR_SIZE + size;
		}
		if (off > 0) { memmove(c->in, c->in + off, c->inlen - off); c->inlen -= off; }

		/* Read more. Buffer full with no complete frame => a single frame really
		   exceeds NET_FRAMEMAX, i.e. genuine desync. */
		space = (int)sizeof(c->in) - c->inlen;
		if (space <= 0) { net_conn_close(ci); return; }
		n = (int)recv(c->sock, (char *)c->in + c->inlen, space, 0);
		if (n > 0) { c->inlen += n; continue; }              /* loop: extract, read again */
		if (n == 0) { net_conn_close(ci); return; }          /* peer closed */
		if (net_wouldblock()) break;                         /* nothing more now */
		net_conn_close(ci); return;                          /* error */
	}
}

static void net_udp_respond(void);   /* forward decl (defined below) */

/* Accept new clients (host) and read from all connections. Cheap to call often. */
static void net_pump(void) {
	int i;
	if (net_role == NET_OFF) return;

	if (net_role == NET_HOST) net_udp_respond();

	if (net_role == NET_HOST && net_listen != NET_BAD && net_nconns < NET_MAXCONNS) {
		NetSocket cs = accept(net_listen, NULL, NULL);
		if (cs != NET_BAD) {
			NetConn *c;
			DPID id;
			unsigned char wel[8];
			int one = 1;
			net_setnonblock(cs);
			setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
			id = net_next_dpid++;
			if (id == DPID_SYSMSG || id == DPID_ALLPLAYERS) id = net_next_dpid++;  /* skip reserved */
			c = &net_conns[net_nconns++];
			c->sock = cs; c->dpid = id; c->inlen = 0;
			net_wr32(wel, NETCTRL_WELCOME);
			net_wr32(wel + 4, id);
			net_send_frame(cs, NET_CTRL_FROM, id, wel, sizeof wel);
			fprintf(stderr, "net: client connected, assigned DPID %d (%d players)\n", (int)id, net_nconns + 1);
			/* milestone 2: send roster + synthesize DPSYS_CREATEPLAYERORGROUP */
		}
	}
	for (i = net_nconns - 1; i >= 0; i--) net_pump_conn(i);
}

static void net_teardown(void) {
	int i;
	for (i = 0; i < net_nconns; i++) net_closesock(net_conns[i].sock);
	net_nconns = 0;
	if (net_listen != NET_BAD)   { net_closesock(net_listen);   net_listen   = NET_BAD; }
	if (net_udp != NET_BAD)      { net_closesock(net_udp);      net_udp      = NET_BAD; }
	if (net_join_sock != NET_BAD){ net_closesock(net_join_sock);net_join_sock= NET_BAD; }
	net_join_state = NET_JS_NONE;
	net_browsing = 0;
	while (net_qhead) { NetMsg *m = net_qhead; net_qhead = m->next; free(m); }
	net_qtail = NULL;
	net_role = NET_OFF;
	net_next_dpid = NET_HOST_DPID + 1;
}

/* Host: answer LAN discovery QUERYs with our session name + player count. */
static void net_udp_respond(void) {
	unsigned char pkt[128];
	if (net_udp == NET_BAD) return;
	for (;;) {
		struct sockaddr_in from;
		socklen_t fl = sizeof from;
		int n = (int)recvfrom(net_udp, (char *)pkt, sizeof pkt, 0, (struct sockaddr *)&from, &fl);
		if (n < 0) break;
		if (n < 8) continue;
		if (net_rd32(pkt) != (int32_t)NET_DISC_MAGIC || net_rd32(pkt + 4) != NETDISC_QUERY) continue;
		{
			unsigned char rep[128];
			int nl = (int)strlen(net_host_sessionname);
			if (nl > (int)sizeof net_disc_names[0] - 1) nl = (int)sizeof net_disc_names[0] - 1;
			net_wr32(rep,      NET_DISC_MAGIC);
			net_wr32(rep + 4,  NETDISC_REPLY);
			net_wr32(rep + 8,  NET_PORT);
			net_wr32(rep + 12, net_nconns + 1);           /* current player count */
			memcpy(rep + NET_DISC_HDR, net_host_sessionname, (size_t)nl);
			sendto(net_udp, (const char *)rep, NET_DISC_HDR + nl, 0, (struct sockaddr *)&from, fl);
		}
	}
}

static void net_winsock_init(void) {
#ifdef _WIN32
	static int started = 0;
	if (!started) { WSADATA w; WSAStartup(MAKEWORD(2,2), &w); started = 1; }
#endif
}

/* Host: open the UDP discovery socket so LAN clients can find us. Best-effort;
   hosting still works (via manual IP) if this fails. */
static void net_udp_open_host(void) {
	struct sockaddr_in a;
	int one = 1;
	net_udp = socket(AF_INET, SOCK_DGRAM, 0);
	if (net_udp == NET_BAD) return;
	setsockopt(net_udp, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(NET_DISC_PORT);
	if (bind(net_udp, (struct sockaddr*)&a, sizeof a) != 0) { net_closesock(net_udp); net_udp = NET_BAD; return; }
	net_setnonblock(net_udp);
}

static int net_start_host(void) {
	NetSocket s;
	struct sockaddr_in a;
	int one = 1;
	net_teardown();
	net_winsock_init();
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == NET_BAD) return 0;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(NET_PORT);
	if (bind(s, (struct sockaddr*)&a, sizeof a) != 0) { net_closesock(s); return 0; }
	if (listen(s, NET_MAXCONNS) != 0) { net_closesock(s); return 0; }
	net_setnonblock(s);
	net_listen = s;
	net_role = NET_HOST;
	net_next_dpid = NET_HOST_DPID + 1;
	AVPDPNetID = NET_HOST_DPID;
	glpDP = 1;
	net_udp_open_host();                      /* answer LAN discovery */
	fprintf(stderr, "net: hosting on port %d (host DPID %d), discovery on %d\n",
		NET_PORT, NET_HOST_DPID, NET_DISC_PORT);
	return 1;
}

/* Start a non-blocking TCP connect to a host. Never blocks; the connection and
   DPID handshake are then advanced a step at a time by net_join_pump() each
   frame, so the game loop keeps running throughout. */
static void net_begin_connect(const struct sockaddr_in *addr) {
	int r;
	net_teardown();
	net_winsock_init();
	net_join_state = NET_JS_FAILED;
	net_join_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (net_join_sock == NET_BAD) return;
	net_setnonblock(net_join_sock);
	r = connect(net_join_sock, (const struct sockaddr*)addr, sizeof *addr);
	if (r != 0 && !net_inprogress()) {                 /* immediate hard failure */
		net_closesock(net_join_sock); net_join_sock = NET_BAD;
		return;
	}
	net_join_start = net_now_ms();
	net_join_state = NET_JS_CONNECTING;                 /* r==0 handled next pump too */
	fprintf(stderr, "net: connecting...\n");
}

/* Advance the async connect one step (no blocking). Returns the current state. */
static int net_join_pump(void) {
	if (net_join_state == NET_JS_CONNECTING) {
		fd_set wf;
		struct timeval tv;
		FD_ZERO(&wf); FD_SET(net_join_sock, &wf);
		tv.tv_sec = 0; tv.tv_usec = 0;                 /* poll, don't wait */
		if (select((int)net_join_sock + 1, NULL, &wf, NULL, &tv) > 0) {
			int err = 0, one = 1;
			socklen_t el = sizeof err;
			getsockopt(net_join_sock, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
			if (err != 0) {
				fprintf(stderr, "net: connect refused (%d)\n", err);
				net_join_state = NET_JS_FAILED;
			} else {
				/* connected: adopt the socket as our host link, send HELLO */
				setsockopt(net_join_sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
				net_conns[0].sock = net_join_sock; net_conns[0].dpid = NET_HOST_DPID; net_conns[0].inlen = 0;
				net_nconns = 1;
				net_role = NET_CLIENT;
				glpDP = 1;
				AVPDPNetID = 0;
				net_join_sock = NET_BAD;               /* ownership moved to net_conns[0] */
				{
					extern char MP_PlayerName[];
					unsigned char hello[4 + NET_PLAYERNAMELENGTH];
					int nlen = (int)strlen(MP_PlayerName);
					if (nlen > NET_PLAYERNAMELENGTH - 1) nlen = NET_PLAYERNAMELENGTH - 1;
					net_wr32(hello, NETCTRL_HELLO);
					if (nlen > 0) memcpy(hello + 4, MP_PlayerName, (size_t)nlen);
					net_send_frame(net_conns[0].sock, NET_CTRL_FROM, NET_HOST_DPID, hello, 4 + nlen);
				}
				net_join_start = net_now_ms();
				net_join_state = NET_JS_HANDSHAKE;
				fprintf(stderr, "net: connected, awaiting DPID\n");
			}
		} else if (net_now_ms() - net_join_start > NET_CONNECT_TIMEOUT_MS) {
			fprintf(stderr, "net: connect timed out\n");
			net_join_state = NET_JS_FAILED;
		}
	} else if (net_join_state == NET_JS_HANDSHAKE) {
		net_pump();                                    /* reads WELCOME -> AVPDPNetID */
		if (AVPDPNetID != 0) net_join_state = NET_JS_DONE;
		else if (net_now_ms() - net_join_start > NET_HANDSHAKE_TIMEOUT_MS) {
			fprintf(stderr, "net: no WELCOME from host\n");
			net_join_state = NET_JS_FAILED;
		}
	}
	return (int)net_join_state;
}

/* Send a discovery QUERY to the directed broadcast of every local IPv4 interface,
   plus the limited broadcast (255.255.255.255). Sending only to the limited
   broadcast fails on multi-homed hosts: a Windows PC with WSL / Hyper-V / VMware /
   Docker virtual adapters routes 255.255.255.255 out one (often wrong) adapter, so
   a host on another interface's subnet (e.g. the Quest on Wi-Fi) never hears it —
   which is exactly why Windows could host but not join a Quest-hosted game. */
static void net_send_query_broadcasts(NetSocket sock, const unsigned char *pkt, int len) {
	struct sockaddr_in b;
	memset(&b, 0, sizeof b);
	b.sin_family = AF_INET;
	b.sin_port   = htons(NET_DISC_PORT);

	b.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	sendto(sock, (const char *)pkt, len, 0, (struct sockaddr *)&b, sizeof b);

#ifdef _WIN32
	{
		INTERFACE_INFO ifl[32];
		DWORD nb = 0;
		if (WSAIoctl(sock, SIO_GET_INTERFACE_LIST, NULL, 0, ifl, sizeof ifl, &nb, NULL, NULL) == 0) {
			int i, n = (int)(nb / sizeof(INTERFACE_INFO));
			for (i = 0; i < n; i++) {
				struct sockaddr_in *ad = (struct sockaddr_in *)&ifl[i].iiAddress;
				struct sockaddr_in *mk = (struct sockaddr_in *)&ifl[i].iiNetmask;
				u_long flags = ifl[i].iiFlags;
				if (!(flags & IFF_UP) || (flags & IFF_LOOPBACK)) continue;
				if (ad->sin_addr.s_addr == 0) continue;
				b.sin_addr.s_addr = ad->sin_addr.s_addr | ~mk->sin_addr.s_addr;
				sendto(sock, (const char *)pkt, len, 0, (struct sockaddr *)&b, sizeof b);
			}
		}
	}
#else
	{
		struct ifaddrs *ifap = NULL, *ifa;
		if (getifaddrs(&ifap) == 0) {
			for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
				struct sockaddr_in *ad, *mk;
				if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
				if (!(ifa->ifa_flags & IFF_BROADCAST) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
				ad = (struct sockaddr_in *)ifa->ifa_addr;
				mk = (struct sockaddr_in *)ifa->ifa_netmask;
				b.sin_addr.s_addr = ad->sin_addr.s_addr | (mk ? ~mk->sin_addr.s_addr : 0xFFFFFFFFu);
				sendto(sock, (const char *)pkt, len, 0, (struct sockaddr *)&b, sizeof b);
			}
			freeifaddrs(ifap);
		}
	}
#endif
}

/* Start browsing the LAN for hosts (opens a broadcasting UDP socket). Seeds the
   list with the manually entered IP, if any, so both paths coexist. */
static void net_start_browse(void) {
	int one = 1;
	net_teardown();
	net_winsock_init();
	net_udp = socket(AF_INET, SOCK_DGRAM, 0);
	if (net_udp != NET_BAD) {
		setsockopt(net_udp, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof one);
		net_setnonblock(net_udp);
	}
	net_browsing = 1;
	net_disc_count = 0;
	net_browse_tick = 0;
	{
		extern char IPAddressString[];
		struct sockaddr_in a;
		memset(&a, 0, sizeof a);
		a.sin_family = AF_INET; a.sin_port = htons(NET_PORT);
		if (IPAddressString[0] && inet_pton(AF_INET, IPAddressString, &a.sin_addr) == 1) {
			net_disc_addr[0] = a;
			strncpy(net_disc_names[0], IPAddressString, sizeof net_disc_names[0] - 1);
			net_disc_names[0][sizeof net_disc_names[0] - 1] = '\0';
			net_disc_count = 1;
		}
	}
	fprintf(stderr, "net: browsing LAN for games (UDP %d)\n", NET_DISC_PORT);
}

/* Broadcast a QUERY periodically and fold any REPLYs into the discovered list.
   Returns non-zero when a new host appeared. */
static int net_pump_browse(void) {
	unsigned char pkt[128];
	int changed = 0;
	if (!net_browsing || net_udp == NET_BAD) return 0;

	if ((net_browse_tick++ % 30) == 0) {           /* ~ twice a second at 60fps */
		net_wr32(pkt, NET_DISC_MAGIC);
		net_wr32(pkt + 4, NETDISC_QUERY);
		net_send_query_broadcasts(net_udp, pkt, 8);
	}

	for (;;) {
		struct sockaddr_in from;
		socklen_t fl = sizeof from;
		int i, slot = -1;
		int n = (int)recvfrom(net_udp, (char*)pkt, sizeof pkt, 0, (struct sockaddr*)&from, &fl);
		if (n < 0) break;
		if (n < NET_DISC_HDR) continue;
		if (net_rd32(pkt) != (int32_t)NET_DISC_MAGIC || net_rd32(pkt + 4) != NETDISC_REPLY) continue;
		for (i = 0; i < net_disc_count; i++)
			if (net_disc_addr[i].sin_addr.s_addr == from.sin_addr.s_addr) { slot = i; break; }
		if (slot < 0 && net_disc_count < MAX_NO_OF_SESSIONS) {
			int port = net_rd32(pkt + 8);
			slot = net_disc_count++;
			net_disc_addr[slot] = from;
			net_disc_addr[slot].sin_port = htons((unsigned short)(port ? port : NET_PORT));
			changed = 1;
			fprintf(stderr, "net: found host at %s\n", inet_ntoa(from.sin_addr));
		}
		if (slot >= 0) {
			int nl = n - NET_DISC_HDR;
			if (nl > (int)sizeof net_disc_names[0] - 1) nl = (int)sizeof net_disc_names[0] - 1;
			if (nl < 0) nl = 0;
			memcpy(net_disc_names[slot], pkt + NET_DISC_HDR, (size_t)nl);
			net_disc_names[slot][nl] = '\0';
		}
	}
	return changed;
}

BOOL DpExtInit(DWORD cGrntdBufs, DWORD cBytesPerBuf, BOOL bErrChcks)
{
	fprintf(stderr, "DpExtInit(%d, %d, %d)\n", cGrntdBufs, cBytesPerBuf, bErrChcks);
	/* Cross-platform wire-layout check: these MUST be identical on the Quest
	   (clang) and Windows (MSVC) builds or messages are read at the wrong offsets
	   (garbage player IDs, "connected/left" spam). Compare the two platforms' logs. */
	NET_LOG("net: layout NET_MAXPLAYERS=%d hdr=%d playerdata=%d gamedesc=%d\n",
		(int)NET_MAXPLAYERS,
		(int)sizeof(NETMESSAGEHEADER),
		(int)sizeof(GAMEDESCRIPTION_PLAYERDATA),
		(int)sizeof(NETMESSAGE_GAMEDESCRIPTION));
	net_winsock_init();
	return TRUE;
}

void DpExtUnInit()
{
	fprintf(stderr, "DpExtUnInit()\n");
	net_teardown();
}

/* Dequeue one received message. Returns DP_OK (0) if one was returned, or a
   non-zero (no-message) code so the caller's drain loop terminates. The data
   pointer is valid until the next DpExtRecv call. */
HRESULT DpExtRecv(int lpDP2A, void *lpidFrom, void *lpidTo, DWORD dwFlags, void *lplpData, LPDWORD lpdwDataSize)
{
	NetMsg *m;
	net_pump();
	if (!net_qhead) return 1;   /* no messages */
	m = net_qhead;
	net_qhead = m->next;
	if (!net_qhead) net_qtail = NULL;
	*(DPID *)lpidFrom = m->from;
	*(DPID *)lpidTo   = m->to;
	if (m->size > 0) memcpy(net_scratch, m->data, (size_t)m->size);
	*(unsigned char **)lplpData = net_scratch;
	*lpdwDataSize = (DWORD)m->size;
	free(m);
	return DP_OK;
}

/* Send one message. Client -> host link; host -> fan out to clients (the sender
   never receives its own DPID_ALLPLAYERS message, matching DirectPlay). */
HRESULT DpExtSend(int lpDP2A, DPID idFrom, DPID idTo, DWORD dwFlags, void *lpData, DWORD dwDataSize)
{
	if (net_role == NET_OFF) return 1;
	if ((int)dwDataSize > NET_FRAMEMAX) return DPERR_SENDTOOBIG;
	if (net_role == NET_CLIENT) {
		if (net_nconns < 1) return DPERR_CONNECTIONLOST;
		net_send_frame(net_conns[0].sock, idFrom, idTo, lpData, (int)dwDataSize);
	} else {
		int j;
		for (j = 0; j < net_nconns; j++) {
			if (idTo == DPID_ALLPLAYERS || idTo == net_conns[j].dpid)
				net_send_frame(net_conns[j].sock, idFrom, idTo, lpData, (int)dwDataSize);
		}
	}
	return DP_OK;
}

/* directplay.c */
int DirectPlay_ConnectingToLobbiedGame(char* playerName)
{
	fprintf(stderr, "DirectPlay_ConnectingToLobbiedGame(%s)\n", playerName);
	
	return 0;
}

int DirectPlay_ConnectingToSession()
{
	/* Polled each frame while the "Joining..." screen is up. Advances the async
	   connect without blocking. Returns 0 to abort (back a menu), the join-config
	   menu id on success, or the joining menu id to keep waiting. */
	int st = net_join_pump();
	if (st == NET_JS_DONE) {
		InitAVPNetGameForJoin();               /* AVPDPNetID assigned; become a peer */
		return AVPMENU_MULTIPLAYER_CONFIG_JOIN;
	}
	if (st == NET_JS_CONNECTING || st == NET_JS_HANDSHAKE)
		return AVPMENU_MULTIPLAYER_JOINING;    /* non-zero, != CONFIG_JOIN => keep waiting */
	net_teardown();                            /* FAILED / NONE */
	return 0;
}

BOOL DirectPlay_UpdateSessionList(int *SelectedItem)
{
	/* While browsing, poll LAN discovery and mirror the found hosts into the
	   session list. Return TRUE only when the list actually changed, else the
	   menu rebuilds (and resets the selection) every frame. */
	extern int NumberOfSessionsFound;
	int changed, i;
	(void)SelectedItem;

	if (!net_browsing) return 0;

	changed = net_pump_browse();
	for (i = 0; i < net_disc_count; i++) {
		strncpy(SessionData[i].Name, net_disc_names[i], sizeof SessionData[i].Name - 1);
		SessionData[i].Name[sizeof SessionData[i].Name - 1] = '\0';
		SessionData[i].levelIndex   = 0;
		SessionData[i].Guid         = i + 1;
		SessionData[i].AllowedToJoin = 1;
	}
	if (NumberOfSessionsFound != net_disc_count) {
		NumberOfSessionsFound = net_disc_count;
		changed = 1;
	}
	return changed ? 1 : 0;
}

int DirectPlay_JoinGame()
{
	/* Entering the "select session" screen: start LAN discovery. Hosts appear
	   in the session list automatically; the manually entered IP (if any) is
	   seeded as an entry too. The actual join happens on ConnectToSession. */
	fprintf(stderr, "DirectPlay_JoinGame() (browse LAN)\n");
	net_start_browse();
	return 1;
}

void DirectPlay_EnumConnections()
{
	fprintf(stderr, "DirectPlay_EnumConnections()\n");
	
	netGameData.tcpip_available = 1;
	netGameData.ipx_available = 0;
	netGameData.modem_available = 0;
	netGameData.serial_available = 0;                        
}

int DirectPlay_HostGame(char *playerName, char *sessionName,int species,int gamestyle,int level)
{
	extern int DetermineAvailableCharacterTypes(int);
	
	int maxPlayers=DetermineAvailableCharacterTypes(FALSE);
	if(maxPlayers<1) maxPlayers=1;
	if(maxPlayers>8) maxPlayers=8;
	
	if(!netGameData.skirmishMode) {
		fprintf(stderr, "DirectPlay_HostGame(%s, %s, %d, %d, %d)\n", playerName, sessionName, species, gamestyle, level);

		/* Remember the session name for discovery replies (before net_start_host
		   opens the responder). */
		if (sessionName && sessionName[0]) {
			strncpy(net_host_sessionname, sessionName, sizeof net_host_sessionname - 1);
			net_host_sessionname[sizeof net_host_sessionname - 1] = '\0';
		}

		/* Open the listening + discovery sockets; sets AVPDPNetID and glpDP. */
		if (!net_start_host()) {
			fprintf(stderr, "net: failed to start host on port %d\n", NET_PORT);
			return 0;
		}

		memset(&AVPDPplayerName, 0, sizeof(AVPDPplayerName));
		AVPDPplayerName.dwSize = sizeof(DPNAME);
		AVPDPplayerName.lpszShortNameA  = playerName;
		AVPDPplayerName.lpszLongNameA = playerName;
	} else {
		//fake multiplayer
		//need to set the id to an non zero value
		AVPDPNetID=100;
		
		memset(&AVPDPplayerName, 0, sizeof(AVPDPplayerName));
		AVPDPplayerName.dwSize = sizeof(DPNAME);
		AVPDPplayerName.lpszShortNameA  = playerName;
		AVPDPplayerName.lpszLongNameA = playerName;
	}
	
	InitAVPNetGameForHost(species,gamestyle,level);

	/* Register the host's own player (DirectPlay would raise a CREATE for it).
	   Processed once we're I_Host, which InitAVPNetGameForHost has just set. */
	if (!netGameData.skirmishMode)
		net_enqueue_create(AVPDPNetID, playerName);

	return 1;
}

int DirectPlay_ConnectToSession(int sessionNumber, char *playerName)
{
	/* The player picked a discovered session: kick off a NON-BLOCKING connect and
	   go to the "Joining..." screen, which polls DirectPlay_ConnectingToSession()
	   each frame to finish the handshake and become a joining peer. Nothing here
	   blocks the game loop. */
	struct sockaddr_in target;
	fprintf(stderr, "DirectPlay_ConnectToSession(%d, %s)\n", sessionNumber, playerName);

	if (sessionNumber < 0 || sessionNumber >= net_disc_count) {
		fprintf(stderr, "net: ConnectToSession: bad session index\n");
		return 0;
	}
	target = net_disc_addr[sessionNumber];   /* copy before net_begin_connect tears down browse */

	memset(&AVPDPplayerName, 0, sizeof(AVPDPplayerName));
	AVPDPplayerName.dwSize = sizeof(DPNAME);
	AVPDPplayerName.lpszShortNameA = playerName;
	AVPDPplayerName.lpszLongNameA  = playerName;

	net_begin_connect(&target);
	if (net_join_state == NET_JS_FAILED) {   /* couldn't even create/start the socket */
		fprintf(stderr, "net: could not start connect\n");
		return 0;
	}
	return 1;                                 /* -> AVPMENU_MULTIPLAYER_JOINING */
}

int DirectPlay_Disconnect()
{
	fprintf(stderr, "DirectPlay_Disconnect()\n");
	net_teardown();
	AVPDPNetID = 0;
	glpDP = 0;
	return 1;
}

HRESULT IDirectPlayX_GetPlayerName(int glpDP, DPID id, void *data, void *size)
{
	fprintf(stderr, "IDirectPlayX_GetPlayerName(%d, %d, %p, %p)\n", glpDP, id, data, size);

	return 1;
}

/* End of Linux-related junk */
