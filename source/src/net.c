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

#include "fixer.h"

#include "3dc.h"
#include "inline.h"
#include "module.h"
#include "stratdef.h"
#include "equipmnt.h"

#include "pldnet.h"
#include "net.h"
#include "avp_menus.h"   /* SESSION_DESC / SessionData for the join session list */
#include "relay_proto.h"


DPID AVPDPNetID;
int QuickStartMultiplayer=1;
DPNAME AVPDPplayerName;
int glpDP; /* directplay object */

/* ============================================================================
 * Multiplayer transport: UDP, two delivery classes, host relay.
 *
 * Star topology: the host binds a UDP port; clients send to it. When a client
 * sends to DPID_ALLPLAYERS the host relays it to all OTHER clients and enqueues
 * a local copy, reproducing DirectPlay's "one send reaches everyone but the
 * sender" behaviour.
 *
 * Every message is one of two classes, chosen by the DpExtSend flags exactly as
 * DirectPlay chose them:
 *
 *   DPSEND_GUARANTEED  RELIABLE + ORDERED. Acked, retransmitted, fragmented to fit
 *                      a datagram, delivered in send order. Kills, damage, chat,
 *                      scores, game start/end - anything the game applies once.
 *
 *   0                  UNRELIABLE STATE. Usually one datagram, may be lost. Each is
 *                      superseded by the next tick's, so a late one is DROPPED
 *                      rather than applied: it is dropped if a newer state from the
 *                      same peer has been seen, or if a reliable message sent AFTER
 *                      it has already been delivered (so a stale "alive" state can
 *                      never undo a kill). pldnet.c builds these in its own buffer.
 *
 * The point of splitting them: under TCP one lost packet held up everything
 * behind it until it was retransmitted, so a single Wi-Fi drop froze every other
 * player. Now only the reliable channel waits, and state keeps flowing.
 *
 * Sockets are pumped on demand from DpExtRecv(), which the engine calls every
 * frame ([Minimal]NetCollectMessages), so no thread is needed.
 *
 * Internet play goes through a relay server (tools/relay), configured with
 * Net_SetRelayServer. The relay only forwards, so everything above runs end to end
 * through it unchanged. A peer reached through it gets a VIRTUAL address -
 * 0.0.0.0 with its relay tag in the port (the host is tag 0) - which no real
 * datagram can come from. Only net_sendto and net_read_all know about the relay:
 * they wrap and unwrap the envelope and translate between the two. A relayed host
 * still answers LAN discovery and direct joins, so one game can have both.
 *
 * Liveness: any packet counts as a sign of life; an idle link sends a PING once a
 * second. The timeout is long (NET_TIMEOUT_MS) because the game does not pump
 * during a level load, and a slow load must not read as a dropped player. A clean
 * exit sends BYE, so ordinary leaving is still noticed at once.
 * ========================================================================== */
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET NetSocket;
#define NET_BAD INVALID_SOCKET
#define net_closesock closesocket
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ifaddrs.h>
#include <net/if.h>
typedef int NetSocket;
#define NET_BAD (-1)
#define net_closesock close
#endif

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

#define NET_PORT        27015       /* UDP port the host listens on */
/* host: max simultaneous clients. The game has NET_MAXPLAYERS slots and the host
   takes one; a link past that would connect but never get a slot, so the host
   answers its HELLO with PK_FULL instead. */
#define NET_MAXCONNS    (NET_MAXPLAYERS - 1)
#define NET_HOST_DPID   1           /* host's fixed player id */
#define NET_FRAMEMAX    (16*1024)   /* max single (reliable) message payload */

/* Wire format, all little-endian:
 *   0 u32 magic   4 u8 kind   5 u8 flags   6 u16 version
 *   8 u32 seq    12 u32 aux  16 i32 from  20 i32 to   24 payload
 * seq/aux by kind:
 *   REL      seq = packet sequence number        flags = FIRST/LAST fragment
 *   UNREL    seq = state sequence number         aux   = sender's next REL seq at send time
 *            flags = fragment index | count << 4 (a state message spans at most 15)
 *   ACK      seq = next REL seq expected         aux   = bitmask: bit i => seq+1+i received
 *   HELLO    payload = player name
 *   WELCOME  seq = DPID assigned to the joiner
 *   FULL     host -> joiner: every player slot is taken */
#define NET_MAGIC       0x55505641  /* 'AVPU' */
#define NET_VERSION     1
#define NET_HDR         24
#define NET_MTU         1200        /* datagram ceiling: safely under any internet path MTU */
#define NET_SEG         (NET_MTU - NET_HDR)

enum { PK_REL = 1, PK_UNREL, PK_ACK, PK_HELLO, PK_WELCOME, PK_PING, PK_BYE, PK_FULL };
#define PKF_FIRST       1
#define PKF_LAST        2
/* UNREL flags: fragment index in the low nibble, fragment count in the high one.
   15 x NET_SEG is larger than NET_FRAMEMAX, so state never needs the reliable path. */
#define NET_UNREL_FRAGS 15

#define NET_WIN             1024    /* reliable packets in flight / held out of order, per link */
#define NET_RTO_MIN_MS      40
#define NET_RTO_MAX_MS      500     /* a game would rather resend than wait */
#define NET_RTO_INIT_MS     200
#define NET_PING_MS         1000    /* send a keepalive after this long with nothing sent */
#define NET_TIMEOUT_MS      30000   /* nothing heard for this long => peer gone (see header) */
#define NET_HELLO_EVERY_MS  250     /* joiner: resend HELLO until WELCOME */
#define NET_JOIN_TIMEOUT_MS 5000    /* joiner: give up if no WELCOME by then */
#define NET_RCVBUF          (1024*1024)
#define NET_PENDING_MAX     (8*1024*1024)  /* reliable bytes queued behind a full window before giving up */

/* LAN auto-discovery (UDP broadcast): clients broadcast a QUERY; hosts reply
   with their session name + player count. The host's IP is taken from the reply
   packet's source address, so no manual IP entry is needed. */
#define NET_DISC_PORT   27016
#define NET_DISC_MAGIC  0x41565044  /* 'AVPD' */
#define NETDISC_QUERY   1
#define NETDISC_REPLY   2
#define NET_DISC_HDR    16          /* magic(4)+op(4)+tcpPort(4)+playerCount(4), name follows */

