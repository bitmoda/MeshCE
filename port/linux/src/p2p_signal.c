/*
P2P_SIGNAL.C

Internet play's handshake (p2p.c). A joiner and a host tell each other where
they can be reached by UDP, on the tunnel socket, to the address in the
invite. There is no signalling broker and no server-browser listing.

The messages are sealed with a key derived from the invite's token
(p2p_crypto.c):

- the joiner sends JOIN: its public key (its identifier is the key's hash),
  a nonce, and its addresses;
- the host answers ACCEPT: its public key, the joiner's nonce, one of its
  own, its addresses, and a tag that only the two of them can make (from
  their keys). The host signs that sealed answer with its mesh key. The
  private key stays on the host;
- the joiner then repeats its JOIN with the host's nonce and a tag of its
  own, made the same way, which shows that it holds the key it gave. Only
  then does the host make a session.

Their tunnel's keys come from their X25519 shared secret and the two
nonces, and never travel. Another holder of the invite reads the messages
but cannot work them out, and cannot answer as the host (whose key must
have the hash in the invite: 16 bytes, not only the identifier's 6, which
a key could be made to have). It can send a JOIN in another machine's name,
but cannot prove it: the host answers it (to that machine) and makes no
session of it.

The host's nonce is a hash of the request with a key of the host's and the
time (it changes every HOST_NONCE_PERIOD), so the host need not remember
what it answered. What it does keep of requests not proven yet (the work of
their keys) only saves work. It answers them sparingly, as anyone with the
invite can send them from as many keys as it likes.

A joiner repeats its JOIN until the tunnel reaches the host. The host makes
one session of a request (a public key and nonce) at most. It remembers the
request at least while its host nonce lasts. A joiner asks with a new nonce
when its session with the host ends before the tunnel reached it, or when
the host has not answered it in a while.

A player of the same game, other than the host, is not introduced to the
other joiners. The host and the joiner do tell the mesh about each other
once the join is accepted. That does not start a search. Map pieces travel
on the tunnel each joiner already has with the host.
*/

#include "platform.h"
#include "posix.h"
#include "p2p_internal.h"
#include "peer_mesh.h"

#include <string.h>

enum
{
	/* the joiners a host made sessions for, so a repeated request gets the
	same session: as many as it takes */
	MAXIMUM_JOINERS = P2P_MAXIMUM_PEERS + 1,
	/* the requests to join a machine makes at once: the invite's host, and
	other players of its game (p2p_signal_join_player) */
	MAXIMUM_JOINS = 1 + P2P_MAXIMUM_PLAYER_JOINS,
	/* the nonces of an asker's key it keeps the answer times of */
	ASKER_NONCES = 4,
	/* the requests not proven yet whose keys' work a host keeps, one a key;
	only to save work, but as many as a flood of new keys takes seconds to
	push out (one each UNPROVEN_ANSWER_INTERVAL at most, after the first
	few: about 11 seconds), so a joiner's proof, which follows its answer at
	once, finds its own */
	MAXIMUM_ASKERS = 256,
	/* the requests a session was made from, which make none again (a
	host takes at most a few new players a minute: this is hours of them;
	one is not forgotten while its host nonce lasts: USED_REQUEST_TIME) */
	MAXIMUM_USED_REQUESTS = 1024,
	NONCE_SIZE = 8,
	/* an ACCEPT's or a proven JOIN's tag: the first half of an HMAC-SHA256 */
	TAG_SIZE = 16,
	/* a proven JOIN's end: the host's nonce, and the tag */
	PROOF_SIZE = NONCE_SIZE + TAG_SIZE,
	MAXIMUM_MESSAGE_SIZE = 512,

	/* milliseconds */
	JOIN_INTERVAL = 2000,
	ANSWER_INTERVAL = 1000,
	/* a joiner the host has not answered in this long asks anew, with a
	new nonce (a fresh start: the host makes nothing of a request it did
	not answer, as it makes a session only of a proven one) */
	UNANSWERED_TIME = 20000,
	/* a host nonce is taken in the period it is made in and the next */
	HOST_NONCE_PERIOD = 30000,
	USED_REQUEST_TIME = 2 * HOST_NONCE_PERIOD,
	/* a host's work of the keys of requests from keys it has not met (a
	millisecond or more each, on the thread the tunnels run on): this many
	at once, and one more each this often */
	MAXIMUM_KEY_WORK = 32,
	KEY_WORK_INTERVAL = 50,
	/* a host's answers to requests not proven yet, which anyone with the
	invite can send from any number of keys: this many at once, and one more
	each this often */
	MAXIMUM_UNPROVEN_ANSWERS = 32,
	UNPROVEN_ANSWER_INTERVAL = 50,
};

enum
{
	_message_join = 'J',
	_message_accept = 'A',
	/* 3: a JOIN proves its key before the host makes a session; 4: both
	messages say the machine's WebRTC (p2p_webrtc.c), so browsers join */
	MESSAGE_VERSION = 4,
};

/* a request: of a session made (joiners), or not proven yet (askers: one a
key, with its latest nonce, and no host nonce or secret; and its latest
nonces answered, and when) */
struct joiner
{
	unsigned char identifier[P2P_IDENTIFIER_SIZE];
	unsigned char public_key[P2P_KEY_SIZE];
	/* its and the host's */
	unsigned char nonce[NONCE_SIZE];
	unsigned char host_nonce[NONCE_SIZE];
	/* pair_base's, and their session's secret */
	unsigned char base[P2P_SHA256_SIZE];
	unsigned char secret[P2P_SHA256_SIZE];
	unsigned long answered_time;
	/* (an asker's) its latest nonces answered, and when */
	unsigned char answered_nonces[ASKER_NONCES][NONCE_SIZE];
	unsigned long answered_nonce_times[ASKER_NONCES];
	int used;
};

