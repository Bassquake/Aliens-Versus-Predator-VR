/* Wire protocol between the game (net.c) and the internet relay (tools/relay/relay.c).
 * Shared by both so they cannot drift.
 *
 * The relay is a forwarder, nothing more. The game's own protocol (reliable +
 * state channels, HELLO/WELCOME, acks) runs end to end between host and client,
 * wrapped in this envelope:
 *
 *   0 u32 magic 'AVPR'   4 u8 op   5 u8 relay version   6 u16 0   8 u32 val   12 payload
 *
 * All little-endian. val by op:
 *
 *   RELAY_HOST   host -> relay   val = room code wanted (0 = any). Opens the room, or
 *                                refreshes it. Resent until RELAY_ROOM, then every
 *                                RELAY_HOST_REFRESH_MS as a keepalive. Asking for a
 *                                code lets a host get its room back after a relay
 *                                restart, so players can rejoin with the code they
 *                                have. Games in progress are lost in a restart.
 *   RELAY_ROOM   relay -> host   val = the room's code
 *   RELAY_DATA   host -> relay   val = destination client's tag
 *                relay -> host   val = source client's tag
 *                client -> relay val = room code (the first one joins the room)
 *                relay -> client val = 0
 *   RELAY_CLOSE  host -> relay   the room is finished
 *   RELAY_LEAVE  client -> relay this client is finished
 *   RELAY_ERROR  relay -> either val = RELAY_ERR_*
 *
 * Clients and hosts are identified by their source address, as the relay sees it
 * (i.e. after NAT). Every packet from them refreshes them; they expire after
 * RELAY_IDLE_MS of silence, which is longer than the game's own 30 s link timeout
 * so the relay never forgets a peer the game still believes in.
 */
#ifndef RELAY_PROTO_H
#define RELAY_PROTO_H

#define RELAY_PORT             27017

/* The game's Relay Server setting: "host[:port]", blank for LAN only. The default
   is what a config.cfg with no #RELAY line gets, so set it once a public relay
   exists and players never have to type an address. */
#define RELAY_DEFAULT_SERVER   "bassquake.ddns.net"
#define RELAY_SERVER_LEN       48
#define RELAY_MAGIC            0x52505641u   /* 'AVPR' */
#define RELAY_VERSION          1
#define RELAY_HDR              12
#define RELAY_MAX_PAYLOAD      1400          /* game datagrams are at most 1200 */

#define RELAY_IDLE_MS          60000
#define RELAY_HOST_RESEND_MS   250           /* host: RELAY_HOST until RELAY_ROOM */
#define RELAY_HOST_REFRESH_MS  10000         /* host: RELAY_HOST once the room is open */
#define RELAY_HOST_TIMEOUT_MS  5000          /* host: give up if no RELAY_ROOM by then */

enum { RELAY_HOST = 1, RELAY_ROOM, RELAY_DATA, RELAY_CLOSE, RELAY_LEAVE, RELAY_ERROR };

enum {
	RELAY_ERR_NOROOM = 1,     /* no room with that code */
	RELAY_ERR_FULL,           /* the room has RELAY_ROOM_CLIENTS already */
	RELAY_ERR_BUSY,           /* the relay has no room left to open */
	RELAY_ERR_VERSION         /* a different relay protocol version */
};

/* One more than the game's own limit (net.c's NET_MAXCONNS, 7): the spare covers a
   player who dropped without a goodbye and holds a place until RELAY_IDLE_MS. The
   host turns an 8th client away itself, with a reason. */
#define RELAY_ROOM_CLIENTS     8

/* Room codes: 5 characters from 32, no 0/O or 1/I, so they can be read out loud.
   The value is a 25-bit number plus one, so 0 always means "no code". */
#define RELAY_CODE_LEN         5
#define RELAY_CODE_ALPHABET    "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
#define RELAY_CODE_MAX         (1u << 25)

/* Not every includer uses both helpers. */
#ifdef __GNUC__
#define RELAY_MAYBE_UNUSED __attribute__((unused))
#else
#define RELAY_MAYBE_UNUSED
#endif

RELAY_MAYBE_UNUSED static void relay_code_to_str(unsigned code, char out[RELAY_CODE_LEN + 1])
{
	unsigned v = code - 1;
	int i;
	for (i = RELAY_CODE_LEN - 1; i >= 0; i--) { out[i] = RELAY_CODE_ALPHABET[v & 31]; v >>= 5; }
	out[RELAY_CODE_LEN] = '\0';
}

/* Returns 0 if s is not a code. Case-insensitive, and spaces and dashes are skipped,
   so "AB-CDE" and "ab cde" both work. 0/O/1/I are rejected rather than guessed at. */
RELAY_MAYBE_UNUSED static unsigned relay_code_from_str(const char *s)
{
	unsigned v = 0;
	int n = 0;
	for (; *s; s++) {
		char c = *s;
		const char *p;
		if (c == ' ' || c == '-') continue;
		if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
		for (p = RELAY_CODE_ALPHABET; *p && *p != c; p++) {}
		if (!*p || n == RELAY_CODE_LEN) return 0;
		v = (v << 5) | (unsigned)(p - RELAY_CODE_ALPHABET);
		n++;
	}
	return n == RELAY_CODE_LEN ? v + 1 : 0;
}

#endif