typedef enum { NET_OFF = 0, NET_HOST, NET_CLIENT } NetRole;

struct NetMsg;

typedef struct {
	unsigned char *pkt;         /* NULL = slot free (acked) */
	int            len;
	uint32_t       sent_ms;
	uint32_t       rto_ms;      /* this packet's current retransmit timeout (backs off) */
	int            resent;      /* retransmitted at least once: no RTT sample (Karn) */
} NetRelOut;

typedef struct {
	struct sockaddr_in addr;
	DPID           dpid;        /* remote player's id */

	/* reliable, sending */
	uint32_t       rel_next;    /* seq the next packet gets */
	uint32_t       rel_una;     /* oldest unacknowledged seq */
	NetRelOut      out[NET_WIN];
	uint32_t       srtt_ms;     /* smoothed round trip; 0 = no sample yet */
	/* Whole messages waiting for room in the window. A peer that is loading a level
	   does not acknowledge, and the host keeps sending (the lobby sends its game
	   description every frame), so the window can fill without anything being wrong.
	   They wait here, as TCP's socket buffer used to hold them. */
	struct NetMsg *pend_head, *pend_tail;
	int            pend_bytes;
	int            pend_frags;  /* packets those will take: state is stamped as sent after them */

	/* reliable, receiving */
	uint32_t       rel_expect;  /* next in-order seq to deliver */
	unsigned char *held[NET_WIN];   /* out-of-order packets waiting for the gap to fill */
	int            held_len[NET_WIN];
	unsigned char  asm_buf[NET_FRAMEMAX];  /* fragment reassembly */
	int            asm_len, asm_on;
	int            ack_due;

	/* unreliable state */
	uint32_t       unrel_next;
	uint32_t       unrel_last;
	int            unrel_seen;
	/* reassembly of ONE fragmented state message; a newer one discards it */
	uint32_t       ua_seq;
	int            ua_on, ua_count, ua_len;
	unsigned       ua_mask;
	unsigned char  ua_buf[NET_UNREL_FRAGS * NET_SEG];

	uint32_t       last_rx_ms, last_tx_ms;
} NetConn;

static NetRole   net_role     = NET_OFF;
static NetSocket net_sock     = NET_BAD;             /* the game socket (host: bound to NET_PORT) */
static NetConn  *net_conns[NET_MAXCONNS];            /* host: clients; client: [0]=host link */
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
static uint32_t     net_join_start = 0;
static uint32_t     net_join_hello_ms = 0;

/* internet relay */
/* "host[:port]"; blank = LAN only. The frontend's Relay Server field edits this
   buffer directly, and config.cfg's #RELAY line seeds it (main.c). */
char             NetRelayServer[RELAY_SERVER_LEN] = RELAY_DEFAULT_SERVER;
static int       net_relay_heard = 0;                /* anything at all came back from the relay */
static int       net_relay_on    = 0;                /* this session uses the relay */
static struct sockaddr_in net_relay_addr;
static unsigned  net_relay_code  = 0;                /* host: room code once open; client: code joined */
static int       net_relay_open  = 0;                /* host: RELAY_ROOM received */
static int       net_relay_err   = 0;                /* last RELAY_ERR_*, or a NET_RELAY_ERR_* below */
static uint32_t  net_relay_tx_ms = 0;
static int       net_relay_asks  = 0;                /* host: RELAY_HOST sent without an answer */
static unsigned  net_disc_code[MAX_NO_OF_SESSIONS];  /* browse entry is a relay room (0 = LAN host) */
#define NET_RELAY_ERR_NOANSWER  (-1)                 /* the relay never replied */
#define NET_RELAY_ERR_NOHOST    (-2)                 /* the relay replied, the host did not */

/* Is a relay set at all? A field of spaces counts as blank. */
static int net_relay_configured(void) {
	const char *p;
	for (p = NetRelayServer; *p; p++) if (*p != ' ') return 1;
	return 0;
}