/* a request to join: to the invite's host, or to another player of its
game (who answers as a host does) */
struct join
{
	int active;
	/* the identifier of the machine asked, and the hash of its key (the
	invite's, or as the game's host told) */
	unsigned char host[P2P_IDENTIFIER_SIZE];
	unsigned char host_hash[P2P_KEY_HASH_SIZE];
	unsigned char nonce[NONCE_SIZE];
	/* its public key, once it answered, and pair_base's */
	int has_base;
	unsigned char host_public[P2P_KEY_SIZE];
	unsigned char base[P2P_SHA256_SIZE];
	unsigned long sent_time;
	/* when nonce was made, and whether the machine answered it (with
	host_nonce, which the proof carries) */
	unsigned long nonce_time;
	int answered;
	unsigned char host_nonce[NONCE_SIZE];
};

/* work allowed: some at once, and one more each interval */
struct budget
{
	int left;
	/* as of when */
	unsigned long time;
};

struct used_request
{
	/* the joiner's identifier and nonce */
	unsigned char request[P2P_IDENTIFIER_SIZE + NONCE_SIZE];
	unsigned long time;
};

static struct
{
	int started;

	/* hosting */
	int hosting;
	/* the address in the invite being joined, and that host's public mesh key */
	int direct_set;
	unsigned long direct_address;
	unsigned short direct_port;
	unsigned char direct_hash[P2P_KEY_HASH_SIZE];
	int direct_key_set;
	unsigned char direct_key[32];
	/* where the handshake packet in hand came from, while it is handled */
	int direct_reply_set;
	unsigned long direct_reply_address;
	unsigned short direct_reply_port;
	unsigned char host_token[P2P_TOKEN_SIZE];
	unsigned char host_key[P2P_SHA256_SIZE];
	struct joiner joiners[MAXIMUM_JOINERS];
	int next_joiner;
	struct joiner askers[MAXIMUM_ASKERS];
	int next_asker;
	/* the key of the host's nonces (host_nonce_for), for the run */
	int has_nonce_key;
	unsigned char nonce_key[P2P_SHA256_SIZE];
	/* each request a session was made from (kept while hosting stops and
	starts: an invite lasts the run) */
	struct used_request used_requests[MAXIMUM_USED_REQUESTS];
	int used_request_count;
	int used_request_next;
	/* the key work it may do now (MAXIMUM_KEY_WORK), and the answers it
	may give requests not proven yet (MAXIMUM_UNPROVEN_ANSWERS) */
	struct budget key_work;
	struct budget unproven_answers;

	/* joining: the invite's host (joins[0]), and the other players of its
	game this machine asks for as their host does for it (p2p_signal_join_player);
	all of one token */
	struct join joins[MAXIMUM_JOINS];
	unsigned char join_token[P2P_TOKEN_SIZE];
	unsigned char join_key[P2P_SHA256_SIZE];
} signalling;

static int elapsed(unsigned long since, unsigned long time)
{
	/* 0 is never; unsigned subtraction also survives clock wrap. */
	return !since || (unsigned int)(p2p_now() - since) >= (unsigned int)time;
}

/* whether some of a budget is left now (maximum at once, one more each
interval); spend takes one */
static int budget_left(struct budget *budget, int maximum, int interval, int spend)
{
	unsigned long now = p2p_now();
	unsigned long earned = (now - budget->time) / (unsigned long)interval;

	if (earned >= (unsigned long)maximum || budget->left + (int)earned >= maximum)
	{
		budget->left = maximum;
		budget->time = now;
	}
	else if (earned)
	{
		budget->left += (int)earned;
		budget->time += earned * (unsigned long)interval;
	}
	if (budget->left <= 0)
		return 0;
	if (spend)
		budget->left--;
	return 1;
}

static int joins_active(void);
static void send_join(struct join *join);
static void direct_send(unsigned long address, unsigned short port, const unsigned char *sealed, int size, int sign);
static void send_mesh_intro(unsigned long address, unsigned short port);

/* ---------- what is derived from a token */

static void derive(const unsigned char *token, const char *label, const unsigned char *identifier,
	unsigned char *digest)
{
	unsigned char data[16 + P2P_IDENTIFIER_SIZE];
	int size = (int)strlen(label);

	memcpy(data, label, (size_t)size);
	if (identifier)
	{
		memcpy(data + size, identifier, P2P_IDENTIFIER_SIZE);
		size += P2P_IDENTIFIER_SIZE;
	}
	p2p_hmac_sha256(token, P2P_TOKEN_SIZE, data, size, digest);
}

/* ---------- addresses */

/* this machine's addresses and its WebRTC (p2p_webrtc_describe), for a
message to the machine with that identifier: the count of addresses, each
address and port, then the WebRTC's kind and, unless none, the certificate's
hash, the ICE username fragment and password, each after its length, and
the count of mDNS names, each after its length and before its port; returns
the bytes written */
static int put_addresses(unsigned char *message, const unsigned char *identifier, int proven)
{
	struct p2p_candidate candidates[P2P_MAXIMUM_CANDIDATES];
	struct p2p_webrtc webrtc;
	int count = p2p_webrtc_describe(identifier, proven, &webrtc, candidates, P2P_MAXIMUM_CANDIDATES);
	int size;
	int index;

	message[0] = (unsigned char)count;
	for (index = 0; index < count; index++)
	{
		memcpy(message + 1 + index * 6, &candidates[index].address, 4);
		memcpy(message + 1 + index * 6 + 4, &candidates[index].port, 2);
	}
	size = 1 + count * 6;
	message[size++] = (unsigned char)webrtc.kind;
	if (webrtc.kind == _p2p_webrtc_none)
		return size;
	memcpy(message + size, webrtc.fingerprint, P2P_FINGERPRINT_SIZE);
	size += P2P_FINGERPRINT_SIZE;
	message[size] = (unsigned char)strlen(webrtc.ufrag);
	memcpy(message + size + 1, webrtc.ufrag, message[size]);
	size += 1 + message[size];
	message[size] = (unsigned char)strlen(webrtc.password);
	memcpy(message + size + 1, webrtc.password, message[size]);
	size += 1 + message[size];
	message[size++] = (unsigned char)webrtc.name_count;
	for (index = 0; index < webrtc.name_count; index++)
	{
		message[size] = (unsigned char)strlen(webrtc.names[index]);
		memcpy(message + size + 1, webrtc.names[index], message[size]);
		size += 1 + message[size];
		memcpy(message + size, &webrtc.name_ports[index], 2);
		size += 2;
	}
	return size;
}