static void net_wr32(unsigned char *p, int32_t v) {
	p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8);
	p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24);
}
static int32_t net_rd32(const unsigned char *p) {
	return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24));
}
/* Sequence comparison that survives wrap-around: a is after b. */
static int net_seq_after(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

static void net_setnonblock(NetSocket s) {
#ifdef _WIN32
	u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
#else
	int fl = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

static int net_same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b) {
	return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

/* A peer behind the relay (see the header comment). */
static int net_is_relayed(const struct sockaddr_in *a) { return a->sin_addr.s_addr == 0; }
static struct sockaddr_in net_relay_peer(uint32_t tag) {
	struct sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port   = htons((unsigned short)tag);
	return a;
}

/* ---------------------------------------------------------------------------
 * Received-message queue and the DirectPlay system messages the engine expects
 * ------------------------------------------------------------------------- */

static void net_enqueue(DPID from, DPID to, const unsigned char *data, int size) {
	NetMsg *m = (NetMsg *)malloc(sizeof(NetMsg) + (size > 0 ? (size_t)size : 0));
	if (!m) return;
	m->next = NULL; m->from = from; m->to = to; m->size = size;
	if (size) memcpy(m->data, data, (size_t)size);
	if (net_qtail) net_qtail->next = m; else net_qhead = m;
	net_qtail = m;
}

/* These structs carry a POINTER to the player name (DPNAME.lpszShortNameA);
   ProcessSystemMessage dereferences it immediately after DpExtRecv returns, so we
   point it at a small ring of persistent name buffers. */
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

/* ---------------------------------------------------------------------------
 * Datagrams
 * ------------------------------------------------------------------------- */

static void net_hdr(unsigned char *p, int kind, int flags, uint32_t seq, uint32_t aux, DPID from, DPID to) {
	net_wr32(p, NET_MAGIC);
	p[4] = (unsigned char)kind;
	p[5] = (unsigned char)flags;
	p[6] = (unsigned char)NET_VERSION; p[7] = 0;
	net_wr32(p + 8,  (int32_t)seq);
	net_wr32(p + 12, (int32_t)aux);
	net_wr32(p + 16, from);
	net_wr32(p + 20, to);
}

static void net_relay_send(int op, uint32_t val, const unsigned char *payload, int plen) {
	unsigned char buf[RELAY_HDR + NET_MTU];
	if (net_sock == NET_BAD || plen > NET_MTU) return;
	net_wr32(buf, (int32_t)RELAY_MAGIC);
	buf[4] = (unsigned char)op;
	buf[5] = RELAY_VERSION;
	buf[6] = buf[7] = 0;
	net_wr32(buf + 8, (int32_t)val);
	if (plen > 0) memcpy(buf + RELAY_HDR, payload, (size_t)plen);
	sendto(net_sock, (const char *)buf, RELAY_HDR + plen, 0,
	       (const struct sockaddr *)&net_relay_addr, sizeof net_relay_addr);
	net_relay_tx_ms = net_now_ms();
}

static void net_sendto(const struct sockaddr_in *addr, const unsigned char *pkt, int len) {
	if (net_sock == NET_BAD) return;
	if (net_is_relayed(addr)) {
		/* host: addressed by the client's tag; client: by the room it is in */
		if (net_relay_on)
			net_relay_send(RELAY_DATA, net_role == NET_HOST ? ntohs(addr->sin_port) : net_relay_code, pkt, len);
		return;
	}
	sendto(net_sock, (const char *)pkt, len, 0, (const struct sockaddr *)addr, sizeof *addr);
}

static void net_send_ctrl(NetConn *c, int kind, uint32_t seq, uint32_t aux,
                          const void *payload, int plen) {
	unsigned char pkt[NET_MTU];
	if (plen > NET_SEG) plen = NET_SEG;
	net_hdr(pkt, kind, 0, seq, aux, 0, 0);
	if (plen > 0) memcpy(pkt + NET_HDR, payload, (size_t)plen);
	net_sendto(&c->addr, pkt, NET_HDR + plen);
	c->last_tx_ms = net_now_ms();
}

static NetConn *net_conn_new(const struct sockaddr_in *addr, DPID dpid) {
	NetConn *c = (NetConn *)calloc(1, sizeof(NetConn));
	if (!c) return NULL;
	c->addr = *addr;
	c->dpid = dpid;
	c->last_rx_ms = c->last_tx_ms = net_now_ms();
	return c;
}

static void net_conn_free(NetConn *c) {
	int i;
	for (i = 0; i < NET_WIN; i++) { free(c->out[i].pkt); free(c->held[i]); }
	while (c->pend_head) { struct NetMsg *m = c->pend_head; c->pend_head = m->next; free(m); }
	free(c);
}

/* Drop a link. 'notify' raises the matching system message for the game. */
static void net_conn_close(int i, int notify) {
	NetConn *c = net_conns[i];
	DPID id = c->dpid;
	net_conn_free(c);
	net_conns[i] = net_conns[net_nconns - 1];   /* compact */
	net_conns[net_nconns - 1] = NULL;
	net_nconns--;
	if (!notify) return;
	if (net_role == NET_HOST)        net_enqueue_destroy(id);       /* host: player left */
	else if (net_role == NET_CLIENT) net_enqueue_sessionlost();     /* client: host lost */
}

/* Put one reliable message on the wire: split into fragments, each sent now and
   kept until acknowledged. The caller has checked the window has room. */
static void net_transmit_rel(NetConn *c, DPID from, DPID to, const unsigned char *data, int size) {
	int nfrag = size > 0 ? (size + NET_SEG - 1) / NET_SEG : 1;
	int f, off = 0;
	uint32_t now = net_now_ms();
	for (f = 0; f < nfrag; f++) {
		int len = size - off;
		NetRelOut *o = &c->out[c->rel_next % NET_WIN];
		if (len > NET_SEG) len = NET_SEG;
		free(o->pkt);
		o->pkt = (unsigned char *)malloc((size_t)(NET_HDR + len));
		if (!o->pkt) return;
		net_hdr(o->pkt, PK_REL, (f == 0 ? PKF_FIRST : 0) | (f == nfrag - 1 ? PKF_LAST : 0),
		        c->rel_next, 0, from, to);
		if (len > 0) memcpy(o->pkt + NET_HDR, data + off, (size_t)len);
		o->len     = NET_HDR + len;
		o->sent_ms = now;
		o->rto_ms  = c->srtt_ms ? c->srtt_ms * 2 + 20 : NET_RTO_INIT_MS;
		if (o->rto_ms < NET_RTO_MIN_MS) o->rto_ms = NET_RTO_MIN_MS;
		if (o->rto_ms > NET_RTO_MAX_MS) o->rto_ms = NET_RTO_MAX_MS;
		o->resent  = 0;
		net_sendto(&c->addr, o->pkt, o->len);
		c->rel_next++;
		off += len;
	}
	c->last_tx_ms = now;
}

static int net_frags(int size) { return size > 0 ? (size + NET_SEG - 1) / NET_SEG : 1; }

static int net_rel_fits(const NetConn *c, int size) {
	return (int)(c->rel_next - c->rel_una) + net_frags(size) <= NET_WIN;
}

/* Send what the window now has room for, oldest first. */
static void net_flush_pending(NetConn *c) {
	while (c->pend_head && net_rel_fits(c, c->pend_head->size)) {
		struct NetMsg *m = c->pend_head;
		c->pend_head = m->next;
		if (!c->pend_head) c->pend_tail = NULL;
		c->pend_bytes -= m->size;
		c->pend_frags -= net_frags(m->size);
		net_transmit_rel(c, m->from, m->to, m->data, m->size);
		free(m);
	}
}

/* Queue one reliable message on a link. Goes out at once if the window has room
   and nothing is already waiting (order must be kept), else waits in the pending
   queue. Returns -1 only if that queue has grown past NET_PENDING_MAX. */
static int net_send_rel(NetConn *c, DPID from, DPID to, const unsigned char *data, int size) {
	struct NetMsg *m;
	if (!c->pend_head && net_rel_fits(c, size)) {
		net_transmit_rel(c, from, to, data, size);
		return 0;
	}
	if (c->pend_bytes + size > NET_PENDING_MAX) return -1;
	m = (struct NetMsg *)malloc(sizeof(struct NetMsg) + (size > 0 ? (size_t)size : 0));
	if (!m) return -1;
	m->next = NULL; m->from = from; m->to = to; m->size = size;
	if (size) memcpy(m->data, data, (size_t)size);
	if (c->pend_tail) c->pend_tail->next = m; else c->pend_head = m;
	c->pend_tail = m;
	c->pend_bytes += size;
	c->pend_frags += net_frags(size);
	return 0;
}

/* One state message: one datagram, or up to NET_UNREL_FRAGS when a crowd of aliens
 * makes it large. It never falls back to the reliable channel - state delivered
 * that way could land after a NEWER state sent directly, and be applied over it.
 *
 * The stamp counts reliable messages still waiting in the pending queue as already
 * sent, because to the game they were: they were handed over before this state. */
static int net_send_unrel(NetConn *c, DPID from, DPID to, const unsigned char *data, int size) {
	unsigned char pkt[NET_MTU];
	int nfrag = net_frags(size), f, off = 0;
	uint32_t seq = c->unrel_next++;
	uint32_t mark = c->rel_next + (uint32_t)c->pend_frags;
	if (nfrag > NET_UNREL_FRAGS) return -1;
	for (f = 0; f < nfrag; f++) {
		int len = size - off;
		if (len > NET_SEG) len = NET_SEG;
		net_hdr(pkt, PK_UNREL, f | (nfrag << 4), seq, mark, from, to);
		if (len > 0) memcpy(pkt + NET_HDR, data + off, (size_t)len);
		net_sendto(&c->addr, pkt, NET_HDR + len);
		off += len;
	}
	c->last_tx_ms = net_now_ms();
	return 0;
}

/* Route one message out of this machine. 'except' is the link it arrived on when
   the host is relaying (it is not sent back there), or NULL. A POINTER, not an
   index: a failing link is closed here and the array compacted, and 'except'
   itself is never closed, so the pointer stays valid where an index would not. */
static void net_route(const NetConn *except, DPID from, DPID to, int reliable,
                      const unsigned char *data, int size) {
	int j;
	for (j = net_nconns - 1; j >= 0; j--) {
		NetConn *c = net_conns[j];
		if (c == except) continue;
		if (net_role == NET_HOST && !(to == DPID_ALLPLAYERS || to == c->dpid)) continue;
		if (reliable) {
			if (net_send_rel(c, from, to, data, size) != 0) {
				fprintf(stderr, "net: DPID %d has %d bytes waiting unacknowledged; dropping the link\n",
				        (int)c->dpid, c->pend_bytes);
				net_conn_close(j, 1);
			}
		} else {
			net_send_unrel(c, from, to, data, size);
		}
	}
}

/* A whole message arrived on link ci: give it to the game, and relay it if we host. */
static void net_deliver(const NetConn *c, DPID from, DPID to, int reliable,
                        const unsigned char *data, int size) {
	net_enqueue(from, to, data, size);
	if (net_role == NET_HOST) net_route(c, from, to, reliable, data, size);
}

/* ---------------------------------------------------------------------------
 * Receiving
 * ------------------------------------------------------------------------- */

/* Apply one in-order reliable packet: append to the reassembly buffer, deliver on
   the last fragment. */
static void net_rel_apply(NetConn *c, const unsigned char *pkt, int len) {
	int flags = pkt[5];
	int plen  = len - NET_HDR;
	if (flags & PKF_FIRST) { c->asm_len = 0; c->asm_on = 1; }
	if (!c->asm_on) return;                           /* tail of a message we never started */
	if (c->asm_len + plen > NET_FRAMEMAX) { c->asm_on = 0; return; }
	memcpy(c->asm_buf + c->asm_len, pkt + NET_HDR, (size_t)plen);
	c->asm_len += plen;
	if (flags & PKF_LAST) {
		c->asm_on = 0;
		net_deliver(c, net_rd32(pkt + 16), net_rd32(pkt + 20), 1, c->asm_buf, c->asm_len);
	}
}

static void net_on_rel(NetConn *c, const unsigned char *pkt, int len) {
	uint32_t seq = (uint32_t)net_rd32(pkt + 8);
	c->ack_due = 1;                                   /* ack even duplicates: our ack may have been lost */
	if (!net_seq_after(seq + 1, c->rel_expect)) return;           /* already delivered */
	if ((uint32_t)(seq - c->rel_expect) >= NET_WIN) return;      /* too far ahead: it will be resent */
	if (seq != c->rel_expect) {                       /* out of order: hold it */
		int k = (int)(seq % NET_WIN);
		if (!c->held[k]) {
			c->held[k] = (unsigned char *)malloc((size_t)len);
			if (!c->held[k]) return;
			memcpy(c->held[k], pkt, (size_t)len);
			c->held_len[k] = len;
		}
		return;
	}
	/* Advance BEFORE applying: delivery can relay, and a state packet relayed or
	   checked meanwhile must see this one as delivered. */
	c->rel_expect++;
	net_rel_apply(c, pkt, len);
	for (;;) {                                        /* drain anything the gap was holding back */
		int k = (int)(c->rel_expect % NET_WIN);
		unsigned char *h = c->held[k];
		if (!h || (uint32_t)net_rd32(h + 8) != c->rel_expect) break;
		c->held[k] = NULL;
		c->rel_expect++;
		net_rel_apply(c, h, c->held_len[k]);
		free(h);
	}
}

static void net_on_unrel(NetConn *c, const unsigned char *pkt, int len) {
	uint32_t seq     = (uint32_t)net_rd32(pkt + 8);
	uint32_t relmark = (uint32_t)net_rd32(pkt + 12);
	int idx  = pkt[5] & 15, cnt = pkt[5] >> 4;
	int plen = len - NET_HDR;
	const unsigned char *data = pkt + NET_HDR;
	int size = plen;

	if (cnt == 0) cnt = 1;
	if (idx >= cnt || (idx < cnt - 1 && plen != NET_SEG)) return;     /* malformed */
	if (c->unrel_seen && !net_seq_after(seq, c->unrel_last)) return;   /* older than one we used */

	if (cnt > 1) {
		if (c->ua_on && seq != c->ua_seq) {
			if (!net_seq_after(seq, c->ua_seq)) return;   /* a fragment of an older one */
			c->ua_on = 0;                                 /* newer: abandon the old one */
		}
		if (!c->ua_on) {
			c->ua_on = 1; c->ua_seq = seq; c->ua_count = cnt; c->ua_mask = 0; c->ua_len = 0;
		}
		if (cnt != c->ua_count) return;
		memcpy(c->ua_buf + idx * NET_SEG, data, (size_t)plen);
		c->ua_mask |= 1u << idx;
		if (idx == cnt - 1) c->ua_len = idx * NET_SEG + plen;
		if (c->ua_mask != (1u << cnt) - 1) return;        /* still waiting for fragments */
		c->ua_on = 0;
		data = c->ua_buf;
		size = c->ua_len;
	}

	if (net_seq_after(c->rel_expect, relmark)) return;  /* an event sent after it is already applied */
	c->unrel_seen = 1;
	c->unrel_last = seq;
	net_deliver(c, net_rd32(pkt + 16), net_rd32(pkt + 20), 0, data, size);
}

static void net_on_ack(NetConn *c, const unsigned char *pkt) {
	uint32_t ack  = (uint32_t)net_rd32(pkt + 8);
	uint32_t bits = (uint32_t)net_rd32(pkt + 12);
	uint32_t now  = net_now_ms();
	int i;
	if (net_seq_after(ack, c->rel_next)) return;      /* acks something never sent: ignore */
	while (net_seq_after(ack, c->rel_una)) {          /* cumulative */
		NetRelOut *o = &c->out[c->rel_una % NET_WIN];
		if (o->pkt) {
			if (!o->resent) {                         /* RTT sample (Karn: never from a resend) */
				uint32_t s = now - o->sent_ms;
				c->srtt_ms = c->srtt_ms ? (c->srtt_ms * 7 + s) / 8 : s;
			}
			free(o->pkt); o->pkt = NULL;
		}
		c->rel_una++;
	}
	for (i = 0; i < 32; i++) {                        /* selective: received past a gap */
		uint32_t s = ack + 1 + (uint32_t)i;
		if (!(bits & (1u << i))) continue;
		if (!net_seq_after(c->rel_next, s)) break;
		{
			NetRelOut *o = &c->out[s % NET_WIN];
			if (o->pkt) { free(o->pkt); o->pkt = NULL; }
		}
	}
}

static int net_find_conn(const struct sockaddr_in *from) {
	int i;
	for (i = 0; i < net_nconns; i++)
		if (net_same_addr(&net_conns[i]->addr, from)) return i;
	return -1;
}

/* Host: a joiner's HELLO. A repeat (our WELCOME was lost) just gets the WELCOME again. */
static void net_on_hello(const struct sockaddr_in *from, const unsigned char *pkt, int len) {
	int ci = net_find_conn(from);
	NetConn *c;
	if (ci < 0) {
		char nm[NET_PLAYERNAMELENGTH];
		int nlen = len - NET_HDR;
		DPID id;
		if (net_nconns >= NET_MAXCONNS) {
			/* Say so, rather than leave the joiner to time out. Sent per HELLO,
			   so a lost one is simply answered again. */
			unsigned char full[NET_HDR];
			net_hdr(full, PK_FULL, 0, 0, 0, 0, 0);
			net_sendto(from, full, NET_HDR);
			return;
		}
		id = net_next_dpid++;
		if (id == DPID_SYSMSG || id == DPID_ALLPLAYERS) id = net_next_dpid++;   /* skip reserved */
		c = net_conn_new(from, id);
		if (!c) return;
		net_conns[net_nconns++] = c;
		if (nlen < 0) nlen = 0;
		if (nlen > NET_PLAYERNAMELENGTH - 1) nlen = NET_PLAYERNAMELENGTH - 1;
		memcpy(nm, pkt + NET_HDR, (size_t)nlen);
		nm[nlen] = '\0';
		if (net_is_relayed(from))
			fprintf(stderr, "net: client relay tag %d joined, assigned DPID %d (%d players)\n",
				ntohs(from->sin_port), (int)id, net_nconns + 1);
		else
			fprintf(stderr, "net: client %s:%d joined, assigned DPID %d (%d players)\n",
				inet_ntoa(from->sin_addr), ntohs(from->sin_port), (int)id, net_nconns + 1);
		net_enqueue_create(id, nm);
	} else {
		c = net_conns[ci];
	}
	net_send_ctrl(c, PK_WELCOME, (uint32_t)c->dpid, 0, NULL, 0);
}

static const char *net_relay_errstr(int e) {
	switch (e) {
	case RELAY_ERR_NOROOM:  return "no game with that code";
	case RELAY_ERR_FULL:    return "that game is full";
	case RELAY_ERR_BUSY:    return "the relay has no free rooms";
	case RELAY_ERR_VERSION: return "the relay runs a different version";
	case NET_RELAY_ERR_NOANSWER: return "no answer from the relay server";
	case NET_RELAY_ERR_NOHOST:   return "the host did not answer";
	default:                return "unknown relay error";
	}
}

/* A datagram from the relay. Returns 1 if it carried a game packet, which has been
   unwrapped in place: *n shrinks and *from becomes the sender's virtual address. */
static int net_on_relay(unsigned char *pkt, int *n, struct sockaddr_in *from) {
	uint32_t val;
	if (*n < RELAY_HDR || (uint32_t)net_rd32(pkt) != RELAY_MAGIC) return 0;
	net_relay_heard = 1;
	val = (uint32_t)net_rd32(pkt + 8);
	switch (pkt[4]) {
	case RELAY_DATA:
		*n -= RELAY_HDR;
		memmove(pkt, pkt + RELAY_HDR, (size_t)*n);
		*from = net_relay_peer(net_role == NET_HOST ? val : 0);
		return 1;
	case RELAY_ROOM:
		if (net_role == NET_HOST) {
			char code[RELAY_CODE_LEN + 1];
			if (!net_relay_open || net_relay_code != val) {
				relay_code_to_str(val, code);
				fprintf(stderr, "net: internet game open, code %s\n", code);
			}
			net_relay_code = val;
			net_relay_open = 1;
			net_relay_err  = 0;
		}
		return 0;
	case RELAY_ERROR:
		if (net_relay_err != (int)val)
			fprintf(stderr, "net: relay: %s\n", net_relay_errstr((int)val));
		net_relay_err = (int)val;
		/* A joiner fails at once instead of waiting out the timeout. Once joined, a
		   NOROOM means the relay restarted. That game is lost - the relay would give
		   this client a new tag, which the host treats as a stranger - but the link's
		   own timeout reports it, so it is not acted on here. */
		if (net_role == NET_CLIENT && net_join_state == NET_JS_HANDSHAKE)
			net_join_state = NET_JS_FAILED;
		return 0;
	default:
		return 0;
	}
}

/* Read every waiting datagram. */
static void net_read_all(void) {
	unsigned char pkt[NET_MTU + 64];
	for (;;) {
		struct sockaddr_in from;
		socklen_t fl = sizeof from;
		int n = (int)recvfrom(net_sock, (char *)pkt, sizeof pkt, 0, (struct sockaddr *)&from, &fl);
		int kind, ci;
		if (n < 0) break;                             /* would block, or an ICMP error: done */
		if (net_relay_on && net_same_addr(&from, &net_relay_addr) && !net_on_relay(pkt, &n, &from)) continue;
		if (n < NET_HDR || net_rd32(pkt) != (int32_t)NET_MAGIC || pkt[6] != NET_VERSION) continue;
		kind = pkt[4];

		if (kind == PK_HELLO) { if (net_role == NET_HOST) net_on_hello(&from, pkt, n); continue; }

		ci = net_find_conn(&from);
		if (ci < 0) continue;                         /* not one of ours */
		net_conns[ci]->last_rx_ms = net_now_ms();

		switch (kind) {
		case PK_REL:     net_on_rel(net_conns[ci], pkt, n);    break;
		case PK_UNREL:   net_on_unrel(net_conns[ci], pkt, n);  break;
		case PK_ACK:     net_on_ack(net_conns[ci], pkt);   break;
		case PK_WELCOME:
			if (net_role == NET_CLIENT && AVPDPNetID == 0) {
				AVPDPNetID = net_rd32(pkt + 8);
				fprintf(stderr, "net: assigned DPID %d by host\n", (int)AVPDPNetID);
			}
			break;
		case PK_BYE:
			fprintf(stderr, "net: DPID %d said goodbye\n", (int)net_conns[ci]->dpid);
			net_conn_close(ci, 1);
			break;
		case PK_FULL:
			/* Reported through the relay-error slot, which is what the Joining
			   screen shows - the reason reads the same whichever way the join
			   went (LAN, IP or code). */
			if (net_role == NET_CLIENT && net_join_state == NET_JS_HANDSHAKE) {
				fprintf(stderr, "net: the game is full\n");
				net_relay_err  = RELAY_ERR_FULL;
				net_join_state = NET_JS_FAILED;
			}
			break;
		default: break;                               /* PING: last_rx_ms was all it needed */
		}
	}
}

/* Per link, per pump: ack what arrived, resend what timed out, keep an idle link
   alive, and give up on a silent one. */
static void net_service_links(void) {
	uint32_t now = net_now_ms();
	int i;
	for (i = net_nconns - 1; i >= 0; i--) {
		NetConn *c = net_conns[i];
		uint32_t s;

		if (now - c->last_rx_ms > NET_TIMEOUT_MS) {
			fprintf(stderr, "net: DPID %d timed out\n", (int)c->dpid);
			net_conn_close(i, 1);
			continue;
		}
		if (c->ack_due) {
			uint32_t bits = 0;
			int k;
			for (k = 0; k < 32; k++) {
				uint32_t q = c->rel_expect + 1 + (uint32_t)k;
				unsigned char *h = c->held[q % NET_WIN];
				if (h && (uint32_t)net_rd32(h + 8) == q) bits |= 1u << k;
			}
			net_send_ctrl(c, PK_ACK, c->rel_expect, bits, NULL, 0);
			c->ack_due = 0;
		}
		for (s = c->rel_una; net_seq_after(c->rel_next, s); s++) {
			NetRelOut *o = &c->out[s % NET_WIN];
			if (!o->pkt || now - o->sent_ms < o->rto_ms) continue;
			net_sendto(&c->addr, o->pkt, o->len);
			o->sent_ms = now;
			o->resent  = 1;
			o->rto_ms *= 2;
			if (o->rto_ms > NET_RTO_MAX_MS) o->rto_ms = NET_RTO_MAX_MS;
			c->last_tx_ms = now;
		}
		net_flush_pending(c);
		if (now - c->last_tx_ms > NET_PING_MS) net_send_ctrl(c, PK_PING, 0, 0, NULL, 0);
	}
}

static void net_udp_respond(void);   /* forward decl (defined below) */

/* Host: open the room, and keep it open. Resent quickly until the relay answers,
   then as a slow keepalive - which also gets the room back, under the same code, if
   the relay has restarted.
 *
 * "No answer" is counted in unanswered ASKS, not elapsed time. The first ask goes
 * out from the menu and then the level loads, which can take longer than the whole
 * timeout without the game reading the socket once - a clock would call that
 * silence while the answer sat unread in the receive buffer. Asks are only sent
 * while the game is pumping, so they measure time actually spent listening. */
static void net_relay_host_pump(void) {
	uint32_t now = net_now_ms();
	if (!net_relay_on || net_role != NET_HOST) return;
	if (!net_relay_open) {
		if (net_relay_err > 0) return;                         /* refused: stop asking */
		if (net_relay_err == 0 && net_relay_asks >= RELAY_HOST_TIMEOUT_MS / RELAY_HOST_RESEND_MS) {
			fprintf(stderr, "net: no answer from relay %s\n", NetRelayServer);
			net_relay_err = NET_RELAY_ERR_NOANSWER;            /* reported, but keep trying */
		}
	}
	if (now - net_relay_tx_ms >= (net_relay_open ? RELAY_HOST_REFRESH_MS : RELAY_HOST_RESEND_MS)) {
		net_relay_send(RELAY_HOST, net_relay_code, NULL, 0);
		if (!net_relay_open) net_relay_asks++;
	}
}

/* Read the socket and service every link. Cheap to call often. Reading comes
   first, so anything that arrived during a load is seen before any timeout is
   judged. */
static void net_pump(void) {
	if (net_role == NET_OFF || net_sock == NET_BAD) return;
	if (net_role == NET_HOST) net_udp_respond();
	net_read_all();
	if (net_role == NET_HOST) net_relay_host_pump();
	net_service_links();
}

static void net_teardown(void) {
	int i;
	/* Say goodbye so peers notice at once rather than after NET_TIMEOUT_MS. Twice,
	   since it is not acknowledged and one may be lost. */
	for (i = 0; i < net_nconns; i++) {
		net_send_ctrl(net_conns[i], PK_BYE, 0, 0, NULL, 0);
		net_send_ctrl(net_conns[i], PK_BYE, 0, 0, NULL, 0);
		net_conn_free(net_conns[i]);
		net_conns[i] = NULL;
	}
	net_nconns = 0;
	if (net_relay_on) {                           /* free the room / our slot in it now */
		int op = net_role == NET_HOST ? RELAY_CLOSE : RELAY_LEAVE;
		net_relay_send(op, 0, NULL, 0);
		net_relay_send(op, 0, NULL, 0);
	}
	net_relay_on = 0; net_relay_open = 0; net_relay_code = 0;   /* err kept: the menu reports it */
	if (net_sock != NET_BAD) { net_closesock(net_sock); net_sock = NET_BAD; }
	if (net_udp != NET_BAD)  { net_closesock(net_udp);  net_udp  = NET_BAD; }
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

/* The game socket. port 0 = any (a joiner). */
static NetSocket net_open_game_socket(unsigned short port) {
	struct sockaddr_in a;
	int rcv = NET_RCVBUF;
	NetSocket s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s == NET_BAD) return NET_BAD;
	/* Large receive buffer: the game does not pump during a level load, and what
	   arrives meanwhile has to wait in the kernel. */
	setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&rcv, sizeof rcv);
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(port);
	if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) { net_closesock(s); return NET_BAD; }
	net_setnonblock(s);
#ifdef _WIN32
	{
		/* Windows reports an ICMP port-unreachable from an EARLIER sendto as a
		   WSAECONNRESET on the next recvfrom, which would end the read loop every
		   pump while a peer is gone. Switch that off. */
		#ifndef SIO_UDP_CONNRESET
		#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
		#endif
		BOOL off = FALSE; DWORD nb = 0;
		WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &nb, NULL, NULL);
	}