/* whether text is an mDNS name of a browser's address: letters, digits and
"-" before ".local" (a browser puts it in its session description) */
static int webrtc_name_valid(const char *text)
{
	int length = (int)strlen(text);

	return length > 6 && !strcmp(text + length - 6, ".local") && (int)strspn(text,
		"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") == length - 6;
}

/* ICE's text (RFC 8839's ice-char: letters, digits, "+" and "/"), of a
length from minimum to maximum, or none; a browser puts it in its session
description */
static int get_ice_text(const unsigned char *message, int size, int *offset, int minimum, int maximum,
	char *text)
{
	int length;
	int index;

	if (*offset >= size)
		return 0;
	length = message[(*offset)++];
	if ((length && (length < minimum || length > maximum)) || *offset + length > size)
		return 0;
	for (index = 0; index < length; index++)
	{
		char character = (char)message[*offset + index];

		if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9') || character == '+' || character == '/'))
		{
			return 0;
		}
		text[index] = character;
	}
	text[length] = 0;
	*offset += length;
	return 1;
}

/* the reverse: the bytes read, or -1 if they are not made so */
static int get_addresses(const unsigned char *message, int size, struct p2p_candidate *candidates, int *count,
	struct p2p_webrtc *webrtc)
{
	int offset;
	int index;

	memset(webrtc, 0, sizeof(*webrtc));
	if (size < 2)
		return -1;
	*count = message[0];
	if (*count > P2P_MAXIMUM_CANDIDATES || size < 2 + *count * 6)
		return -1;
	for (index = 0; index < *count; index++)
	{
		unsigned int address;

		memcpy(&address, message + 1 + index * 6, 4);
		candidates[index].address = address;
		memcpy(&candidates[index].port, message + 1 + index * 6 + 4, 2);
	}
	offset = 1 + *count * 6;
	webrtc->kind = message[offset++];
	if (webrtc->kind == _p2p_webrtc_none)
		return offset;
	if ((webrtc->kind != _p2p_webrtc_native && webrtc->kind != _p2p_webrtc_browser) ||
		offset + P2P_FINGERPRINT_SIZE > size)
	{
		return -1;
	}
	memcpy(webrtc->fingerprint, message + offset, P2P_FINGERPRINT_SIZE);
	offset += P2P_FINGERPRINT_SIZE;
	if (!get_ice_text(message, size, &offset, 4, P2P_ICE_UFRAG_SIZE - 1, webrtc->ufrag) ||
		!get_ice_text(message, size, &offset, 22, P2P_ICE_PASSWORD_SIZE - 1, webrtc->password) ||
		!webrtc->ufrag[0] != !webrtc->password[0] || offset >= size)
	{
		return -1;
	}
	webrtc->name_count = message[offset++];
	if (webrtc->name_count > P2P_MAXIMUM_WEBRTC_NAMES)
		return -1;
	for (index = 0; index < webrtc->name_count; index++)
	{
		int length;

		if (offset >= size)
			return -1;
		length = message[offset++];
		if (length >= P2P_WEBRTC_NAME_SIZE || offset + length + 2 > size)
			return -1;
		memcpy(webrtc->names[index], message + offset, (size_t)length);
		webrtc->names[index][length] = 0;
		memcpy(&webrtc->name_ports[index], message + offset + length, 2);
		offset += length + 2;
		if (!webrtc_name_valid(webrtc->names[index]) || !webrtc->name_ports[index])
			return -1;
	}
	return offset;
}

/* ---------- the keys: what only a joiner and the host can work out */

/* HMAC(their X25519 shared secret, "hceu/2" | the joiner's public key | the
host's); other is the one that is not this machine's. 0 if it is unusable */
static int pair_base(const unsigned char *joiner_public, const unsigned char *host_public,
	const unsigned char *other, unsigned char *base)
{
	unsigned char shared[P2P_KEY_SIZE];
	unsigned char data[6 + 2 * P2P_KEY_SIZE];

	if (!p2p_shared_secret(other, shared))
		return 0;
	memcpy(data, "hceu/2", 6);
	memcpy(data + 6, joiner_public, P2P_KEY_SIZE);
	memcpy(data + 6 + P2P_KEY_SIZE, host_public, P2P_KEY_SIZE);
	p2p_hmac_sha256(shared, P2P_KEY_SIZE, data, sizeof(data), base);
	return 1;
}

/* a session's secret, which the tunnel's keys come from (p2p.c) */
static void session_secret(const unsigned char *base, const unsigned char *nonce, const unsigned char *host_nonce,
	unsigned char *secret)
{
	unsigned char data[7 + 2 * NONCE_SIZE];

	memcpy(data, "session", 7);
	memcpy(data + 7, nonce, NONCE_SIZE);
	memcpy(data + 7 + NONCE_SIZE, host_nonce, NONCE_SIZE);
	p2p_hmac_sha256(base, P2P_SHA256_SIZE, data, sizeof(data), secret);
}

/* an ACCEPT's tag (label "accept") or a proven JOIN's ("join"), over what
precedes it */
static void message_tag(const unsigned char *base, const char *label, const unsigned char *message, int size,
	unsigned char *tag)
{
	unsigned char data[6 + MAXIMUM_MESSAGE_SIZE];
	unsigned char digest[P2P_SHA256_SIZE];
	int label_size = (int)strlen(label);

	memcpy(data, label, (size_t)label_size);
	memcpy(data + label_size, message, (size_t)size);
	p2p_hmac_sha256(base, P2P_SHA256_SIZE, data, label_size + size, digest);
	memcpy(tag, digest, TAG_SIZE);
}

/* whether a message's tag (its last TAG_SIZE bytes) is right */
static int tag_right(const unsigned char *base, const char *label, const unsigned char *message, int size)
{
	unsigned char tag[TAG_SIZE];

	message_tag(base, label, message, size - TAG_SIZE, tag);
	return p2p_equal(tag, message + size - TAG_SIZE, TAG_SIZE);
}

/* the host's nonce for a request (a joiner's public key and nonce) in a
period of HOST_NONCE_PERIOD */
static void host_nonce_for(const unsigned char *public_key, const unsigned char *nonce, unsigned long period,
	unsigned char *host_nonce)
{
	unsigned char data[4 + P2P_KEY_SIZE + NONCE_SIZE];
	unsigned char digest[P2P_SHA256_SIZE];
	int index;

	for (index = 0; index < 4; index++)
		data[index] = (unsigned char)(period >> (index * 8));
	memcpy(data + 4, public_key, P2P_KEY_SIZE);
	memcpy(data + 4 + P2P_KEY_SIZE, nonce, NONCE_SIZE);
	p2p_hmac_sha256(signalling.nonce_key, P2P_SHA256_SIZE, data, sizeof(data), digest);
	memcpy(host_nonce, digest, NONCE_SIZE);
}

/* whether the host answered a request with this nonce lately: in this
period, or the last */
static int host_nonce_current(const unsigned char *public_key, const unsigned char *nonce,
	const unsigned char *host_nonce)
{
	unsigned long period = p2p_now() / HOST_NONCE_PERIOD;
	unsigned char expected[NONCE_SIZE];

	host_nonce_for(public_key, nonce, period, expected);
	if (p2p_equal(expected, host_nonce, NONCE_SIZE))
		return 1;
	/* (the last before the clock wraps, before the first) */
	host_nonce_for(public_key, nonce, period ? period - 1 : 0xFFFFFFFFUL / HOST_NONCE_PERIOD, expected);
	return p2p_equal(expected, host_nonce, NONCE_SIZE);
}

/* ---------- the messages */

/* the sealed handshake, on the tunnel socket. The host's answer carries a
signature of those bytes by its mesh key. The private key is not sent. */
static void direct_send(unsigned long address, unsigned short port, const unsigned char *sealed, int size, int sign)
{
	unsigned char packet[4 + MAXIMUM_MESSAGE_SIZE + P2P_SEAL_OVERHEAD + 64];
	unsigned char signature[64];
	int total;

	if (!address || !port || size <= 0 || size > MAXIMUM_MESSAGE_SIZE + P2P_SEAL_OVERHEAD)
		return;
	packet[0] = 0x70;
	packet[1] = 1;
	packet[2] = (unsigned char)(size >> 8);
	packet[3] = (unsigned char)size;
	memcpy(packet + 4, sealed, (size_t)size);
	total = 4 + size;
	if (sign)
	{
		int signed_ok;

		pthread_mutex_unlock(&p2p_lock);
		signed_ok = peer_mesh_sign(sealed, size, signature);
		pthread_mutex_lock(&p2p_lock);
		if (signed_ok && total + 64 <= (int)sizeof(packet))
		{
			memcpy(packet + total, signature, 64);
			total += 64;
		}
	}
	p2p_tunnel_send(address, port, packet, total);
}

/* A proven join may introduce itself on the tunnel for this long. A signed
   packet from any other address is ignored, so it cannot fill the peer list. */
enum { ADMITTED_JOINERS = 16, ADMITTED_MS = 30 * 60 * 1000 };

static struct
{
	unsigned long address;
	unsigned at;
} admitted_joiner[ADMITTED_JOINERS];
static int admitted_next;

static void admit_joiner(unsigned long address)
{
	int index;
	unsigned now;

	if (!address)
		return;
	now = p2p_now();
	for (index = 0; index < ADMITTED_JOINERS; index++)
	{
		if (admitted_joiner[index].address != address)
			continue;
		admitted_joiner[index].at = now ? now : 1;
		return;
	}
	admitted_joiner[admitted_next].address = address;
	admitted_joiner[admitted_next].at = now ? now : 1;
	admitted_next = (admitted_next + 1) % ADMITTED_JOINERS;
}

static int joiner_admitted(unsigned long address)
{
	int index;
	unsigned now = p2p_now();

	if (!address)
		return 0;
	for (index = 0; index < ADMITTED_JOINERS; index++)
	{
		if (admitted_joiner[index].address == address && admitted_joiner[index].at &&
			now - admitted_joiner[index].at < ADMITTED_MS)
			return 1;
	}
	return 0;
}

/* This client's mesh key and the public port of its mesh socket. The game
   tunnel is a different socket, so the other client learns that port here. */
static void send_mesh_intro(unsigned long address, unsigned short port)
{
	unsigned char packet[2 + 32 + 4 + 2 + 64];
	unsigned char key[32];
	unsigned long mapped_address = 0;
	unsigned short mapped_port = 0;
	int ready = 0;

	if (!address || !port)
		return;
	pthread_mutex_unlock(&p2p_lock);
	packet[0] = 0x71;
	packet[1] = 1;
	peer_mesh_mapped(&mapped_address, &mapped_port);
	if (peer_mesh_public(key))
	{
		memcpy(packet + 2, key, 32);
		memcpy(packet + 34, &mapped_address, 4);
		memcpy(packet + 38, &mapped_port, 2);
		ready = peer_mesh_sign(packet, 40, packet + 40);
	}
	pthread_mutex_lock(&p2p_lock);
	if (ready)
		p2p_tunnel_send(address, port, packet, 2 + 32 + 4 + 2 + 64);
}