#endif
	return s;
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

/* Resolve NetRelayServer into net_relay_addr. Blocking DNS, but it is called
   once, from a menu, when hosting or joining starts. */
static int net_relay_resolve(void) {
	char host[128];
	char *colon;
	int port = RELAY_PORT;
	struct addrinfo hints, *res = NULL;
	const char *src = NetRelayServer;
	size_t len;
	if (!net_relay_configured()) return 0;
	while (*src == ' ') src++;                     /* typed text: trim it */
	strncpy(host, src, sizeof host - 1);
	host[sizeof host - 1] = '\0';
	len = strlen(host);
	while (len && host[len - 1] == ' ') host[--len] = '\0';
	colon = strrchr(host, ':');
	if (colon) { port = atoi(colon + 1); *colon = '\0'; }
	if (port <= 0 || port > 65535) port = RELAY_PORT;
	memset(&hints, 0, sizeof hints);
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
		fprintf(stderr, "net: cannot resolve relay '%s'\n", host);
		return 0;
	}
	memcpy(&net_relay_addr, res->ai_addr, sizeof net_relay_addr);
	net_relay_addr.sin_port = htons((unsigned short)port);
	freeaddrinfo(res);
	return 1;
}

static int net_start_host(void) {
	net_teardown();
	net_winsock_init();
	net_sock = net_open_game_socket(NET_PORT);
	/* Internet hosting does not need the fixed port, so a second copy of the game
	   on this machine (or anything else on 27015) does not stop it. */
	if (net_sock == NET_BAD && net_relay_configured()) net_sock = net_open_game_socket(0);
	if (net_sock == NET_BAD) return 0;
	net_relay_err = 0;
	net_relay_heard = 0;
	if (net_relay_resolve()) {
		net_relay_on = 1;
		net_relay_asks = 1;
		net_relay_send(RELAY_HOST, 0, NULL, 0);
	}
	net_role = NET_HOST;
	net_next_dpid = NET_HOST_DPID + 1;
	AVPDPNetID = NET_HOST_DPID;
	glpDP = 1;
	net_udp_open_host();                      /* answer LAN discovery */
	fprintf(stderr, "net: hosting on UDP %d (host DPID %d), discovery on %d\n",
		NET_PORT, NET_HOST_DPID, NET_DISC_PORT);
	return 1;
}