static void send_join(struct join *join)
{
	unsigned char message[MAXIMUM_MESSAGE_SIZE];
	unsigned char sealed[MAXIMUM_MESSAGE_SIZE + P2P_SEAL_OVERHEAD];
	int size = 0;

	message[size++] = _message_join;
	message[size++] = MESSAGE_VERSION;
	memcpy(message + size, p2p_public_key(), P2P_KEY_SIZE);
	size += P2P_KEY_SIZE;
	memcpy(message + size, join->nonce, NONCE_SIZE);
	size += NONCE_SIZE;
	size += put_addresses(message + size, join->host, 1);
	/* answered: the proof that this machine holds its key, of which the host
	makes the session */
	if (join->answered)
	{
		memcpy(message + size, join->host_nonce, NONCE_SIZE);
		size += NONCE_SIZE;
		message_tag(join->base, "join", message, size, message + size);
		size += TAG_SIZE;
	}
	size = p2p_seal(signalling.join_key, message, size, sealed);
	/* the invite names one host. another player of that game has no address here. */
	if (signalling.direct_set && !memcmp(join->host_hash, signalling.direct_hash, P2P_KEY_HASH_SIZE))
		direct_send(signalling.direct_address, signalling.direct_port, sealed, size, 0);
	if (join->answered && signalling.direct_key_set && signalling.direct_address && signalling.direct_port)
		send_mesh_intro(signalling.direct_address, signalling.direct_port);
	join->sent_time = p2p_now();
}

/* whether any request to join is under way */
static int joins_active(void)
{
	int index;

	for (index = 0; index < MAXIMUM_JOINS; index++)
	{
		if (signalling.joins[index].active)
			return 1;
	}
	return 0;
}

/* starts a request to the machine whose key has this hash, holding this
token, with a new nonce (the host makes one session of a request) */
static void join_start(struct join *join, const unsigned char *host_hash, const unsigned char *token)
{
	memset(join, 0, sizeof(*join));
	memcpy(join->host_hash, host_hash, P2P_KEY_HASH_SIZE);
	p2p_identifier_from_hash(host_hash, join->host);
	posix_random_bytes(join->nonce, NONCE_SIZE);
	join->nonce_time = p2p_now();
	join->active = 1;
	(void)token;
	send_join(join);
}

/* the request of a key and nonce (NULL: any) in a list */
static struct joiner *find_joiner(struct joiner *list, int count, const unsigned char *public_key,
	const unsigned char *nonce)
{
	int index;

	for (index = 0; index < count; index++)
	{
		if (list[index].used && !memcmp(list[index].public_key, public_key, P2P_KEY_SIZE) &&
			(!nonce || !memcmp(list[index].nonce, nonce, NONCE_SIZE)))
		{
			return &list[index];
		}
	}
	return NULL;
}

/* pair_base with a joiner: kept from a request of its, else worked out */
static int joiner_base(const unsigned char *public_key, unsigned char *base)
{
	int index;

	for (index = 0; index < MAXIMUM_JOINERS + MAXIMUM_ASKERS; index++)
	{
		struct joiner const *joiner = index < MAXIMUM_JOINERS ? &signalling.joiners[index] :
			&signalling.askers[index - MAXIMUM_JOINERS];

		if (joiner->used && !memcmp(joiner->public_key, public_key, P2P_KEY_SIZE))
		{
			memcpy(base, joiner->base, P2P_SHA256_SIZE);
			return 1;
		}
	}
	/* (none left: the request is not answered, and asked again) */
	if (!budget_left(&signalling.key_work, MAXIMUM_KEY_WORK, KEY_WORK_INTERVAL, 1))
		return 0;
	return pair_base(public_key, p2p_public_key(), public_key, base);
}

static int request_used(const unsigned char *request)
{
	int index;

	for (index = 0; index < signalling.used_request_count; index++)
	{
		if (!memcmp(signalling.used_requests[index].request, request, P2P_IDENTIFIER_SIZE + NONCE_SIZE))
			return 1;
	}
	return 0;
}

/* the host's answer to a request, back to the address it arrived from */
static void send_accept(const unsigned char *identifier, const unsigned char *nonce,
	const unsigned char *host_nonce, const unsigned char *base, int proven)
{
	unsigned char answer[MAXIMUM_MESSAGE_SIZE];
	unsigned char sealed[MAXIMUM_MESSAGE_SIZE + P2P_SEAL_OVERHEAD];
	int size = 0;

	answer[size++] = _message_accept;
	answer[size++] = MESSAGE_VERSION;
	memcpy(answer + size, p2p_public_key(), P2P_KEY_SIZE);
	size += P2P_KEY_SIZE;
	memcpy(answer + size, nonce, NONCE_SIZE);
	size += NONCE_SIZE;
	memcpy(answer + size, host_nonce, NONCE_SIZE);
	size += NONCE_SIZE;
	size += put_addresses(answer + size, identifier, proven);
	message_tag(base, "accept", answer, size, answer + size);
	size += TAG_SIZE;
	size = p2p_seal(signalling.host_key, answer, size, sealed);
	/* (a proven joiner's first: its session is made; others ask again) */
	if (signalling.direct_reply_set)
		direct_send(signalling.direct_reply_address, signalling.direct_reply_port, sealed, size, 1);
}