static void net_send_hello(void) {
	extern char MP_PlayerName[];
	int nlen = (int)strlen(MP_PlayerName);
	if (nlen > NET_PLAYERNAMELENGTH - 1) nlen = NET_PLAYERNAMELENGTH - 1;
	net_send_ctrl(net_conns[0], PK_HELLO, 0, 0, MP_PlayerName, nlen);
	net_join_hello_ms = net_now_ms();
}

/* Start joining a host. Never blocks: HELLO is resent by net_join_pump() each
   frame until the WELCOME arrives, so the game loop keeps running throughout. */
static void net_begin_connect(const struct sockaddr_in *addr, unsigned room) {
	NetConn *c;
	struct sockaddr_in target = *addr;
	net_teardown();
	net_winsock_init();
	net_join_state = NET_JS_FAILED;
	net_sock = net_open_game_socket(0);
	if (net_sock == NET_BAD) return;
	net_relay_err = 0;
	net_relay_heard = 0;
	if (room) {                                    /* an internet game: the host is relay tag 0 */
		if (!net_relay_resolve()) { net_closesock(net_sock); net_sock = NET_BAD; return; }
		net_relay_on   = 1;
		net_relay_code = room;
		target = net_relay_peer(0);
	}
	c = net_conn_new(&target, NET_HOST_DPID);
	if (!c) { net_closesock(net_sock); net_sock = NET_BAD; return; }
	net_conns[0] = c;
	net_nconns = 1;
	net_role = NET_CLIENT;
	glpDP = 1;
	AVPDPNetID = 0;
	net_send_hello();
	net_join_start = net_now_ms();
	net_join_state = NET_JS_HANDSHAKE;
	if (room) {
		char code[RELAY_CODE_LEN + 1];
		relay_code_to_str(room, code);
		fprintf(stderr, "net: joining game %s via relay %s...\n", code, NetRelayServer);
	} else {
		fprintf(stderr, "net: joining %s:%d...\n", inet_ntoa(addr->sin_addr), ntohs(addr->sin_port));
	}
}