/* the host: a joiner asked, with a proof (the host's nonce, and a tag) once
the host answered it */
static void join_received(const unsigned char *message, int size)
{
	struct p2p_candidate candidates[P2P_MAXIMUM_CANDIDATES];
	struct p2p_webrtc webrtc;
	static const struct p2p_webrtc no_webrtc;
	unsigned char identifier[P2P_IDENTIFIER_SIZE];
	unsigned char request[P2P_IDENTIFIER_SIZE + NONCE_SIZE];
	unsigned char host_nonce[NONCE_SIZE];
	unsigned char base[P2P_SHA256_SIZE];
	unsigned char secret[P2P_SHA256_SIZE];
	const unsigned char *public_key = message + 2;
	const unsigned char *nonce = public_key + P2P_KEY_SIZE;
	int fixed = 2 + P2P_KEY_SIZE + NONCE_SIZE;
	struct joiner *joiner;
	struct used_request *used;
	int proven;
	int count;
	int read;

	/* (a browser answers once its WebRTC can be told: the joiner asks
	again) */
	if (size < fixed + 1 || !p2p_webrtc_ready())
		return;
	read = get_addresses(message + fixed, size - fixed, candidates, &count, &webrtc);
	if (read < 0)
		return;
	proven = size - fixed - read;
	if (proven != 0 && proven != PROOF_SIZE)
		return;
	p2p_identifier_for(public_key, identifier);
	joiner = find_joiner(signalling.joiners, MAXIMUM_JOINERS, public_key, nonce);
	if (joiner)
	{
		/* a request a session was made from, again (repeated until the
		tunnel reaches the host): that session's answer, while it lasts;
		never another. The addresses only from a proof (anyone can send the
		rest) */
		if (proven && (memcmp(message + size - PROOF_SIZE, joiner->host_nonce, NONCE_SIZE) ||
			!tag_right(joiner->base, "join", message, size)))
		{
			return;
		}
		if (!elapsed(joiner->answered_time, ANSWER_INTERVAL) ||
			!p2p_peer_reoffered(identifier, joiner->secret, candidates, proven ? count : 0,
			proven ? &webrtc : &no_webrtc) ||
			(!proven && !budget_left(&signalling.unproven_answers, MAXIMUM_UNPROVEN_ANSWERS,
			UNPROVEN_ANSWER_INTERVAL, 1)))
		{
			return;
		}
		joiner->answered_time = p2p_now();
		send_accept(identifier, joiner->nonce, joiner->host_nonce, joiner->base, proven);
		return;
	}
	memcpy(request, identifier, P2P_IDENTIFIER_SIZE);
	memcpy(request + P2P_IDENTIFIER_SIZE, nonce, NONCE_SIZE);
	if (request_used(request))
		return;
	if (!proven)
	{
		/* an answer (to the machine whose key it is, which alone can prove
		the request), and nothing else: the host keeps nothing of it that it
		needs. A request's once each ANSWER_INTERVAL (a key's with another
		nonce too: anyone may send its key with theirs, which must not keep
		its own out), and few in all */
		struct joiner *asker = find_joiner(signalling.askers, MAXIMUM_ASKERS, public_key, NULL);
		int slot = 0;

		if (asker)
		{
			int index;

			for (index = 0; index < ASKER_NONCES; index++)
			{
				if (!memcmp(asker->answered_nonces[index], nonce, NONCE_SIZE))
				{
					slot = index;
					break;
				}
				if ((long)(asker->answered_nonce_times[index] - asker->answered_nonce_times[slot]) < 0)
					slot = index;
			}
			if (index < ASKER_NONCES && !elapsed(asker->answered_nonce_times[index], ANSWER_INTERVAL))
				return;
		}
		/* (checked before the work of the keys, which anyone with the invite
		can ask for as often as they like) */
		if (p2p_peer_turned_away(identifier, 0) ||
			!budget_left(&signalling.unproven_answers, MAXIMUM_UNPROVEN_ANSWERS, UNPROVEN_ANSWER_INTERVAL, 0))
		{
			return;
		}
		if (!asker)
		{
			if (!joiner_base(public_key, base))
				return;
			asker = &signalling.askers[signalling.next_asker];
			signalling.next_asker = (signalling.next_asker + 1) % MAXIMUM_ASKERS;
			memset(asker, 0, sizeof(*asker));
			memcpy(asker->identifier, identifier, P2P_IDENTIFIER_SIZE);
			memcpy(asker->public_key, public_key, P2P_KEY_SIZE);
			memcpy(asker->base, base, P2P_SHA256_SIZE);
			asker->used = 1;
		}
		budget_left(&signalling.unproven_answers, MAXIMUM_UNPROVEN_ANSWERS, UNPROVEN_ANSWER_INTERVAL, 1);
		memcpy(asker->nonce, nonce, NONCE_SIZE);
		asker->answered_time = p2p_now();
		memcpy(asker->answered_nonces[slot], nonce, NONCE_SIZE);
		asker->answered_nonce_times[slot] = asker->answered_time;
		host_nonce_for(public_key, nonce, p2p_now() / HOST_NONCE_PERIOD, host_nonce);
		send_accept(identifier, nonce, host_nonce, asker->base, 0);
		return;
	}
	/* proven: with a nonce the host answered the request with lately, and a
	tag only the key's holder can make. A new session (p2p.c turns it away
	while another with that machine lives) */
	memcpy(host_nonce, message + size - PROOF_SIZE, NONCE_SIZE);
	if (!host_nonce_current(public_key, nonce, host_nonce) || p2p_peer_turned_away(identifier, 0) ||
		!joiner_base(public_key, base) || !tag_right(base, "join", message, size))
	{
		return;
	}
	/* (a request is remembered while its proof lasts, at least: a copy of it
	would make the session again, with the same keys) */
	used = &signalling.used_requests[signalling.used_request_next];
	if (signalling.used_request_count == MAXIMUM_USED_REQUESTS && !elapsed(used->time, USED_REQUEST_TIME))
		return;
	session_secret(base, nonce, host_nonce, secret);
	if (!p2p_peer_offered(identifier, public_key, secret, candidates, count, &webrtc, _p2p_offered_joiner))
		return;
	memcpy(used->request, request, sizeof(request));
	used->time = p2p_now();
	signalling.used_request_next = (signalling.used_request_next + 1) % MAXIMUM_USED_REQUESTS;
	if (signalling.used_request_count < MAXIMUM_USED_REQUESTS)
		signalling.used_request_count++;
	joiner = &signalling.joiners[signalling.next_joiner];
	signalling.next_joiner = (signalling.next_joiner + 1) % MAXIMUM_JOINERS;
	memcpy(joiner->identifier, identifier, P2P_IDENTIFIER_SIZE);
	memcpy(joiner->public_key, public_key, P2P_KEY_SIZE);
	memcpy(joiner->nonce, nonce, NONCE_SIZE);
	memcpy(joiner->host_nonce, host_nonce, NONCE_SIZE);
	memcpy(joiner->base, base, P2P_SHA256_SIZE);
	memcpy(joiner->secret, secret, P2P_SHA256_SIZE);
	joiner->answered_time = p2p_now();
	joiner->used = 1;
	if (signalling.direct_reply_address)
		admit_joiner(signalling.direct_reply_address);
	send_accept(identifier, nonce, host_nonce, base, 1);
	if (signalling.direct_reply_address && signalling.direct_reply_port)
	{
		unsigned long joiner_address = signalling.direct_reply_address;
		unsigned short joiner_port = signalling.direct_reply_port;

		send_mesh_intro(joiner_address, joiner_port);
		pthread_mutex_unlock(&p2p_lock);
		peer_mesh_greet(joiner_address, 0);
		pthread_mutex_lock(&p2p_lock);
	}
}

/* the joiner: the host (or the player) asked answered */
static void accept_received(const unsigned char *message, int size)
{
	struct p2p_candidate candidates[P2P_MAXIMUM_CANDIDATES];
	struct p2p_webrtc webrtc;
	unsigned char hash[P2P_KEY_HASH_SIZE];
	unsigned char secret[P2P_SHA256_SIZE];
	const unsigned char *host_public = message + 2;
	const unsigned char *nonce = host_public + P2P_KEY_SIZE;
	const unsigned char *host_nonce = nonce + NONCE_SIZE;
	int fixed = 2 + P2P_KEY_SIZE + 2 * NONCE_SIZE;
	struct join *join = NULL;
	int count;
	int index;

	if (size < fixed + 1 + TAG_SIZE)
		return;
	/* (the request it answers: by its nonce) */
	for (index = 0; index < MAXIMUM_JOINS && !join; index++)
	{
		struct join *asked = &signalling.joins[index];

		if (asked->active && !memcmp(nonce, asked->nonce, NONCE_SIZE))
			join = asked;
	}
	if (!join)
		return;
	/* the first answer to the request holds: the proof carries its host
	nonce (the host's changes every HOST_NONCE_PERIOD) */
	if (join->answered && (memcmp(host_public, join->host_public, P2P_KEY_SIZE) ||
		memcmp(host_nonce, join->host_nonce, NONCE_SIZE)))
	{
		return;
	}
	/* the machine asked: its key has the hash in the invite (or the one
	the game's host told) */
	p2p_key_hash(host_public, hash);
	if (memcmp(hash, join->host_hash, P2P_KEY_HASH_SIZE))
		return;
	if (!join->has_base || memcmp(join->host_public, host_public, P2P_KEY_SIZE))
	{
		if (!pair_base(p2p_public_key(), host_public, host_public, join->base))
			return;
		memcpy(join->host_public, host_public, P2P_KEY_SIZE);
		join->has_base = 1;
	}
	/* and the answer is its */
	if (!tag_right(join->base, "accept", message, size))
		return;
	if (get_addresses(message + fixed, size - TAG_SIZE - fixed, candidates, &count, &webrtc) !=
		size - TAG_SIZE - fixed)
	{
		return;
	}
	session_secret(join->base, nonce, host_nonce, secret);
	if (!p2p_peer_offered(join->host, host_public, secret, candidates, count, &webrtc,
		join == &signalling.joins[0] ? _p2p_offered_host : _p2p_offered_player) || join->answered)
	{
		return;
	}
	memcpy(join->host_nonce, host_nonce, NONCE_SIZE);
	join->answered = 1;
	/* the proof, at once */
	send_join(join);
	if (signalling.direct_key_set && signalling.direct_address)
	{
		unsigned char meet_key[32];
		unsigned long meet_address = signalling.direct_address;

		memcpy(meet_key, signalling.direct_key, 32);
		pthread_mutex_unlock(&p2p_lock);
		peer_mesh_introduce(meet_key, meet_address, 0);
		pthread_mutex_lock(&p2p_lock);
	}
}

void p2p_signal_set_direct(unsigned long address, unsigned short port,
	const unsigned char host_hash[16], const unsigned char key[32])
{
	int same;

	if (!(address && port && host_hash))
	{
		signalling.direct_set = 0;
		signalling.direct_address = 0;
		signalling.direct_port = 0;
		signalling.direct_key_set = 0;
		return;
	}
	/* a public address already in use stays. a private one learned later is the
	machine's own network, and it would aim the handshake at an unreachable host. */
	if (signalling.direct_set && !p2p_address_private(signalling.direct_address) &&
		p2p_address_private(address))
		return;
	same = signalling.direct_set && signalling.direct_address == address && signalling.direct_port == port;
	signalling.direct_set = 1;
	signalling.direct_address = address;
	signalling.direct_port = port;
	signalling.direct_key_set = key != NULL;
	memcpy(signalling.direct_hash, host_hash, P2P_KEY_HASH_SIZE);
	if (key)
		memcpy(signalling.direct_key, key, 32);
	if (!same && signalling.joins[0].active)
		signalling.joins[0].sent_time = 0;
}