/* Advance the join one step (no blocking). Returns the current state. */
static int net_join_pump(void) {
	if (net_join_state != NET_JS_HANDSHAKE) return (int)net_join_state;
	net_pump();                                    /* reads WELCOME -> AVPDPNetID */
	if (AVPDPNetID != 0) {
		net_join_state = NET_JS_DONE;
	} else if (net_nconns < 1 || net_now_ms() - net_join_start > NET_JOIN_TIMEOUT_MS) {
		fprintf(stderr, "net: no answer from host\n");
		/* Through the relay, say which end was silent: a wrong relay address and a
		   host stuck loading look the same otherwise. */
		if (net_relay_on && net_relay_err == 0)
			net_relay_err = net_relay_heard ? NET_RELAY_ERR_NOHOST : NET_RELAY_ERR_NOANSWER;
		net_join_state = NET_JS_FAILED;
	} else if (net_now_ms() - net_join_hello_ms > NET_HELLO_EVERY_MS) {
		net_send_hello();                          /* HELLO or WELCOME may have been lost */
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
		unsigned room = net_relay_configured() ? relay_code_from_str(IPAddressString) : 0;
		if (room) {
			/* The address field holds a room code: offer the internet game. */
			char code[RELAY_CODE_LEN + 1];
			relay_code_to_str(room, code);
			net_disc_addr[0] = net_relay_peer(0);
			net_disc_code[0] = room;
			snprintf(net_disc_names[0], sizeof net_disc_names[0], "Internet game %s", code);
			net_disc_count = 1;
		} else if (IPAddressString[0] && inet_pton(AF_INET, IPAddressString, &a.sin_addr) == 1) {
			net_disc_addr[0] = a;
			net_disc_code[0] = 0;
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
			if (!net_disc_code[i] && net_disc_addr[i].sin_addr.s_addr == from.sin_addr.s_addr) { slot = i; break; }
		if (slot < 0 && net_disc_count < MAX_NO_OF_SESSIONS) {
			int port = net_rd32(pkt + 8);
			slot = net_disc_count++;
			net_disc_addr[slot] = from;
			net_disc_code[slot] = 0;
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
	net_winsock_init();
	/* AVP_RELAY=host[:port] overrides the Relay Server setting, for testing. */
	if (getenv("AVP_RELAY")) Net_SetRelayServer(getenv("AVP_RELAY"));
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
   never receives its own DPID_ALLPLAYERS message, matching DirectPlay).
   DPSEND_GUARANTEED selects the reliable channel; see the header comment. */
HRESULT DpExtSend(int lpDP2A, DPID idFrom, DPID idTo, DWORD dwFlags, void *lpData, DWORD dwDataSize)
{
	if (net_role == NET_OFF) return 1;
	if ((int)dwDataSize > NET_FRAMEMAX) return DPERR_SENDTOOBIG;
	if (net_role == NET_CLIENT && net_nconns < 1) return DPERR_CONNECTIONLOST;
	net_route(NULL, idFrom, idTo, (dwFlags & DPSEND_GUARANTEED) != 0,
	          (const unsigned char *)lpData, (int)dwDataSize);
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

		/* Open the game + discovery sockets; sets AVPDPNetID and glpDP. */
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
	/* The player picked a discovered session: start the (non-blocking) join and go
	   to the "Joining..." screen, which polls DirectPlay_ConnectingToSession() each
	   frame to finish the handshake and become a joining peer. Nothing here blocks
	   the game loop. */
	struct sockaddr_in target;
	unsigned room;
	fprintf(stderr, "DirectPlay_ConnectToSession(%d, %s)\n", sessionNumber, playerName);

	if (sessionNumber < 0 || sessionNumber >= net_disc_count) {
		fprintf(stderr, "net: ConnectToSession: bad session index\n");
		return 0;
	}
	target = net_disc_addr[sessionNumber];   /* copy before net_begin_connect tears down browse */
	room   = net_disc_code[sessionNumber];

	memset(&AVPDPplayerName, 0, sizeof(AVPDPplayerName));
	AVPDPplayerName.dwSize = sizeof(DPNAME);
	AVPDPplayerName.lpszShortNameA = playerName;
	AVPDPplayerName.lpszLongNameA  = playerName;

	net_begin_connect(&target, room);
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

/* ---------------------------------------------------------------------------
 * Internet relay: configuration and status, for the frontend
 * ------------------------------------------------------------------------- */

/* "host[:port]", or empty for LAN only. Takes effect at the next host or join. */
void Net_SetRelayServer(const char *hostport)
{
	strncpy(NetRelayServer, hostport ? hostport : "", sizeof NetRelayServer - 1);
	NetRelayServer[sizeof NetRelayServer - 1] = '\0';
}

/* 1 and the code in 'out' while this machine is in an internet game: the host once
   the relay has opened the room, a client once it has joined. 0 otherwise. */
int Net_RelayRoomCode(char out[RELAY_CODE_LEN + 1])
{
	if (!net_relay_on || !net_relay_code) return 0;
	if (net_role == NET_HOST && !net_relay_open) return 0;
	if (net_role == NET_CLIENT && net_join_state != NET_JS_DONE) return 0;
	relay_code_to_str(net_relay_code, out);
	return 1;
}

/* The relay's last complaint as text, or NULL. Survives the teardown of a failed
   join, so the menu can still say why. */
const char *Net_RelayError(void)
{
	if (net_relay_err == 0) return NULL;
	return net_relay_errstr(net_relay_err);
}

/* End of Linux-related junk */