int p2p_signal_direct_received(const unsigned char *packet, int size, unsigned long address, unsigned short port)
{
	int sealed_size;
	const unsigned char *sealed;
	unsigned char message[MAXIMUM_MESSAGE_SIZE];
	int message_size;
	int have_signature;

	if (packet && size >= 2 + 32 + 4 + 2 + 64 && packet[0] == 0x71 && packet[1] == 1)
	{
		const unsigned char *mesh_key = packet + 2;
		unsigned long claimed_address;
		unsigned short claimed_port;
		unsigned short use_port = 0;
		int from_joiner;
		int from_host;

		memcpy(&claimed_address, packet + 34, 4);
		memcpy(&claimed_port, packet + 38, 2);
		/* The address is the one this packet came from. The port is the mesh
		   socket's public port when it names that same address. */
		if (claimed_address == address && claimed_port)
			use_port = claimed_port;
		from_joiner = signalling.hosting && joiner_admitted(address) &&
			peer_mesh_verify(mesh_key, packet, 40, packet + 40);
		from_host = joins_active() && signalling.direct_key_set &&
			address == signalling.direct_address &&
			memcmp(mesh_key, signalling.direct_key, 32) == 0 &&
			peer_mesh_verify(signalling.direct_key, packet, 40, packet + 40);
		if (from_joiner || from_host)
		{
			unsigned char meet_key[32];
			unsigned long meet_address = address;

			memcpy(meet_key, mesh_key, 32);
			pthread_mutex_unlock(&p2p_lock);
			peer_mesh_introduce(meet_key, meet_address, use_port);
			pthread_mutex_lock(&p2p_lock);
		}
		return 1;
	}
	if (!packet || size < 6 || packet[0] != 0x70 || packet[1] != 1)
		return 0;
	sealed_size = (packet[2] << 8) | packet[3];
	if (sealed_size <= 0 || sealed_size > MAXIMUM_MESSAGE_SIZE + P2P_SEAL_OVERHEAD || 4 + sealed_size > size)
		return 1;
	sealed = packet + 4;
	have_signature = size >= 4 + sealed_size + 64;
	signalling.direct_reply_address = address;
	signalling.direct_reply_port = port;
	signalling.direct_reply_set = 1;
	if (signalling.hosting)
	{
		message_size = p2p_open(signalling.host_key, sealed, sealed_size, message);
		if (message_size >= 2 && message[0] == _message_join && message[1] == MESSAGE_VERSION)
			join_received(message, message_size);
	}
	if (joins_active())
	{
		int signed_ok = 1;

		if (signalling.direct_key_set)
			signed_ok = have_signature && peer_mesh_verify(signalling.direct_key, sealed, sealed_size,
				sealed + sealed_size);
		if (signed_ok)
		{
			message_size = p2p_open(signalling.join_key, sealed, sealed_size, message);
			if (message_size >= 2 && message[0] == _message_accept && message[1] == MESSAGE_VERSION)
				accept_received(message, message_size);
		}
	}
	signalling.direct_reply_set = 0;
	return 1;
}

void p2p_signal_start(void)
{
	signalling.started = 1;
}

void p2p_signal_select_sets(int *read, int *read_count, int *write, int *write_count, int maximum_count)
{
	(void)read;
	(void)read_count;
	(void)write;
	(void)write_count;
	(void)maximum_count;
}

void p2p_signal_update(const int *read, int read_count, const int *write, int write_count)
{
	int index;

	(void)read;
	(void)read_count;
	(void)write;
	(void)write_count;
	for (index = 0; index < MAXIMUM_JOINS; index++)
	{
		struct join *join = &signalling.joins[index];

		if (!join->active)
			continue;
		if (!join->answered && elapsed(join->nonce_time, UNANSWERED_TIME))
		{
			posix_random_bytes(join->nonce, NONCE_SIZE);
			join->nonce_time = p2p_now();
			send_join(join);
		}
		if (elapsed(join->sent_time, JOIN_INTERVAL))
			send_join(join);
	}
}

void p2p_signal_reconnect(void)
{
}

void p2p_signal_host(const unsigned char *token)
{
	if (!signalling.has_nonce_key)
	{
		posix_random_bytes(signalling.nonce_key, P2P_SHA256_SIZE);
		signalling.has_nonce_key = 1;
	}
	memcpy(signalling.host_token, token, P2P_TOKEN_SIZE);
	derive(token, "seal", NULL, signalling.host_key);
	signalling.hosting = 1;
}

void p2p_signal_stop_hosting(void)
{
	signalling.hosting = 0;
	memset(signalling.joiners, 0, sizeof(signalling.joiners));
	memset(signalling.askers, 0, sizeof(signalling.askers));
	memset(admitted_joiner, 0, sizeof(admitted_joiner));
}

void p2p_signal_join(const unsigned char *host_hash, const unsigned char *token)
{
	int index;

	/* (the requests to the last game's players end: they hold another token) */
	for (index = 1; index < MAXIMUM_JOINS; index++)
		signalling.joins[index].active = 0;
	memcpy(signalling.join_token, token, P2P_TOKEN_SIZE);
	derive(token, "seal", NULL, signalling.join_key);
	/* (new each time: the host makes one session of a request; a browser's
	connection for the last goes, unless a session took it up) */
	p2p_webrtc_new_request();
	join_start(&signalling.joins[0], host_hash, token);
}

void p2p_signal_stop_joining(void)
{
	signalling.joins[0].active = 0;
}

int p2p_signal_join_player(const unsigned char *player_hash, const unsigned char *token)
{
	struct join *free_join = NULL;
	int index;

	/* (another token's requests would be answered under another seal) */
	if (signalling.joins[0].active && memcmp(token, signalling.join_token, P2P_TOKEN_SIZE))
		return 0;
	for (index = 1; index < MAXIMUM_JOINS; index++)
	{
		struct join *join = &signalling.joins[index];

		if (join->active && !memcmp(join->host_hash, player_hash, P2P_KEY_HASH_SIZE))
			return 1;
		if (!join->active && !free_join)
			free_join = join;
	}
	if (!free_join)
		return 0;
	if (!joins_active() || memcmp(token, signalling.join_token, P2P_TOKEN_SIZE))
	{
		memcpy(signalling.join_token, token, P2P_TOKEN_SIZE);
		derive(token, "seal", NULL, signalling.join_key);
		for (index = 1; index < MAXIMUM_JOINS; index++)
			signalling.joins[index].active = 0;
	}
	join_start(free_join, player_hash, token);
	return 1;
}

void p2p_signal_stop_joining_player(const unsigned char *identifier)
{
	int index;

	for (index = 1; index < MAXIMUM_JOINS; index++)
	{
		if (signalling.joins[index].active && !memcmp(signalling.joins[index].host, identifier, P2P_IDENTIFIER_SIZE))
			signalling.joins[index].active = 0;
	}
}

int p2p_signal_probe(const unsigned char *host_hash, const unsigned char *token)
{
	(void)host_hash;
	(void)token;
	return 0;
}

void p2p_signal_stop_probe(const unsigned char *identifier)
{
	(void)identifier;
}

void p2p_signal_lobby_topics(int listed, int browsing)
{
	(void)listed;
	(void)browsing;
}

void p2p_signal_lobby_publish(const unsigned char *listing, int size, int closing)
{
	(void)listing;
	(void)size;
	(void)closing;
}

void p2p_signal_lobby_query(void)
{
}

void p2p_signal_lobby_quit(void)
{
}
