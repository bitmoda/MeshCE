/*
PEER_MESH.C

Clients find each other without a server browser. Each one listens on UDP
38952, says hello on the local network, and passes along the clients it has
already met. A saved list is tried again later, which is as far as this
reaches: two clients that are both behind routers still need a path between
them, and this does not add one.

Find Game gathers the clients looking for the same playlist, times the
round trip, and the best connection hosts. That client sends the invite it
already makes for a party. The others join it.

The reputation id is this install's Ed25519 key. The seed is sealed to the
machine's secure element when it has one: the Secure Enclave on a Mac, and
the TPM on Windows and Linux. A copied file does not open on another
machine. A new install is a new key. Peers check the Ed25519 signature of a
packet. They can also check a P-256 signature over the id. That check shows
the sender holds the P-256 key. A program can still create its own P-256 key
in ordinary memory.

A report is a signature from a player in the match. It counts when that
install also signed the scoreboard the mesh kept, and the votes are more
than half of the installs that scoreboard names. A key that was not in that
scoreboard cannot add a vote or stand in for a player. The same count
removes the player from the game. A vote that falls short is kept. Thirty
three of those votes ban the key, starting at the middle of the cooldown,
16 hours. The first ban from a majority waits 1 hour. Each further ban
doubles the wait, up to 7 days. During the wait the key is not matched.
After it, the key is matched only when the search is otherwise short, and
it does not host while anyone else can.

Ranks live in a list every client keeps. A rank changes when more than half
of the installs named on a scoreboard submit that same text. A client that
never sends does not hold the others up, and that match does not change the
missing client's rank. A shorter scoreboard does not stay once a longer one
reaches that count: the rank change from the shorter one is taken back. A
client whose scoreboard differs is banned. A client that already sent a
longer scoreboard is not.
*/

#include "platform.h"
#include "posix.h"
#include "p2p.h"
#include "p2p_internal.h"
#include "peer_mesh.h"
#include "peer_mesh_rules.h"
#include "matchmaking.h"
#include "mesh_service.h"
#include "rank_account.h"
#include "networking/network_game_protocol.h"
#include "halo_port_limits.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(HALO_MACOS_GUEST) || defined(HALO_WINDOWS) || defined(HALO_PEER_ELEMENT)
#define PEER_HAVE_ELEMENT 1
#endif

#ifdef PEER_HAVE_ELEMENT
int host_peer_wrap(const unsigned char *seed, int seed_size, unsigned char *out, int out_cap);
int host_peer_unwrap(const unsigned char *wrapped, int wrapped_size, unsigned char *seed, int seed_size);
int host_peer_public(unsigned char *out, int out_cap);
int host_peer_sign(const unsigned char *message, int message_size, unsigned char *out, int out_cap);
int host_peer_verify(const unsigned char *public_key, int public_size, const unsigned char *message,
	int message_size, const unsigned char *signature, int signature_size);
#endif
#ifdef HALO_WINDOWS
int host_peer_replace(const char *from, const char *to);
#endif

enum
{
	MESH_PORT = NETWORK_MESH_PORT,
	MESH_MAGIC0 = 'O',
	MESH_MAGIC1 = 'C',
	MESH_MAGIC2 = 'P',
	MESH_MAGIC3 = 'E',
	MESH_VERSION = 2,
	MESH_HELLO = 1,
	/* Key, port, bound flag, P-256 key, and signature. Shorter reads off the datagram. */
	MESH_HELLO_PAYLOAD = 182,
	MESH_PEERS = 2,
	MESH_FIND = 3,
	MESH_PING = 4,
	MESH_PONG = 5,
	MESH_SCORE = 6,
	MESH_OFFER = 7,
	MESH_REPORT = 8,
	MESH_RESULT = 9,
	MESH_TALLY = 10,
	MESH_MAX_PACKET = 1200,
	MESH_PEERS_MAX = 64,
	MESH_CANDIDATES = 8,
	MESH_GOSSIP = 8,
	MESH_REPORTS = 16,
	MESH_STRIKES = 512,
	MESH_APPLIED = 32,
	MESH_MACHINES = 128,
	MESH_STANDINGS = 128,
	MESH_POLLS = 4,
	MESH_VOTES = 8,
	MESH_TALLY_TEXT = 896,
	MESH_GATHER_MS = 3000,
	MESH_PING_MS = 1500,
	MESH_SCORE_MS = 1000,
	MESH_OFFER_MS = 15000,
	MESH_HELLO_MS = 3000,
	MESH_PHASE_IDLE = 0,
	MESH_PHASE_COOLDOWN = 1,
	MESH_PHASE_GATHER = 2,
	MESH_PHASE_PING = 3,
	MESH_PHASE_SCORE = 4,
	MESH_PHASE_OFFER = 5,
	MESH_PHASE_JOIN = 6,
	MESH_PHASE_DONE = 7
};

struct mesh_peer
{
	unsigned char key[32];
	unsigned long address;
	unsigned short port;
	unsigned bans;
	unsigned cooldown_until;
	unsigned last_seen;
	int heard;
	int used;
};

struct mesh_candidate
{
	unsigned char key[32];
	unsigned long address;
	unsigned short port;
	int bans;
	unsigned cooldown_until;
	unsigned nonce;
	unsigned sent_at;
	unsigned rtt[3];
	int rtt_count;
	int pings_sent;
	unsigned short score_median;
	int score_answered;
	int score_have;
	int level;
	int seats;
	char name[12];
	int used;
};

struct mesh_vote
{
	unsigned char key[32];
	char name[12];
	char text[MESH_TALLY_TEXT];
	unsigned char raw[1152];
	int raw_size;
	int used;
};

struct mesh_poll
{
	char match_id[40];
	struct mesh_vote votes[MESH_VOTES];
	char decided[MESH_TALLY_TEXT];
	int committed;
	unsigned char applied[MESH_VOTES][32];
	int applied_count;
	unsigned char outliers[MESH_VOTES][32];
	int outlier_count;
	/* installs named by the tally that was applied. a larger roster replaces it. */
	int applied_keys;
	int relays;
	unsigned quiet_at;
	int used;
};

struct mesh_report
{
	unsigned char voter[32];
	unsigned char target[32];
	unsigned char signature[64];
	int used;
};

struct mesh_strike
{
	unsigned char voter[32];
	unsigned char target[32];
	unsigned char match[16];
	unsigned char signature[64];
	/* 1 open, 2 already used for a ban, 3 waiting until the match tally is kept */
	int used;
};

struct mesh_applied
{
	unsigned char target[32];
	unsigned char match[16];
	int used;
};

static struct
{
	int started;
	int socket;
	int ready;
	int have_key;
	int bound;
	unsigned char seed[32];
	unsigned char public_key[32];
	unsigned char se_public[65];
	unsigned char se_signature[80];
	int se_public_len;
	int se_signature_len;
	struct mesh_peer peers[MESH_PEERS_MAX];
	int dirty;
	unsigned last_hello;
	unsigned last_offer;
	unsigned last_save;
	int searching;
	int phase;
	unsigned phase_at;
	unsigned search_at;
	int rank_extra;
	unsigned rank_extra_at;
	int account_level;
	char account_name[12];
	int seats;
	int retries;
	char playlist[16];
	struct mesh_candidate candidates[MESH_CANDIDATES];
	int we_host;
	/* 1 when someone else was already in this search at the decision.
	a later hello does not count as a player who has joined. */
	int grouped;
	unsigned char winner[32];
	unsigned char match_id[16];
	int have_match;
	char invite[160];
	int want_host_note;
	int want_join_note;
	unsigned self_bans;
	unsigned self_until;
	struct mesh_report reports[MESH_REPORTS];
	struct mesh_applied applied[MESH_APPLIED];
	unsigned char machine_key[MESH_MACHINES][32];
	int machine_have[MESH_MACHINES];
	unsigned char host_key[32];
	int host_have;
	unsigned char published_target[32];
	int published;
	char result_out[960];
	int result_send;
	int result_sends_left;
	unsigned result_sent_at;
	int result_unbound_used;
	unsigned char result_unbound_host[32];
	char result_in[960];
	int result_have;
	unsigned char result_host[32];
	unsigned char result_match[16];
	int result_bound;
	unsigned char result_log_host[4][32];
	unsigned char result_log_match[4][16];
	int result_log_used[4];
	int result_log_next;
	unsigned host_note_at;
	struct mesh_standing standings[MESH_STANDINGS];
	struct mesh_poll polls[MESH_POLLS];
	unsigned last_tally;
	int standings_ready;
	int sync_have;
	int sync_games;
	int sync_level;
	int sync_wins;
	int sync_losses;
	struct mesh_strike strikes[MESH_STRIKES];
	int strikes_ready;
	int strikes_dirty;
	unsigned char strike_packet[MESH_MAX_PACKET];
	int strike_packet_size;
	int strike_relays;
	unsigned strike_relay_at;
} mesh;

static pthread_mutex_t mesh_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned short net16(unsigned short value)
{
	return (unsigned short)((value << 8) | (value >> 8));
}

static unsigned short read16(const unsigned char *bytes)
{
	return (unsigned short)(bytes[0] | (bytes[1] << 8));
}

static void write16(unsigned char *bytes, unsigned short value)
{
	bytes[0] = (unsigned char)value;
	bytes[1] = (unsigned char)(value >> 8);
}

static unsigned read32(const unsigned char *bytes)
{
	return (unsigned)bytes[0] | ((unsigned)bytes[1] << 8) | ((unsigned)bytes[2] << 16) |
		((unsigned)bytes[3] << 24);
}

static void write32(unsigned char *bytes, unsigned value)
{
	bytes[0] = (unsigned char)value;
	bytes[1] = (unsigned char)(value >> 8);
	bytes[2] = (unsigned char)(value >> 16);
	bytes[3] = (unsigned char)(value >> 24);
}

static unsigned unix_now(void)
{
	time_t now = time(NULL);

	if (now < 0)
		return 0;
	return (unsigned)now;
}

static int same_key(const unsigned char *left, const unsigned char *right)
{
	return left && right && memcmp(left, right, 32) == 0;
}

static int sign_unlocked(const void *message, int size, unsigned char signature[64])
{
	if (!mesh.have_key || !message || size < 0 || !signature)
		return 0;
	p2p_ed25519_sign(mesh.seed, mesh.public_key, message, size, signature);
	return 1;
}

static int verify_key(const unsigned char *key, const void *message, int size, const unsigned char *signature)
{
	if (!key || !message || size < 0 || !signature)
		return 0;
	return p2p_ed25519_verify(key, message, size, signature);
}

static void identity_path(char *path, int size)
{
	snprintf(path, (size_t)size, "%s/peer-identity.bin", platform_save_root());
}

static void peers_path(char *path, int size)
{
	snprintf(path, (size_t)size, "%s/peers.dat", platform_save_root());
}

static void remember_self_record(void)
{
	int index;

	mesh.self_bans = 0;
	mesh.self_until = 0;
	for (index = 0; index < MESH_PEERS_MAX; index++)
	{
		if (!mesh.peers[index].used || !same_key(mesh.peers[index].key, mesh.public_key))
			continue;
		mesh.self_bans = mesh.peers[index].bans;
		mesh.self_until = mesh.peers[index].cooldown_until;
		return;
	}
}

static void load_peers(void)
{
	char path[512];
	FILE *file;
	unsigned char header[8];
	unsigned count;
	unsigned index;

	peers_path(path, (int)sizeof(path));
	file = fopen(path, "rb");
	if (!file)
		return;
	if (fread(header, 1, 8, file) != 8 || memcmp(header, "OPPR", 4) != 0 || header[4] != 1)
	{
		fclose(file);
		return;
	}
	count = (unsigned)header[5] | ((unsigned)header[6] << 8) | ((unsigned)header[7] << 16);
	if (count > MESH_PEERS_MAX)
		count = MESH_PEERS_MAX;
	for (index = 0; index < count; index++)
	{
		unsigned char row[48];
		struct mesh_peer *peer = &mesh.peers[index];

		if (fread(row, 1, sizeof(row), file) != sizeof(row))
			break;
		memset(peer, 0, sizeof(*peer));
		memcpy(peer->key, row, 32);
		peer->address = read32(row + 32);
		peer->port = read16(row + 36);
		peer->bans = read16(row + 38);
		peer->cooldown_until = read32(row + 40);
		peer->last_seen = read32(row + 44);
		peer->heard = 1;
		peer->used = 1;
	}
	fclose(file);
}

static void save_peers(void)
{
	char path[512];
	FILE *file;
	unsigned char header[8];
	int index;
	int count = 0;

	if (!mesh.dirty)
		return;
	peers_path(path, (int)sizeof(path));
	file = fopen(path, "wb");
	if (!file)
		return;
	for (index = 0; index < MESH_PEERS_MAX; index++)
	{
		if (mesh.peers[index].used)
			count++;
	}
	memset(header, 0, sizeof(header));
	memcpy(header, "OPPR", 4);
	header[4] = 1;
	header[5] = (unsigned char)count;
	header[6] = (unsigned char)(count >> 8);
	header[7] = (unsigned char)(count >> 16);
	if (fwrite(header, 1, 8, file) != 8)
	{
		fclose(file);
		return;
	}
	for (index = 0; index < MESH_PEERS_MAX; index++)
	{
		unsigned char row[48];
		struct mesh_peer *peer = &mesh.peers[index];

		if (!peer->used)
			continue;
		memset(row, 0, sizeof(row));
		memcpy(row, peer->key, 32);
		write32(row + 32, (unsigned)peer->address);
		write16(row + 36, peer->port);
		write16(row + 38, (unsigned short)peer->bans);
		write32(row + 40, peer->cooldown_until);
		write32(row + 44, peer->last_seen);
		if (fwrite(row, 1, sizeof(row), file) != sizeof(row))
			break;
	}
	fclose(file);
	mesh.dirty = 0;
}

static void strikes_path(char *path, int size)
{
	snprintf(path, (size_t)size, "%s/strikes.dat", platform_save_root());
}

static void load_strikes(void)
{
	char path[512];
	FILE *file;
	unsigned char header[8];
	unsigned count;
	unsigned index;

	if (mesh.strikes_ready)
		return;
	mesh.strikes_ready = 1;
	strikes_path(path, (int)sizeof(path));
	file = fopen(path, "rb");
	if (!file)
		return;
	if (fread(header, 1, 8, file) != 8 || memcmp(header, "OSTK", 4) != 0 || header[4] != 1)
	{
		fclose(file);
		return;
	}
	count = read16(header + 5);
	if (count > MESH_STRIKES)
		count = MESH_STRIKES;
	for (index = 0; index < count; index++)
	{
		unsigned char row[145];
		struct mesh_strike *strike = &mesh.strikes[index];

		if (fread(row, 1, sizeof(row), file) != sizeof(row))
			break;
		if (row[144] != 1 && row[144] != 2 && row[144] != 3)
			continue;
		memset(strike, 0, sizeof(*strike));
		memcpy(strike->voter, row, 32);
		memcpy(strike->target, row + 32, 32);
		memcpy(strike->match, row + 64, 16);
		memcpy(strike->signature, row + 80, 64);
		strike->used = row[144];
	}
	fclose(file);
}

static void save_strikes(void)
{
	char path[512];
	FILE *file;
	unsigned char header[8];
	int index;
	int count = 0;

	if (!mesh.strikes_dirty)
		return;
	strikes_path(path, (int)sizeof(path));
	file = fopen(path, "wb");
	if (!file)
		return;
	for (index = 0; index < MESH_STRIKES; index++)
	{
		if (mesh.strikes[index].used)
			count++;
	}
	memset(header, 0, sizeof(header));
	memcpy(header, "OSTK", 4);
	header[4] = 1;
	write16(header + 5, (unsigned short)count);
	if (fwrite(header, 1, 8, file) != 8)
	{
		fclose(file);
		return;
	}
	for (index = 0; index < MESH_STRIKES; index++)
	{
		unsigned char row[145];
		struct mesh_strike *strike = &mesh.strikes[index];

		if (!strike->used)
			continue;
		memset(row, 0, sizeof(row));
		memcpy(row, strike->voter, 32);
		memcpy(row + 32, strike->target, 32);
		memcpy(row + 64, strike->match, 16);
		memcpy(row + 80, strike->signature, 64);
		row[144] = (unsigned char)strike->used;
		if (fwrite(row, 1, sizeof(row), file) != sizeof(row))
		{
			fclose(file);
			return;
		}
	}
	fclose(file);
	mesh.strikes_dirty = 0;
}

static int write_identity_file(const char *path, int is_wrapped, const unsigned char *bytes, int size)
{
	char temporary[540];
	FILE *file;
	unsigned char header[8];
	int path_len;
	int wrote;

	if (!path || !bytes || size <= 0 || size > 65535)
		return 0;
	path_len = (int)strlen(path);
	if (path_len <= 0 || path_len > 500)
		return 0;
	memcpy(temporary, path, (size_t)path_len);
	memcpy(temporary + path_len, ".tmp", 5);
	file = fopen(temporary, "wb");
	if (!file)
		return 0;
	memset(header, 0, sizeof(header));
	memcpy(header, "OCID", 4);
	header[4] = 1;
	header[5] = is_wrapped ? 1 : 0;
	write16(header + 6, (unsigned short)size);
	wrote = fwrite(header, 1, 8, file) == 8 && fwrite(bytes, 1, (size_t)size, file) == (size_t)size;
	if (fclose(file) != 0)
		wrote = 0;
	if (!wrote)
	{
		remove(temporary);
		return 0;
	}
#ifdef HALO_WINDOWS
	if (!host_peer_replace(temporary, path))
#else
	if (rename(temporary, path) != 0)
#endif
	{
		remove(temporary);
		return 0;
	}
	return 1;
}

#ifdef PEER_HAVE_ELEMENT
/* Write the sealed blob only after this machine can open that same blob. */
static int element_seal(const unsigned char *seed, unsigned char *sealed, int cap)
{
	unsigned char check[32];
	int sealed_len;
	int ok;

	sealed_len = host_peer_wrap(seed, 32, sealed, cap);
	if (sealed_len <= 0)
		return 0;
	memset(check, 0, sizeof(check));
	ok = host_peer_unwrap(sealed, sealed_len, check, 32) && memcmp(check, seed, 32) == 0;
	memset(check, 0, sizeof(check));
	return ok ? sealed_len : 0;
}
#endif

static void load_identity(void)
{
	char path[512];
	FILE *file;
	unsigned char header[8];
	unsigned char blob[1200];
	unsigned length;
	unsigned char seed[32];
	int wrapped = 0;
	int have = 0;
	int blocked = 0;
	static int gave_up;

	if (mesh.have_key || gave_up)
		return;
	identity_path(path, (int)sizeof(path));
	file = fopen(path, "rb");
	if (!file)
	{
		if (errno != ENOENT)
			blocked = 1;
	}
	else
	{
		if (fread(header, 1, 8, file) == 8 && memcmp(header, "OCID", 4) == 0 && header[4] == 1)
		{
			length = read16(header + 6);
			wrapped = (header[5] & 1) != 0;
			if (length > 0 && length <= sizeof(blob) && fread(blob, 1, length, file) == length)
			{
#ifdef PEER_HAVE_ELEMENT
				if (wrapped && host_peer_unwrap(blob, (int)length, seed, 32))
					have = 1;
#endif
				if (!wrapped && length == 32)
				{
					memcpy(seed, blob, 32);
					have = 1;
				}
			}
		}
		fclose(file);
		if (!have)
			blocked = 1;
	}
	if (!have)
	{
		int sealed_len = 0;

		if (blocked)
		{
			gave_up = 1;
			platform_log("peer identity file could not be opened. It was left in place.");
			return;
		}
		posix_random_bytes(seed, 32);
		wrapped = 0;
#ifdef PEER_HAVE_ELEMENT
		sealed_len = element_seal(seed, blob, (int)sizeof(blob));
		if (sealed_len > 0 && write_identity_file(path, 1, blob, sealed_len))
			wrapped = 1;
#endif
		if (!wrapped)
			write_identity_file(path, 0, seed, 32);
		(void)sealed_len;
		have = 1;
	}
#ifdef PEER_HAVE_ELEMENT
	else if (!wrapped)
	{
		int sealed_len = element_seal(seed, blob, (int)sizeof(blob));

		if (sealed_len > 0 && write_identity_file(path, 1, blob, sealed_len))
			wrapped = 1;
	}
#endif
	memcpy(mesh.seed, seed, 32);
	p2p_ed25519_public(mesh.seed, mesh.public_key, NULL);
	mesh.have_key = 1;
	mesh.bound = 0;
#ifdef PEER_HAVE_ELEMENT
	mesh.se_public_len = host_peer_public(mesh.se_public, (int)sizeof(mesh.se_public));
	mesh.se_signature_len = 0;
	if (mesh.se_public_len == 65)
		mesh.se_signature_len = host_peer_sign(mesh.public_key, 32, mesh.se_signature,
			(int)sizeof(mesh.se_signature));
	/* A public key and a signature from two different primaries would not match. */
	if (mesh.se_signature_len <= 0 || !host_peer_verify(mesh.se_public, mesh.se_public_len,
		mesh.public_key, 32, mesh.se_signature, mesh.se_signature_len))
	{
		mesh.se_public_len = 0;
		mesh.se_signature_len = 0;
	}
	if (wrapped && mesh.se_public_len == 65 && mesh.se_signature_len > 0)
		mesh.bound = 1;
#else
	(void)wrapped;
#endif
	memset(seed, 0, sizeof(seed));
	load_peers();
	load_strikes();
	remember_self_record();
	platform_log("peer identity ready, secure element %s", mesh.bound ? "sealed this key" :
		(wrapped ? "sealed this key, signature missing" : "not used"));
}

static struct mesh_peer *peer_slot(const unsigned char *key, int create)
{
	int index;
	int free_slot = -1;
	int oldest = 0;
	unsigned oldest_seen = 0xffffffffu;

	for (index = 0; index < MESH_PEERS_MAX; index++)
	{
		if (!mesh.peers[index].used)
		{
			if (free_slot < 0)
				free_slot = index;
			continue;
		}
		if (same_key(mesh.peers[index].key, key))
			return &mesh.peers[index];
		if (mesh.peers[index].last_seen <= oldest_seen)
		{
			oldest_seen = mesh.peers[index].last_seen;
			oldest = index;
		}
	}
	if (!create)
		return NULL;
	if (free_slot < 0)
		free_slot = oldest;
	memset(&mesh.peers[free_slot], 0, sizeof(mesh.peers[free_slot]));
	memcpy(mesh.peers[free_slot].key, key, 32);
	mesh.peers[free_slot].used = 1;
	mesh.dirty = 1;
	return &mesh.peers[free_slot];
}

static void note_peer(const unsigned char *key, unsigned long address, unsigned short port, int direct)
{
	struct mesh_peer *peer;
	unsigned now = unix_now();
	int fresh;

	if (!key || same_key(key, mesh.public_key))
		return;
	peer = peer_slot(key, 1);
	if (!peer)
		return;
	/* A list passed along by someone else can name a stale address. An address
	this client heard itself stands for 30 seconds. After that, the list can
	move a peer that has gone quiet. */
	fresh = peer->heard && peer->last_seen && now - peer->last_seen < 30;
	if (direct || !fresh)
	{
		if (address && peer->address != address)
		{
			peer->address = address;
			mesh.dirty = 1;
		}
		if (port && peer->port != port)
		{
			peer->port = port;
			mesh.dirty = 1;
		}
	}
	if (!direct)
		return;
	peer->heard = 1;
	peer->last_seen = now;
	mesh.dirty = 1;
}

static int clean_count(void)
{
	int index;
	int count = mesh.self_bans == 0 ? 1 : 0;

	for (index = 0; index < MESH_CANDIDATES; index++)
	{
		if (mesh.candidates[index].used && mesh.candidates[index].bans <= 0)
			count++;
	}
	return count;
}

static struct mesh_candidate *candidate_slot(const unsigned char *key, int create)
{
	int index;

	for (index = 0; index < MESH_CANDIDATES; index++)
	{
		if (mesh.candidates[index].used && same_key(mesh.candidates[index].key, key))
			return &mesh.candidates[index];
	}
	if (!create)
		return NULL;
	for (index = 0; index < MESH_CANDIDATES; index++)
	{
		if (!mesh.candidates[index].used)
		{
			memset(&mesh.candidates[index], 0, sizeof(mesh.candidates[index]));
			memcpy(mesh.candidates[index].key, key, 32);
			mesh.candidates[index].used = 1;
			return &mesh.candidates[index];
		}
	}
	return NULL;
}

static void send_to(const unsigned char *packet, int size, unsigned long address, unsigned short port)
{
	struct sockaddr_in to;

	if (mesh.socket < 0 || size <= 0 || !address || !port)
		return;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = port;
	to.sin_addr.s_addr = address;
	posix_socket_sendto(mesh.socket, packet, size, 0, &to, (int)sizeof(to));
}

static void send_out(const unsigned char *packet, int size)
{
	unsigned long local;
	unsigned long broadcast;
	int index;

	send_to(packet, size, 0xffffffffu, net16(MESH_PORT));
	local = posix_local_ipv4_address();
	broadcast = local ? (local | 0xff000000u) : 0;
	if (broadcast && broadcast != local)
		send_to(packet, size, broadcast, net16(MESH_PORT));
	for (index = 0; index < MESH_PEERS_MAX; index++)
	{
		if (!mesh.peers[index].used || !mesh.peers[index].address || !mesh.peers[index].port)
			continue;
		send_to(packet, size, mesh.peers[index].address, mesh.peers[index].port);
	}
}

static int packet_finish(unsigned char *packet, int payload)
{
	int size = 8 + payload;

	if (!sign_unlocked(packet, size, packet + size))
		return 0;
	return size + 64;
}

static void send_hello(void)
{
	unsigned char packet[MESH_MAX_PACKET];
	int payload = MESH_HELLO_PAYLOAD;

	memset(packet, 0, sizeof(packet));
	packet[0] = MESH_MAGIC0;
	packet[1] = MESH_MAGIC1;
	packet[2] = MESH_MAGIC2;
	packet[3] = MESH_MAGIC3;
	packet[4] = MESH_VERSION;
	packet[5] = MESH_HELLO;
	write16(packet + 6, (unsigned short)payload);
	memcpy(packet + 8, mesh.public_key, 32);
	write16(packet + 40, MESH_PORT);
	packet[42] = (unsigned char)mesh.bound;
	packet[43] = (unsigned char)mesh.se_public_len;
	if (mesh.se_public_len > 0 && mesh.se_public_len <= 65)
		memcpy(packet + 44, mesh.se_public, (size_t)mesh.se_public_len);
	packet[109] = (unsigned char)mesh.se_signature_len;
	if (mesh.se_signature_len > 0 && mesh.se_signature_len <= 80)
		memcpy(packet + 110, mesh.se_signature, (size_t)mesh.se_signature_len);
	send_out(packet, packet_finish(packet, payload));
}

static void send_peers(void)
{
	unsigned char packet[MESH_MAX_PACKET];
	int count = 0;
	int index;
	int payload;

	memset(packet, 0, sizeof(packet));
	packet[0] = MESH_MAGIC0;
	packet[1] = MESH_MAGIC1;
	packet[2] = MESH_MAGIC2;
	packet[3] = MESH_MAGIC3;
	packet[4] = MESH_VERSION;
	packet[5] = MESH_PEERS;
	memcpy(packet + 8, mesh.public_key, 32);
	for (index = 0; index < MESH_PEERS_MAX && count < MESH_GOSSIP; index++)
	{
		unsigned char *row;
		struct mesh_peer *peer = &mesh.peers[index];

		if (!peer->used || !peer->address || !peer->port)
			continue;
		row = packet + 41 + count * 38;
		memcpy(row, peer->key, 32);
		write32(row + 32, (unsigned)peer->address);
		write16(row + 36, peer->port);
		count++;
	}
	packet[40] = (unsigned char)count;
	payload = 33 + count * 38;
	write16(packet + 6, (unsigned short)payload);
	send_out(packet, packet_finish(packet, payload));
}

static int our_rank_steps(unsigned now)
{
	int elapsed = (int)((now - mesh.search_at) / 1000);
	int steps = mesh_rank_steps(elapsed) + mesh.rank_extra;

	/* 363 is the last step a 16-bit age can describe. Past that, this
	client's window and the window it advertises would disagree. */
	if (steps > 363)
		steps = 363;
	return steps;
}

/* The advertised age makes the other client open the same ranked window, including a step taken early because nobody else was in range. */
static unsigned our_rank_age_ms(unsigned now)
{
	int steps = our_rank_steps(now);
	int seconds;

	if (steps <= 0)
		return now - mesh.search_at;
	seconds = 300 + (steps - 1) * 180;
	if (seconds < 0)
		seconds = 0;
	if (seconds > 65535)
		seconds = 65535;
	return (unsigned)seconds * 1000u;
}

static void send_find(void)
{
	unsigned char packet[256];
	int payload = 69;
	unsigned now = GetTickCount();

	memset(packet, 0, sizeof(packet));
	packet[0] = MESH_MAGIC0;
	packet[1] = MESH_MAGIC1;
	packet[2] = MESH_MAGIC2;
	packet[3] = MESH_MAGIC3;
	packet[4] = MESH_VERSION;
	packet[5] = MESH_FIND;
	write16(packet + 6, (unsigned short)payload);
	memcpy(packet + 8, mesh.public_key, 32);
	memcpy(packet + 40, mesh.playlist, 16);
	write16(packet + 56, (unsigned short)HALO_PORT_NETWORK_VERSION);
	write16(packet + 58, (unsigned short)(mesh.account_level < 0 ? 0 : mesh.account_level));
	packet[60] = (unsigned char)(mesh.seats > 0 && mesh.seats < 5 ? mesh.seats : 1);
	memcpy(packet + 61, mesh.account_name, 11);
	write32(packet + 73, !strcmp(mesh.playlist, "ranked") ? our_rank_age_ms(now) : now - mesh.search_at);
	send_out(packet, packet_finish(packet, payload));
}

static void send_score(void)
{
	unsigned char packet[128];
	int index;
	int answered = 0;
	unsigned short worst = 0;
	int payload = 35;

	for (index = 0; index < MESH_CANDIDATES; index++)
	{
		unsigned short sample;
		int count;

		if (!mesh.candidates[index].used || mesh.candidates[index].rtt_count <= 0)
			continue;
		count = mesh.candidates[index].rtt_count;
		sample = 0;
		{
			int sample_index;

			for (sample_index = 0; sample_index < count; sample_index++)
			{
				if (mesh.candidates[index].rtt[sample_index] > sample)
					sample = (unsigned short)mesh.candidates[index].rtt[sample_index];
			}
		}
		answered++;
		if (sample > worst)
			worst = sample;
	}
	memset(packet, 0, sizeof(packet));
	packet[0] = MESH_MAGIC0;
	packet[1] = MESH_MAGIC1;
	packet[2] = MESH_MAGIC2;
	packet[3] = MESH_MAGIC3;
	packet[4] = MESH_VERSION;
	packet[5] = MESH_SCORE;
	write16(packet + 6, (unsigned short)payload);
	memcpy(packet + 8, mesh.public_key, 32);
	write16(packet + 40, worst);
	packet[42] = (unsigned char)answered;
	send_out(packet, packet_finish(packet, payload));
}

static void send_offer(void)
{
	unsigned char packet[512];
	int invite_len;
	int payload = 259;
	char invite[160];

	if (!p2p_invite_link(invite, (int)sizeof(invite)) || !invite[0])
		return;
	invite_len = (int)strlen(invite);
	if (invite_len > 159)
		invite_len = 159;
	memset(packet, 0, sizeof(packet));
	packet[0] = MESH_MAGIC0;
	packet[1] = MESH_MAGIC1;
	packet[2] = MESH_MAGIC2;
	packet[3] = MESH_MAGIC3;
	packet[4] = MESH_VERSION;
	packet[5] = MESH_OFFER;
	write16(packet + 6, (unsigned short)payload);
	memcpy(packet + 8, mesh.public_key, 32);
	memcpy(packet + 40, mesh.playlist, 16);
	packet[58] = (unsigned char)invite_len;
	memcpy(packet + 59, invite, (size_t)invite_len);
	memset(packet + 219, 0, 32);
	{
		const char *map_name = mesh_map_for(mesh.match_id);

		memcpy(packet + 219, map_name, strlen(map_name));
	}
	memcpy(packet + 251, mesh.match_id, 16);
	{
		write16(packet + 267, (unsigned short)(mesh.account_level < 0 ? 0 : mesh.account_level));
		write16(packet + 269, (unsigned short)((!strcmp(mesh.playlist, "ranked") ?
			our_rank_age_ms(GetTickCount()) : GetTickCount() - mesh.search_at) / 1000));
		payload = 263;
		write16(packet + 6, (unsigned short)payload);
	}
	snprintf(mesh.invite, sizeof(mesh.invite), "%s", invite);
	send_out(packet, packet_finish(packet, payload));
}

static int strike_open(const unsigned char *target)
{
	int index;
	int count = 0;

	for (index = 0; index < MESH_STRIKES; index++)
	{
		if (mesh.strikes[index].used == 1 && same_key(mesh.strikes[index].target, target))
			count++;
	}
	return count;
}

static int strike_waiting(const unsigned char *target, const unsigned char *match)
{
	int index;
	int count = 0;

	for (index = 0; index < MESH_STRIKES; index++)
	{
		struct mesh_strike *strike = &mesh.strikes[index];

		if (strike->used == 3 && same_key(strike->target, target) && memcmp(strike->match, match, 16) == 0)
			count++;
	}
	return count;
}

static int strike_add(const unsigned char *voter, const unsigned char *target, const unsigned char *match,
	const unsigned char *signature)
{
	int index;
	int slot = -1;
	/* 0 empty, 2 consumed, 3 a crowded waiting group, 4 some other match. */
	int slot_kind = 99;

	if (!voter || !target || !match || !signature || same_key(voter, target))
		return 0;
	for (index = 0; index < MESH_STRIKES; index++)
	{
		struct mesh_strike *strike = &mesh.strikes[index];

		if (strike->used && same_key(strike->voter, voter) && same_key(strike->target, target) &&
			memcmp(strike->match, match, 16) == 0)
			return 0;
		if (!strike->used && slot_kind > 0)
		{
			slot = index;
			slot_kind = 0;
		}
		else if (strike->used == 2 && slot_kind > 2)
		{
			slot = index;
			slot_kind = 2;
		}
	}
	if (slot < 0)
	{
		for (index = 0; index < MESH_STRIKES; index++)
		{
			struct mesh_strike *strike = &mesh.strikes[index];

			if (strike->used != 3)
				continue;
			/* Past eight waiting votes for one player in one match, a further
			one is not a real roster. A different match gives its slot up too. */
			if (same_key(strike->target, target) && memcmp(strike->match, match, 16) == 0)
			{
				if (slot_kind > 3 && strike_waiting(target, match) > 8)
				{
					slot = index;
					slot_kind = 3;
				}
			}
			else if (slot_kind > 4)
			{
				slot = index;
				slot_kind = 4;
			}
		}
	}
	if (slot < 0)
		return 0;
	memset(&mesh.strikes[slot], 0, sizeof(mesh.strikes[slot]));
	memcpy(mesh.strikes[slot].voter, voter, 32);
	memcpy(mesh.strikes[slot].target, target, 32);
	memcpy(mesh.strikes[slot].match, match, 16);
	memcpy(mesh.strikes[slot].signature, signature, 64);
	/* Held until the match tally says this voter played. It does not count yet. */
	mesh.strikes[slot].used = 3;
	mesh.strikes_dirty = 1;
	return 1;
}

static void strike_consume_match(const unsigned char *target, const unsigned char *match)
{
	int index;

	for (index = 0; index < MESH_STRIKES; index++)
	{
		struct mesh_strike *strike = &mesh.strikes[index];

		if (strike->used != 1 || !same_key(strike->target, target) || memcmp(strike->match, match, 16) != 0)
			continue;
		strike->used = 2;
		mesh.strikes_dirty = 1;
	}
}

static void strike_apply(const unsigned char *target)
{
	struct mesh_peer *peer;
	unsigned now;
	int left;
	int index;

	left = peer_strike_limit();
	if (!peer_strikes_ban(strike_open(target)))
		return;
	for (index = 0; index < MESH_STRIKES && left > 0; index++)
	{
		if (mesh.strikes[index].used != 1 || !same_key(mesh.strikes[index].target, target))
			continue;
		mesh.strikes[index].used = 2;
		mesh.strikes_dirty = 1;
		left--;
	}
	if (left > 0)
		return;
	peer = peer_slot(target, 1);
	if (!peer)
		return;
	now = unix_now();
	peer->bans = (unsigned)peer_strike_ban_count((int)peer->bans);
	peer->cooldown_until = now + (unsigned)peer_cooldown_seconds((int)peer->bans);
	mesh.dirty = 1;
	if (same_key(target, mesh.public_key))
	{
		mesh.self_bans = peer->bans;
		mesh.self_until = peer->cooldown_until;
	}
	platform_log("peer banned from accumulated votes, cooldown %u seconds", peer->cooldown_until - now);
}

static int already_applied(const unsigned char *target, const unsigned char *match)
{
	int index;

	for (index = 0; index < MESH_APPLIED; index++)
	{
		if (!mesh.applied[index].used)
			continue;
		if (same_key(mesh.applied[index].target, target) &&
			memcmp(mesh.applied[index].match, match, 16) == 0)
			return 1;
	}
	return 0;
}

static void mark_applied(const unsigned char *target, const unsigned char *match)
{
	int index;

	for (index = 0; index < MESH_APPLIED; index++)
	{
		if (!mesh.applied[index].used)
		{
			memcpy(mesh.applied[index].target, target, 32);
			memcpy(mesh.applied[index].match, match, 16);
			mesh.applied[index].used = 1;
			return;
		}
	}
	memmove(&mesh.applied[0], &mesh.applied[1], sizeof(mesh.applied) - sizeof(mesh.applied[0]));
	memcpy(mesh.applied[MESH_APPLIED - 1].target, target, 32);
	memcpy(mesh.applied[MESH_APPLIED - 1].match, match, 16);
	mesh.applied[MESH_APPLIED - 1].used = 1;
}

static void apply_ban(const unsigned char *target)
{
	struct mesh_peer *peer = peer_slot(target, 1);
	unsigned now = unix_now();

	if (!peer)
		return;
	if (peer->bans < 1000)
		peer->bans++;
	peer->cooldown_until = now + (unsigned)peer_cooldown_seconds((int)peer->bans);
	mesh.dirty = 1;
	if (same_key(target, mesh.public_key))
	{
		mesh.self_bans = peer->bans;
		mesh.self_until = peer->cooldown_until;
	}
	platform_log("peer banned, cooldown %u seconds", peer->cooldown_until - now);
}

static void standings_path(char *path, int size)
{
	snprintf(path, (size_t)size, "%s/ranks.dat", platform_save_root());
}

static void load_standings(void)
{
	char path[512];
	FILE *file;
	unsigned char header[8];
	unsigned count;
	unsigned index;

	standings_path(path, (int)sizeof(path));
	file = fopen(path, "rb");
	if (!file)
		return;
	if (fread(header, 1, 8, file) != 8 || memcmp(header, "ORNK", 4) != 0 || header[4] != 1)
	{
		fclose(file);
		return;
	}
	count = (unsigned)header[5] | ((unsigned)header[6] << 8);
	if (count > MESH_STANDINGS)
		count = MESH_STANDINGS;
	for (index = 0; index < count; index++)
	{
		unsigned char row[64];
		struct mesh_standing *standing = &mesh.standings[index];

		if (fread(row, 1, sizeof(row), file) != sizeof(row))
			break;
		memset(standing, 0, sizeof(*standing));
		memcpy(standing->key, row, 32);
		memcpy(standing->name, row + 32, 11);
		standing->games = (int)read32(row + 44);
		standing->level = (int)read32(row + 48);
		standing->wins = (int)read32(row + 52);
		standing->losses = (int)read32(row + 56);
		standing->used = 1;
	}
	fclose(file);
}

static void save_standings(void)
{
	char path[512];
	FILE *file;
	unsigned char header[8];
	int index;
	int count = 0;

	standings_path(path, (int)sizeof(path));
	file = fopen(path, "wb");
	if (!file)
		return;
	for (index = 0; index < MESH_STANDINGS; index++)
	{
		if (mesh.standings[index].used)
			count++;
	}
	memset(header, 0, sizeof(header));
	memcpy(header, "ORNK", 4);
	header[4] = 1;
	header[5] = (unsigned char)count;
	header[6] = (unsigned char)(count >> 8);
	if (fwrite(header, 1, 8, file) != 8)
	{
		fclose(file);
		return;
	}
	for (index = 0; index < MESH_STANDINGS; index++)
	{
		unsigned char row[64];
		struct mesh_standing *standing = &mesh.standings[index];

		if (!standing->used)
			continue;
		memset(row, 0, sizeof(row));
		memcpy(row, standing->key, 32);
		memcpy(row + 32, standing->name, 11);
		write32(row + 44, (unsigned)(standing->games < 0 ? 0 : standing->games));
		write32(row + 48, (unsigned)(standing->level < 0 ? 0 : standing->level));
		write32(row + 52, (unsigned)(standing->wins < 0 ? 0 : standing->wins));
		write32(row + 56, (unsigned)(standing->losses < 0 ? 0 : standing->losses));
		if (fwrite(row, 1, sizeof(row), file) != sizeof(row))
			break;
	}
	fclose(file);
}

static int book_level(const unsigned char *key)
{
	int index;

	if (!key)
		return 0;
	for (index = 0; index < MESH_STANDINGS; index++)
	{
		if (!mesh.standings[index].used || !same_key(mesh.standings[index].key, key))
			continue;
		return mesh.standings[index].level < 0 ? 0 : mesh.standings[index].level;
	}
	return 0;
}

static void note_sync_ours(void)
{
	int index;

	mesh.sync_games = 0;
	mesh.sync_level = 0;
	mesh.sync_wins = 0;
	mesh.sync_losses = 0;
	mesh.sync_have = 1;
	if (!mesh.have_key)
		return;
	for (index = 0; index < MESH_STANDINGS; index++)
	{
		if (!mesh.standings[index].used || !same_key(mesh.standings[index].key, mesh.public_key))
			continue;
		mesh.sync_games = mesh.standings[index].games;
		mesh.sync_level = mesh.standings[index].level;
		mesh.sync_wins = mesh.standings[index].wins;
		mesh.sync_losses = mesh.standings[index].losses;
		return;
	}
}

static struct mesh_poll *poll_for(const char *match_id, int create)
{
	int index;
	int free_slot = -1;

	if (!match_id || !match_id[0])
		return NULL;
	for (index = 0; index < MESH_POLLS; index++)
	{
		if (!mesh.polls[index].used)
		{
			if (free_slot < 0)
				free_slot = index;
			continue;
		}
		if (!strcmp(mesh.polls[index].match_id, match_id))
			return &mesh.polls[index];
	}
	if (!create)
		return NULL;
	if (free_slot < 0)
		free_slot = 0;
	memset(&mesh.polls[free_slot], 0, sizeof(mesh.polls[free_slot]));
	snprintf(mesh.polls[free_slot].match_id, sizeof(mesh.polls[free_slot].match_id), "%s", match_id);
	mesh.polls[free_slot].used = 1;
	return &mesh.polls[free_slot];
}

static int roster_add(unsigned char keys[][32], int count, int cap, const unsigned char *key)
{
	int index;

	if (!key || count >= cap)
		return count;
	for (index = 0; index < count; index++)
	{
		if (same_key(keys[index], key))
			return count;
	}
	memcpy(keys[count], key, 32);
	return count + 1;
}

static int tally_supporters(struct mesh_poll *poll, const char *text)
{
	int index;
	int count = 0;

	for (index = 0; index < MESH_VOTES; index++)
	{
		if (!poll->votes[index].used || strcmp(poll->votes[index].text, text) != 0)
			continue;
		if (!mesh_tally_has_key(text, poll->votes[index].key))
			continue;
		count++;
	}
	return count;
}

/* 1 when some other submitted tally names this vote and is at least as large.
A shorter tally then has to wait. A stranger's tally does not hold up the match. */
static int tally_blocked(struct mesh_poll *poll, const char *text)
{
	int listed;
	int index;

	listed = mesh_tally_key_count(text);
	if (listed < 1)
		return 1;
	for (index = 0; index < MESH_VOTES; index++)
	{
		int other;

		if (!poll->votes[index].used || mesh_tally_has_key(text, poll->votes[index].key))
			continue;
		for (other = 0; other < MESH_VOTES; other++)
		{
			if (!poll->votes[other].used)
				continue;
			if (mesh_tally_key_count(poll->votes[other].text) < listed)
				continue;
			if (mesh_tally_has_key(poll->votes[other].text, poll->votes[index].key))
				return 1;
		}
	}
	return 0;
}

static int tally_applied(struct mesh_poll *poll, const unsigned char *key)
{
	int index;

	for (index = 0; index < poll->applied_count && index < MESH_VOTES; index++)
	{
		if (same_key(poll->applied[index], key))
			return 1;
	}
	return 0;
}

static int hex_nibble(char character)
{
	if (character >= '0' && character <= '9')
		return character - '0';
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;
	if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;
	return -1;
}

/* The tally match id is the hex form of the 16 byte mesh match id. */
static int tally_match_bytes(const char *text, unsigned char match[16])
{
	int index;

	if (!text || strncmp(text, "v1|", 3) != 0)
		return 0;
	for (index = 0; index < 16; index++)
	{
		int high = hex_nibble(text[3 + index * 2]);
		int low = hex_nibble(text[4 + index * 2]);

		if (high < 0 || low < 0)
			return 0;
		match[index] = (unsigned char)((high << 4) | low);
	}
	return text[35] == '|';
}

static struct mesh_standing *standing_by_key(const unsigned char *key)
{
	int index;

	if (!key)
		return NULL;
	for (index = 0; index < MESH_STANDINGS; index++)
	{
		if (mesh.standings[index].used && same_key(mesh.standings[index].key, key))
			return &mesh.standings[index];
	}
	return NULL;
}

static void standing_undo_match(const char *match_id)
{
	int index;

	if (!match_id || !match_id[0])
		return;
	for (index = 0; index < MESH_STANDINGS; index++)
	{
		struct mesh_standing *slot = &mesh.standings[index];

		if (!slot->used || !slot->undo_have || strcmp(slot->undo_match, match_id) != 0)
			continue;
		if (slot->undo_was_new)
			memset(slot, 0, sizeof(*slot));
		else
		{
			slot->games = slot->undo_games;
			slot->level = slot->undo_level;
			slot->wins = slot->undo_wins;
			slot->losses = slot->undo_losses;
			slot->undo_have = 0;
		}
	}
	note_sync_ours();
}

static void standing_snap(const unsigned char *key, const char *match_id, int was_new, int games, int level,
	int wins, int losses)
{
	struct mesh_standing *slot = standing_by_key(key);

	if (!slot)
		return;
	/* The first apply of this match keeps the numbers from before it. A later
	match may replace that, which ends the chance to take this one back. */
	if (slot->undo_have && match_id && strcmp(slot->undo_match, match_id) == 0)
		return;
	slot->undo_have = 1;
	slot->undo_was_new = was_new ? 1 : 0;
	snprintf(slot->undo_match, sizeof(slot->undo_match), "%s", match_id ? match_id : "");
	slot->undo_games = games;
	slot->undo_level = level;
	slot->undo_wins = wins;
	slot->undo_losses = losses;
}

static void report_settle(const unsigned char *target, const unsigned char *match);
static void report_settle_match(const unsigned char match[16]);

static void tally_ban(struct mesh_poll *poll, const unsigned char *key)
{
	int index;

	for (index = 0; index < poll->outlier_count && index < MESH_VOTES; index++)
	{
		if (same_key(poll->outliers[index], key))
			return;
	}
	if (poll->outlier_count < MESH_VOTES)
	{
		memcpy(poll->outliers[poll->outlier_count], key, 32);
		poll->outlier_count++;
	}
	apply_ban(key);
	platform_log("tally outlier banned");
}

static void tally_decide(struct mesh_poll *poll)
{
	const char *best = NULL;
	int best_keys = 0;
	int index;
	int tie = 0;
	unsigned now;
	unsigned char match_bytes[16];

	/* Give the rest of the match a moment to arrive before a tally is kept. */
	now = GetTickCount();
	if (!poll->quiet_at || now - poll->quiet_at < 3000)
		return;
	/* The largest roster with more than half agreeing is the match. A client
	that never sends does not hold that up. A shorter tally waits while a
	fuller one is present. */
	for (index = 0; index < MESH_VOTES; index++)
	{
		int listed;
		int votes;

		if (!poll->votes[index].used || tally_blocked(poll, poll->votes[index].text))
			continue;
		listed = mesh_tally_key_count(poll->votes[index].text);
		votes = tally_supporters(poll, poll->votes[index].text);
		if (!mesh_tally_majority(votes, listed) || listed <= best_keys)
			continue;
		best_keys = listed;
	}
	for (index = 0; index < MESH_VOTES; index++)
	{
		const char *text;
		int listed;
		int votes;

		if (!poll->votes[index].used)
			continue;
		text = poll->votes[index].text;
		listed = mesh_tally_key_count(text);
		if (listed != best_keys || tally_blocked(poll, text))
			continue;
		votes = tally_supporters(poll, text);
		if (!mesh_tally_majority(votes, listed))
			continue;
		if (!best)
			best = text;
		else if (strcmp(best, text) != 0)
			tie = 1;
	}
	if (!best || tie)
		return;
	/* A shorter tally can be kept before the rest arrive. The fuller one
	takes that rank change back. */
	if (poll->applied_keys > 0 && best_keys > poll->applied_keys)
	{
		standing_undo_match(poll->match_id);
		poll->applied_count = 0;
		poll->applied_keys = 0;
		poll->committed = 0;
		poll->decided[0] = 0;
		save_standings();
	}
	if (!poll->committed)
	{
		snprintf(poll->decided, sizeof(poll->decided), "%s", best);
		poll->committed = 1;
	}
	for (index = 0; index < MESH_VOTES; index++)
	{
		struct mesh_claim claim;
		struct mesh_standing *slot;
		int was_new = 1;
		int games = 0;
		int level = 0;
		int wins = 0;
		int losses = 0;

		if (!poll->votes[index].used || !mesh_tally_has_key(poll->decided, poll->votes[index].key))
			continue;
		if (strcmp(poll->votes[index].text, poll->decided) != 0)
		{
			/* A client already sending a fuller roster is not the outlier. */
			if (mesh_tally_key_count(poll->votes[index].text) <= mesh_tally_key_count(poll->decided))
				tally_ban(poll, poll->votes[index].key);
			continue;
		}
		if (tally_applied(poll, poll->votes[index].key) || poll->applied_count >= MESH_VOTES)
			continue;
		slot = standing_by_key(poll->votes[index].key);
		if (slot)
		{
			was_new = 0;
			games = slot->games;
			level = slot->level;
			wins = slot->wins;
			losses = slot->losses;
		}
		memset(&claim, 0, sizeof(claim));
		memcpy(claim.key, poll->votes[index].key, 32);
		snprintf(claim.name, sizeof(claim.name), "%s", poll->votes[index].name);
		if (!mesh_standings_commit(mesh.standings, MESH_STANDINGS, poll->decided, &claim, 1))
			continue;
		standing_snap(poll->votes[index].key, poll->match_id, was_new, games, level, wins, losses);
		memcpy(poll->applied[poll->applied_count], poll->votes[index].key, 32);
		poll->applied_count++;
		save_standings();
		if (same_key(poll->votes[index].key, mesh.public_key))
			note_sync_ours();
	}
	poll->applied_keys = best_keys;
	if (tally_match_bytes(poll->decided, match_bytes))
		report_settle_match(match_bytes);
}

static void tally_store(const unsigned char *key, const char *match_id, const char *name, const char *text,
	const unsigned char *raw, int raw_size)
{
	struct mesh_poll *poll;
	struct mesh_vote *vote = NULL;
	int index;
	char expect[48];

	if (!key || !match_id || !match_id[0] || !text || !mesh_tally_humans(text))
		return;
	snprintf(expect, sizeof(expect), "v1|%s|", match_id);
	if (strncmp(text, expect, strlen(expect)) != 0)
		return;
	poll = poll_for(match_id, 1);
	if (!poll)
		return;
	for (index = 0; index < MESH_VOTES; index++)
	{
		if (poll->votes[index].used && same_key(poll->votes[index].key, key))
		{
			vote = &poll->votes[index];
			break;
		}
	}
	if (vote && !strcmp(vote->text, text))
	{
		tally_decide(poll);
		return;
	}
	if (!vote)
	{
		for (index = 0; index < MESH_VOTES; index++)
		{
			if (!poll->votes[index].used)
			{
				vote = &poll->votes[index];
				break;
			}
		}
	}
	if (!vote)
		return;
	memset(vote, 0, sizeof(*vote));
	memcpy(vote->key, key, 32);
	if (name)
		snprintf(vote->name, sizeof(vote->name), "%s", name);
	snprintf(vote->text, sizeof(vote->text), "%s", text);
	if (raw && raw_size > 0 && raw_size <= (int)sizeof(vote->raw))
	{
		memcpy(vote->raw, raw, (size_t)raw_size);
		vote->raw_size = raw_size;
	}
	vote->used = 1;
	poll->relays = 8;
	poll->quiet_at = GetTickCount();
	if (!poll->quiet_at)
		poll->quiet_at = 1;
	tally_decide(poll);
}

static void relay_tallies(void)
{
	int poll_index;
	int vote_index;

	for (poll_index = 0; poll_index < MESH_POLLS; poll_index++)
	{
		struct mesh_poll *poll = &mesh.polls[poll_index];

		if (!poll->used || poll->relays <= 0)
			continue;
		for (vote_index = 0; vote_index < MESH_VOTES; vote_index++)
		{
			if (!poll->votes[vote_index].used || poll->votes[vote_index].raw_size <= 0)
				continue;
			send_out(poll->votes[vote_index].raw, poll->votes[vote_index].raw_size);
		}
		poll->relays--;
	}
}

static void tally_review(void)
{
	int index;

	for (index = 0; index < MESH_POLLS; index++)
	{
		if (mesh.polls[index].used)
			tally_decide(&mesh.polls[index]);
	}
}

/* text, target, match, and the host key. The host key stops another client
from repeating the same votes under its own signature as a smaller game. */
static int report_message(unsigned char *message, const unsigned char *target, const unsigned char *match,
	const unsigned char *host)
{
	memset(message, 0, 96);
	memcpy(message, "OpenCE report v1", 16);
	if (target)
		memcpy(message + 16, target, 32);
	if (match)
		memcpy(message + 48, match, 16);
	if (host)
		memcpy(message + 64, host, 32);
	return 96;
}

/* 1 when this install signed the tally that was kept. */
static int tally_signed(struct mesh_poll *poll, const unsigned char *key)
{
	int index;

	if (!poll || !key || !poll->committed || !mesh_tally_has_key(poll->decided, key))
		return 0;
	for (index = 0; index < MESH_VOTES; index++)
	{
		if (!poll->votes[index].used || !same_key(poll->votes[index].key, key))
			continue;
		return strcmp(poll->votes[index].text, poll->decided) == 0;
	}
	return 0;
}

static struct mesh_poll *committed_poll(const unsigned char match[16])
{
	int index;

	if (!match)
		return NULL;
	for (index = 0; index < MESH_POLLS; index++)
	{
		unsigned char parsed[16];

		if (!mesh.polls[index].used || !mesh.polls[index].committed)
			continue;
		if (!tally_match_bytes(mesh.polls[index].decided, parsed))
			continue;
		if (memcmp(parsed, match, 16) == 0)
			return &mesh.polls[index];
	}
	return NULL;
}

/* A vote counts only after the tally is kept, and only from an install that
signed that tally. The player count in the packet is not used. */
static void report_settle(const unsigned char *target, const unsigned char *match)
{
	struct mesh_poll *poll;
	int listed;
	int good = 0;
	int index;

	if (!target || !match)
		return;
	poll = committed_poll(match);
	if (!poll || !tally_signed(poll, target))
		return;
	listed = mesh_tally_key_count(poll->decided);
	for (index = 0; index < MESH_STRIKES; index++)
	{
		struct mesh_strike *strike = &mesh.strikes[index];

		if ((strike->used != 1 && strike->used != 3) || !same_key(strike->target, target) ||
			memcmp(strike->match, match, 16) != 0)
			continue;
		if (!tally_signed(poll, strike->voter))
			continue;
		good++;
	}
	if (peer_report_accepted(good, listed))
	{
		if (!already_applied(target, match))
		{
			mark_applied(target, match);
			apply_ban(target);
		}
		for (index = 0; index < MESH_STRIKES; index++)
		{
			struct mesh_strike *strike = &mesh.strikes[index];

			if ((strike->used != 1 && strike->used != 3) || !same_key(strike->target, target) ||
				memcmp(strike->match, match, 16) != 0)
				continue;
			if (!tally_signed(poll, strike->voter))
				continue;
			strike->used = 2;
			mesh.strikes_dirty = 1;
		}
		if (mesh.dirty)
			save_peers();
		if (mesh.strikes_dirty)
			save_strikes();
		return;
	}
	for (index = 0; index < MESH_STRIKES; index++)
	{
		struct mesh_strike *strike = &mesh.strikes[index];

		if (strike->used != 3 || !same_key(strike->target, target) || memcmp(strike->match, match, 16) != 0)
			continue;
		if (!tally_signed(poll, strike->voter))
			continue;
		strike->used = 1;
		mesh.strikes_dirty = 1;
	}
	while (peer_strikes_ban(strike_open(target)))
		strike_apply(target);
	if (mesh.dirty)
		save_peers();
	if (mesh.strikes_dirty)
		save_strikes();
}

static void report_settle_match(const unsigned char match[16])
{
	unsigned char seen[16][32];
	int seen_count = 0;
	int index;

	if (!match)
		return;
	for (index = 0; index < MESH_STRIKES; index++)
	{
		int earlier;
		int copy = 1;

		if (mesh.strikes[index].used != 1 && mesh.strikes[index].used != 3)
			continue;
		if (memcmp(mesh.strikes[index].match, match, 16) != 0)
			continue;
		for (earlier = 0; earlier < seen_count; earlier++)
		{
			if (same_key(seen[earlier], mesh.strikes[index].target))
				copy = 0;
		}
		if (!copy || seen_count >= 16)
			continue;
		memcpy(seen[seen_count], mesh.strikes[index].target, 32);
		seen_count++;
	}
	for (index = 0; index < seen_count; index++)
		report_settle(seen[index], match);
}

/* The publisher key is the first 32 bytes. The host key the voters signed is
the next field after the vote count. A packet from the host can carry every
vote. A packet from a voter counts only that voter's own signature. */
static int handle_report(const unsigned char *payload, int payload_len)
{
	unsigned char message[96];
	const unsigned char *target;
	const unsigned char *match;
	const unsigned char *host;
	int votes;
	int index;
	int good = 0;
	int publisher_is_host;

	if (payload_len < 114 + 96)
		return 0;
	target = payload + 32;
	match = payload + 64;
	votes = payload[81];
	host = payload + 82;
	if (votes < 1 || votes > 8 || payload_len < 114 + votes * 96)
		return 0;
	publisher_is_host = same_key(payload, host);
	report_message(message, target, match, host);
	for (index = 0; index < votes; index++)
	{
		const unsigned char *voter = payload + 114 + index * 96;
		const unsigned char *signature = voter + 32;

		if (!verify_key(voter, message, 96, signature))
			continue;
		/* A voter can publish their own vote. They cannot add anyone else's. */
		if (!publisher_is_host && !same_key(voter, payload))
			continue;
		strike_add(voter, target, match, signature);
		good++;
	}
	if (good < 1)
		return 0;
	report_settle(target, match);
	return 3;
}

static void remember_result(const unsigned char *host, const unsigned char *match)
{
	int index;
	int slot;

	if (!host || !match)
		return;
	for (index = 0; index < 4; index++)
	{
		if (mesh.result_log_used[index] && same_key(mesh.result_log_host[index], host) &&
			memcmp(mesh.result_log_match[index], match, 16) == 0)
			return;
	}
	slot = mesh.result_log_next % 4;
	memcpy(mesh.result_log_host[slot], host, 32);
	memcpy(mesh.result_log_match[slot], match, 16);
	mesh.result_log_used[slot] = 1;
	mesh.result_log_next++;
	memcpy(mesh.result_host, host, 32);
	memcpy(mesh.result_match, match, 16);
	mesh.result_bound = 1;
}

static int result_names_match(const unsigned char *host, const char *json)
{
	char expect[33];
	const char *mark;
	int index;
	int slot;

	if (!host || !json)
		return 0;
	mark = strstr(json, "\"match\":\"");
	if (!mark)
		return 0;
	mark += 9;
	for (slot = 0; slot < 4; slot++)
	{
		if (!mesh.result_log_used[slot] || !same_key(mesh.result_log_host[slot], host))
			continue;
		for (index = 0; index < 16; index++)
			snprintf(expect + index * 2, 3, "%02x", mesh.result_log_match[slot][index]);
		if (strncmp(mark, expect, 32) == 0 && mark[32] == '"')
			return 1;
	}
	return 0;
}

/* A private game has no search binding. The host of this game may publish
one result. The next game clears that, in peer_mesh_stop. */
static int result_from_game_host(const unsigned char *key, const char *json)
{
	const char *mark;

	if (!key || !json || !mesh.host_have || !same_key(key, mesh.host_key))
		return 0;
	if (mesh.result_unbound_used && same_key(mesh.result_unbound_host, key))
		return 0;
	mark = strstr(json, "\"match\":\"");
	if (!mark || !mark[9] || mark[9] == '"')
		return 0;
	memcpy(mesh.result_unbound_host, key, 32);
	mesh.result_unbound_used = 1;
	return 1;
}

static void handle_packet(const unsigned char *packet, int size, unsigned long address, unsigned short port)
{
	int payload;
	const unsigned char *body;
	const unsigned char *key;
	unsigned now;
	struct mesh_peer *known;

	if (size < 8 + 64 || packet[0] != MESH_MAGIC0 || packet[1] != MESH_MAGIC1 || packet[2] != MESH_MAGIC2 ||
		packet[3] != MESH_MAGIC3 || packet[4] != MESH_VERSION)
		return;
	payload = read16(packet + 6);
	if (payload < 32 || 8 + payload + 64 > size)
		return;
	body = packet + 8;
	key = body;
	if (!verify_key(key, packet, 8 + payload, packet + 8 + payload))
		return;
	if (same_key(key, mesh.public_key))
		return;
	note_peer(key, address, port, 1);
	known = peer_slot(key, 0);
	now = unix_now();
	switch (packet[5])
	{
	case MESH_HELLO:
#ifdef PEER_HAVE_ELEMENT
		if (known && payload >= MESH_HELLO_PAYLOAD && body[35] == 65 && body[101] > 0 &&
			body[101] <= 80 && host_peer_verify(body + 36, 65, key, 32, body + 102, body[101]))
			known->last_seen = now;
#else
		(void)known;
		(void)now;
#endif
		break;
	case MESH_PEERS:
	{
		int count = body[32];
		int index;

		if (count > MESH_GOSSIP || payload < 33 + count * 38)
			break;
		for (index = 0; index < count; index++)
		{
			const unsigned char *row = body + 33 + index * 38;

			note_peer(row, read32(row + 32), read16(row + 36), 0);
		}
		break;
	}
	case MESH_FIND:
		if (!mesh.searching || payload < 69 || memcmp(body + 32, mesh.playlist, 16) != 0)
			break;
		if (read16(body + 48) != HALO_PORT_NETWORK_VERSION)
			break;
		if (mesh.phase != MESH_PHASE_GATHER && mesh.phase != MESH_PHASE_PING && mesh.phase != MESH_PHASE_SCORE &&
			mesh.phase != MESH_PHASE_OFFER && mesh.phase != MESH_PHASE_JOIN)
			break;
		if (!strcmp(mesh.playlist, "ranked"))
		{
			int mine;
			int theirs;

			mine = our_rank_steps(GetTickCount());
			theirs = mesh_rank_steps((int)(read32(body + 65) / 1000));
			if (!mesh_ranked_together(mesh.account_level, mine, book_level(key), theirs))
				break;
		}
		{
			struct mesh_peer *peer = peer_slot(key, 0);
			int bans = peer ? (int)peer->bans : 0;
			unsigned until = peer ? peer->cooldown_until : 0;
			struct mesh_candidate *candidate;
			int fresh;

			if (!peer_include_in_search(bans, until, unix_now(), clean_count(),
				(int)(GetTickCount() - mesh.search_at)))
				break;
			candidate = candidate_slot(key, 0);
			fresh = candidate == NULL;
			if (fresh)
				candidate = candidate_slot(key, 1);
			if (!candidate)
				break;
			candidate->address = address;
			candidate->port = port;
			candidate->bans = bans;
			candidate->cooldown_until = until;
			candidate->level = book_level(key);
			candidate->seats = body[52] > 0 && body[52] <= 4 ? body[52] : 1;
			memset(candidate->name, 0, sizeof(candidate->name));
			memcpy(candidate->name, body + 53, 11);
			if (fresh && mesh.phase == MESH_PHASE_OFFER)
				mesh.want_host_note = 1;
		}
		break;
	case MESH_PING:
		if (payload < 36)
			break;
		{
			unsigned char reply[128];
			int reply_payload = 36;

			memset(reply, 0, sizeof(reply));
			reply[0] = MESH_MAGIC0;
			reply[1] = MESH_MAGIC1;
			reply[2] = MESH_MAGIC2;
			reply[3] = MESH_MAGIC3;
			reply[4] = MESH_VERSION;
			reply[5] = MESH_PONG;
			write16(reply + 6, (unsigned short)reply_payload);
			memcpy(reply + 8, mesh.public_key, 32);
			memcpy(reply + 40, body + 32, 4);
			send_to(reply, packet_finish(reply, reply_payload), address, port);
		}
		break;
	case MESH_PONG:
	{
		struct mesh_candidate *candidate = candidate_slot(key, 0);
		unsigned nonce;

		if (!candidate || payload < 36)
			break;
		nonce = read32(body + 32);
		if (nonce != candidate->nonce || candidate->rtt_count >= 3 || !candidate->sent_at)
			break;
		candidate->rtt[candidate->rtt_count++] = GetTickCount() - candidate->sent_at;
		break;
	}
	case MESH_SCORE:
	{
		struct mesh_candidate *candidate = candidate_slot(key, 0);

		if (!candidate || payload < 35)
			break;
		candidate->score_median = read16(body + 32);
		candidate->score_answered = body[34];
		candidate->score_have = 1;
		break;
	}
	case MESH_OFFER:
		if (!mesh.searching || mesh.phase == MESH_PHASE_DONE || payload < 259)
			break;
		if (memcmp(body + 32, mesh.playlist, 16) != 0)
			break;
		/* Once a host has been chosen, a different client's offer is not this game. */
		if (mesh.phase == MESH_PHASE_JOIN && !same_key(key, mesh.winner))
			break;
		if (mesh.phase == MESH_PHASE_OFFER)
		{
			/* Someone found before the decision is already in this search.
			A client who hears them only after deciding is still alone and can move. */
			if (mesh.grouped)
				break;
			if (!mesh_solo_yields(!strcmp(mesh.playlist, "ranked"), mesh.account_level,
				book_level(key), memcmp(key, mesh.public_key, 32) < 0))
				break;
		}
		if (!strcmp(mesh.playlist, "ranked") && payload >= 263)
		{
			int mine;
			int theirs;

			mine = our_rank_steps(GetTickCount());
			theirs = mesh_rank_steps((int)read16(body + 261));
			if (!mesh_ranked_together(mesh.account_level, mine, book_level(key), theirs))
				break;
		}
		if (mesh.phase != MESH_PHASE_JOIN && mesh.phase != MESH_PHASE_GATHER && mesh.phase != MESH_PHASE_PING &&
			mesh.phase != MESH_PHASE_SCORE && mesh.phase != MESH_PHASE_OFFER)
			break;
		{
			int invite_len = body[50];

			if (invite_len <= 0 || invite_len > 159)
				break;
			memcpy(mesh.winner, key, 32);
			mesh.we_host = 0;
			memcpy(mesh.invite, body + 51, (size_t)invite_len);
			mesh.invite[invite_len] = 0;
			memcpy(mesh.match_id, body + 243, 16);
			mesh.have_match = 1;
			remember_result(key, mesh.match_id);
			memcpy(mesh.playlist, body + 32, 16);
			mesh.playlist[15] = 0;
			mesh.want_join_note = 1;
			mesh.phase = MESH_PHASE_DONE;
		}
		break;
	case MESH_REPORT:
		handle_report(body, payload);
		break;
	case MESH_RESULT:
	{
		char incoming[960];
		int length;

		if (payload < 33 || payload > (int)sizeof(mesh.result_in))
			break;
		length = payload - 32;
		memcpy(incoming, body + 32, (size_t)length);
		incoming[length] = 0;
		/* A rejected copy must not replace a result already waiting. */
		if (!result_names_match(key, incoming) && !result_from_game_host(key, incoming))
			break;
		memcpy(mesh.result_in, incoming, (size_t)length + 1);
		mesh.result_have = 1;
		break;
	}
	case MESH_TALLY:
	{
		char match_id[40];
		char name[12];
		char text[MESH_TALLY_TEXT];
		int text_len;

		if (payload < 86)
			break;
		text_len = (int)read16(body + 84);
		if (text_len <= 0 || text_len >= MESH_TALLY_TEXT || payload < 86 + text_len)
			break;
		memset(match_id, 0, sizeof(match_id));
		memcpy(match_id, body + 32, 39);
		memset(name, 0, sizeof(name));
		memcpy(name, body + 72, 11);
		memcpy(text, body + 86, (size_t)text_len);
		text[text_len] = 0;
		tally_store(key, match_id, name, text, packet, size);
		break;
	}
	default:
		break;
	}
}

static void receive_all(void)
{
	int guard;

	for (guard = 0; guard < 32; guard++)
	{
		unsigned char packet[MESH_MAX_PACKET];
		struct sockaddr_in from;
		int from_len = (int)sizeof(from);
		int got;

		memset(&from, 0, sizeof(from));
		got = posix_socket_recvfrom(mesh.socket, packet, (int)sizeof(packet), 0, &from, &from_len);
		if (got < 8 + 64)
			return;
		handle_packet(packet, got, from.sin_addr.s_addr, from.sin_port);
	}
}

static void open_socket(void)
{
	struct sockaddr_in address;
	int one = 1;
	int sock;

	if (mesh.socket >= 0)
		return;
	sock = posix_socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		return;
	posix_socket_setsockopt(sock, 0xffff, 0x0004, &one, (int)sizeof(one));
	posix_socket_setsockopt(sock, 0xffff, 0x0020, &one, (int)sizeof(one));
	posix_socket_set_nonblocking(sock, 1);
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = net16(MESH_PORT);
	address.sin_addr.s_addr = 0;
	if (posix_socket_bind(sock, &address, (int)sizeof(address)) != 0)
	{
		posix_socket_close(sock);
		mesh.ready = 0;
		return;
	}
	mesh.socket = sock;
	mesh.ready = 1;
	platform_log("peer mesh listening on udp %d", MESH_PORT);
}

/* the slowest answered round trip. the client with the lowest of these
hosts, which is the best connection across the clients it reached. */
static unsigned short our_worst(int *answered)
{
	unsigned short worst = 0;
	int index;

	*answered = 0;
	for (index = 0; index < MESH_CANDIDATES; index++)
	{
		unsigned short sample;
		int n;

		if (!mesh.candidates[index].used)
			continue;
		n = mesh.candidates[index].rtt_count;
		if (n <= 0)
			continue;
		sample = 0;
		{
			int sample_index;

			for (sample_index = 0; sample_index < n; sample_index++)
			{
				if (mesh.candidates[index].rtt[sample_index] > sample)
					sample = (unsigned short)mesh.candidates[index].rtt[sample_index];
			}
		}
		(*answered)++;
		if (sample > worst)
			worst = sample;
	}
	return worst;
}

static void decide(void)
{
	unsigned char best_key[32];
	unsigned short best_median;
	int best_answered;
	int best_bans;
	int have = 0;
	int found = 0;
	int index;
	int answered = 0;
	unsigned short mine;

	mine = our_worst(&answered);
	memset(best_key, 0, sizeof(best_key));
	/* A client in cooldown does not host. Leaving it in the comparison made
	the search wait for an offer this machine will never send. */
	if (!peer_in_cooldown(mesh.self_until, unix_now()))
	{
		memcpy(best_key, mesh.public_key, 32);
		best_median = mine;
		best_answered = answered;
		best_bans = (int)mesh.self_bans;
		have = 1;
	}
	for (index = 0; index < MESH_CANDIDATES; index++)
	{
		struct mesh_candidate *candidate = &mesh.candidates[index];
		unsigned short median;
		int answers;
		int bans;

		if (!candidate->used)
			continue;
		found = 1;
		/* A client that never answered a ping does not get to name its own
		round trip. One that did cannot claim to be faster than the ping we saw. */
		{
			unsigned short measured = 0;
			int sample;

			for (sample = 0; sample < candidate->rtt_count; sample++)
			{
				if (candidate->rtt[sample] > measured)
					measured = (unsigned short)candidate->rtt[sample];
			}
			if (candidate->rtt_count <= 0)
			{
				median = (unsigned short)65535;
				answers = 0;
			}
			else if (!candidate->score_have)
			{
				median = measured;
				answers = 1;
			}
			else
			{
				median = candidate->score_median;
				if (median < measured)
					median = measured;
				answers = candidate->score_answered;
				if (answers > MESH_CANDIDATES)
					answers = MESH_CANDIDATES;
				if (answers < 0)
					answers = 0;
			}
		}
		bans = candidate->bans;
		if (!have || peer_hosts_over(median, answers, bans, candidate->key, best_median, best_answered, best_bans,
			best_key))
		{
			memcpy(best_key, candidate->key, 32);
			best_median = median;
			best_answered = answers;
			best_bans = bans;
			have = 1;
		}
	}
	mesh.grouped = found;
	if (!have)
	{
		mesh.we_host = 0;
		mesh.phase = MESH_PHASE_COOLDOWN;
		mesh.phase_at = GetTickCount();
		platform_log("peer mesh: cooldown, no other host");
		return;
	}
	if (same_key(best_key, mesh.public_key))
	{
		posix_random_bytes(mesh.match_id, 16);
		mesh.have_match = 1;
		mesh.we_host = 1;
		mesh.want_host_note = 1;
		mesh.host_note_at = GetTickCount();
		remember_result(mesh.public_key, mesh.match_id);
		mesh.phase = MESH_PHASE_OFFER;
		mesh.phase_at = GetTickCount();
		platform_log("peer mesh: this client hosts %s", mesh.playlist);
		return;
	}
	memcpy(mesh.winner, best_key, 32);
	mesh.we_host = 0;
	mesh.phase = MESH_PHASE_JOIN;
	mesh.phase_at = GetTickCount();
	platform_log("peer mesh: waiting for the host");
}

static void search_tick(void)
{
	unsigned now = GetTickCount();
	int index;

	if (!mesh.searching)
		return;
	/* Widen only after the first gather. Expanding during those seconds
	would drop a high lobby before the other party has time to answer. */
	if (!strcmp(mesh.playlist, "ranked") &&
		(mesh.phase == MESH_PHASE_OFFER || mesh.phase == MESH_PHASE_JOIN))
	{
		int others = 0;
		int seats = mesh.seats > 0 ? mesh.seats : 1;
		int index;

		for (index = 0; index < MESH_CANDIDATES; index++)
		{
			if (!mesh.candidates[index].used)
				continue;
			seats += mesh.candidates[index].seats > 0 ? mesh.candidates[index].seats : 1;
			others = 1;
		}
		if (!others && seats < 8 && mesh.rank_extra < 363 &&
			(!mesh.rank_extra_at || now - mesh.rank_extra_at >= 1000))
		{
			mesh.rank_extra++;
			mesh.rank_extra_at = now;
		}
	}
	if (mesh.phase == MESH_PHASE_COOLDOWN)
	{
		if (!peer_in_cooldown(mesh.self_until, unix_now()))
		{
			mesh.phase = MESH_PHASE_GATHER;
			mesh.phase_at = now;
		}
		return;
	}
	if (mesh.phase == MESH_PHASE_GATHER)
	{
		send_find();
		if (now - mesh.phase_at >= MESH_GATHER_MS)
		{
			mesh.phase = MESH_PHASE_PING;
			mesh.phase_at = now;
		}
	}
	if (mesh.phase == MESH_PHASE_PING)
	{
		for (index = 0; index < MESH_CANDIDATES; index++)
		{
			struct mesh_candidate *candidate = &mesh.candidates[index];
			unsigned char packet[128];

			if (!candidate->used || candidate->pings_sent >= 3)
				continue;
			if (candidate->sent_at && now - candidate->sent_at < 400)
				continue;
			posix_random_bytes(&candidate->nonce, 4);
			candidate->sent_at = now;
			candidate->pings_sent++;
			memset(packet, 0, sizeof(packet));
			packet[0] = MESH_MAGIC0;
			packet[1] = MESH_MAGIC1;
			packet[2] = MESH_MAGIC2;
			packet[3] = MESH_MAGIC3;
			packet[4] = MESH_VERSION;
			packet[5] = MESH_PING;
			write16(packet + 6, 36);
			memcpy(packet + 8, mesh.public_key, 32);
			write32(packet + 40, candidate->nonce);
			send_to(packet, packet_finish(packet, 36), candidate->address, candidate->port);
		}
		if (now - mesh.phase_at >= MESH_PING_MS)
		{
			send_score();
			mesh.phase = MESH_PHASE_SCORE;
			mesh.phase_at = now;
		}
	}
	if (mesh.phase == MESH_PHASE_SCORE && now - mesh.phase_at >= MESH_SCORE_MS)
		decide();
	if (mesh.phase == MESH_PHASE_OFFER)
	{
		if (!mesh.last_offer || now - mesh.last_offer >= 500)
		{
			send_offer();
			mesh.last_offer = now;
		}
		/* The lobby's start time follows the search. A full lobby, or a social
		search that has been open for 100 seconds, has to reach the host. */
		if (mesh.we_host && (!mesh.host_note_at || now - mesh.host_note_at >= 1000))
		{
			mesh.want_host_note = 1;
			mesh.host_note_at = now;
		}
	}
	if (mesh.phase == MESH_PHASE_JOIN && now - mesh.phase_at >= 120000)
	{
		mesh.retries++;
		if (mesh.retries > 1)
			mesh.phase = MESH_PHASE_DONE;
		else
		{
			memset(mesh.candidates, 0, sizeof(mesh.candidates));
			mesh.phase = MESH_PHASE_GATHER;
			mesh.phase_at = now;
		}
	}
}

static void send_pending_result(unsigned now)
{
	unsigned char packet[1024];
	int length;
	int payload;

	if (!mesh.result_send || !mesh.result_out[0] || mesh.result_sends_left <= 0)
		return;
	if (mesh.result_sent_at && now - mesh.result_sent_at < 1000)
		return;
	length = (int)strlen(mesh.result_out);
	if (length <= 0 || length >= 900)
	{
		mesh.result_send = 0;
		return;
	}
	payload = 32 + length;
	memset(packet, 0, sizeof(packet));
	packet[0] = MESH_MAGIC0;
	packet[1] = MESH_MAGIC1;
	packet[2] = MESH_MAGIC2;
	packet[3] = MESH_MAGIC3;
	packet[4] = MESH_VERSION;
	packet[5] = MESH_RESULT;
	write16(packet + 6, (unsigned short)payload);
	memcpy(packet + 8, mesh.public_key, 32);
	memcpy(packet + 40, mesh.result_out, (size_t)length);
	{
		int sent = packet_finish(packet, payload);

		if (sent <= 0)
		{
			mesh.result_sent_at = now ? now : 1;
			return;
		}
		send_out(packet, sent);
	}
	mesh.result_sends_left--;
	mesh.result_sent_at = now ? now : 1;
	if (mesh.result_sends_left <= 0)
		mesh.result_send = 0;
}

static void mesh_tick_locked(void)
{
	unsigned now;

	load_identity();
	if (!mesh.standings_ready)
	{
		load_standings();
		mesh.standings_ready = 1;
		note_sync_ours();
	}
	if (mesh.have_key)
		mesh.account_level = book_level(mesh.public_key);
	open_socket();
	if (!mesh.ready)
		return;
	receive_all();
	now = GetTickCount();
	if (!mesh.last_hello || now - mesh.last_hello >= MESH_HELLO_MS)
	{
		send_hello();
		send_peers();
		mesh.last_hello = now;
	}
	search_tick();
	send_pending_result(now);
	if (!mesh.last_tally || now - mesh.last_tally >= 1000)
	{
		relay_tallies();
		mesh.last_tally = now;
	}
	tally_review();
	if (mesh.strike_relays > 0 && mesh.strike_packet_size > 0 &&
		(!mesh.strike_relay_at || now - mesh.strike_relay_at >= 1000))
	{
		send_out(mesh.strike_packet, mesh.strike_packet_size);
		mesh.strike_relays--;
		mesh.strike_relay_at = now ? now : 1;
	}
	if (!mesh.last_save || now - mesh.last_save >= 30000)
	{
		save_peers();
		save_strikes();
		mesh.last_save = now;
	}
}

static void *mesh_thread(void *unused)
{
	(void)unused;
	for (;;)
	{
		int host = 0;
		int join = 0;
		int sync_have = 0;
		int sync_games = 0;
		int sync_level = 0;
		int sync_wins = 0;
		int sync_losses = 0;
		char playlist[16];
		char invite[160];
		unsigned char match[16];

		{
			struct rank_account account;

			rank_account_get(&account);
			pthread_mutex_lock(&mesh_lock);
			snprintf(mesh.account_name, sizeof(mesh.account_name), "%s", account.name);
			mesh_tick_locked();
			if (mesh.have_key && !mesh.sync_have)
			{
				int games = 0;
				int level = 0;
				int wins = 0;
				int losses = 0;
				int index;
				int found = 0;

				for (index = 0; index < MESH_STANDINGS; index++)
				{
					if (!mesh.standings[index].used || !same_key(mesh.standings[index].key, mesh.public_key))
						continue;
					games = mesh.standings[index].games;
					level = mesh.standings[index].level;
					wins = mesh.standings[index].wins;
					losses = mesh.standings[index].losses;
					found = 1;
					break;
				}
				if (found)
				{
					if (games != account.games || level != account.level || wins != account.wins ||
						losses != account.losses)
						note_sync_ours();
				}
				else if (account.games || account.level || account.wins || account.losses)
					note_sync_ours();
			}
		}
		if (mesh.have_key)
			mesh.account_level = book_level(mesh.public_key);
		if (mesh.want_host_note)
		{
			mesh.want_host_note = 0;
			host = 1;
			memcpy(playlist, mesh.playlist, sizeof(playlist));
			memcpy(match, mesh.match_id, sizeof(match));
		}
		if (mesh.want_join_note)
		{
			mesh.want_join_note = 0;
			join = 1;
			memcpy(playlist, mesh.playlist, sizeof(playlist));
			memcpy(invite, mesh.invite, sizeof(invite));
			memcpy(match, mesh.match_id, sizeof(match));
		}
		if (mesh.sync_have)
		{
			sync_have = 1;
			sync_games = mesh.sync_games;
			sync_level = mesh.sync_level;
			sync_wins = mesh.sync_wins;
			sync_losses = mesh.sync_losses;
			mesh.sync_have = 0;
		}
		pthread_mutex_unlock(&mesh_lock);
		if (sync_have)
			rank_mesh_progress(sync_games, sync_level, sync_wins, sync_losses);
		if (host)
		{
			char text[40];
			int index;

			for (index = 0; index < 16; index++)
				snprintf(text + index * 2, 3, "%02x", match[index]);
			matchmaking_note_mesh(1, playlist, 0, "", text);
		}
		if (join)
		{
			char text[40];
			int index;

			for (index = 0; index < 16; index++)
				snprintf(text + index * 2, 3, "%02x", match[index]);
			matchmaking_note_mesh(0, playlist, 0, invite, text);
		}
		mesh_account_serve();
		posix_socket_select(NULL, NULL, NULL, NULL, NULL, NULL, 0, 200000, 0);
	}
	return NULL;
}

void peer_mesh_start(void)
{
	pthread_t thread;

	pthread_mutex_lock(&mesh_lock);
	if (mesh.started)
	{
		pthread_mutex_unlock(&mesh_lock);
		return;
	}
	mesh.started = 1;
	mesh.socket = -1;
	if (pthread_create(&thread, NULL, mesh_thread, NULL) != 0)
		mesh.started = 0;
	else
		pthread_detach(thread);
	pthread_mutex_unlock(&mesh_lock);
}

void peer_mesh_search(const char *playlist, int seats)
{
	if (!playlist || !playlist[0])
		return;
	if (seats < 1)
		seats = 1;
	if (seats > 4)
		seats = 4;
	peer_mesh_start();
	pthread_mutex_lock(&mesh_lock);
	load_identity();
	memset(mesh.candidates, 0, sizeof(mesh.candidates));
	memset(mesh.winner, 0, sizeof(mesh.winner));
	mesh.searching = 1;
	mesh.we_host = 0;
	mesh.retries = 0;
	mesh.want_host_note = 0;
	mesh.want_join_note = 0;
	mesh.published = 0;
	mesh.grouped = 0;
	memset(mesh.reports, 0, sizeof(mesh.reports));
	mesh.seats = seats;
	snprintf(mesh.playlist, sizeof(mesh.playlist), "%s", playlist);
	mesh.search_at = GetTickCount();
	mesh.phase_at = mesh.search_at;
	mesh.rank_extra = 0;
	mesh.rank_extra_at = mesh.search_at ? mesh.search_at : 1;
	if (peer_in_cooldown(mesh.self_until, unix_now()))
		mesh.phase = MESH_PHASE_COOLDOWN;
	else
		mesh.phase = MESH_PHASE_GATHER;
	pthread_mutex_unlock(&mesh_lock);
}

void peer_mesh_stop(void)
{
	pthread_mutex_lock(&mesh_lock);
	mesh.searching = 0;
	mesh.phase = MESH_PHASE_IDLE;
	mesh.want_host_note = 0;
	mesh.want_join_note = 0;
	mesh.result_unbound_used = 0;
	pthread_mutex_unlock(&mesh_lock);
}

void peer_mesh_clear_game(void)
{
	pthread_mutex_lock(&mesh_lock);
	memset(mesh.reports, 0, sizeof(mesh.reports));
	memset(mesh.machine_key, 0, sizeof(mesh.machine_key));
	memset(mesh.machine_have, 0, sizeof(mesh.machine_have));
	memset(mesh.host_key, 0, sizeof(mesh.host_key));
	mesh.host_have = 0;
	mesh.published = 0;
	pthread_mutex_unlock(&mesh_lock);
}

int peer_mesh_ready(void)
{
	int ready;

	pthread_mutex_lock(&mesh_lock);
	ready = mesh.ready;
	pthread_mutex_unlock(&mesh_lock);
	return ready;
}

int peer_mesh_searching(void)
{
	int searching;

	pthread_mutex_lock(&mesh_lock);
	searching = mesh.searching;
	pthread_mutex_unlock(&mesh_lock);
	return searching;
}

int peer_mesh_party_size(void)
{
	int count = 1;
	int index;

	pthread_mutex_lock(&mesh_lock);
	if (mesh.searching)
	{
		count = 0;
		for (index = 0; index < MESH_CANDIDATES; index++)
		{
			if (!mesh.candidates[index].used)
				continue;
			count += mesh.candidates[index].seats > 0 ? mesh.candidates[index].seats : 1;
		}
		count += mesh.seats > 0 ? mesh.seats : 1;
	}
	pthread_mutex_unlock(&mesh_lock);
	return count;
}

int peer_mesh_party_names(char names[][12], int max)
{
	int count = 0;
	int index;

	if (!names || max < 1)
		return 0;
	pthread_mutex_lock(&mesh_lock);
	for (index = 0; index < MESH_CANDIDATES && count < max; index++)
	{
		if (!mesh.candidates[index].used)
			continue;
		snprintf(names[count], 12, "%s", mesh.candidates[index].name);
		count++;
	}
	pthread_mutex_unlock(&mesh_lock);
	return count;
}

int peer_mesh_party_for_key(const unsigned char key[32])
{
	int party = 1;
	int found = -1;
	int index;

	if (!key)
		return -1;
	pthread_mutex_lock(&mesh_lock);
	if (mesh.have_key && same_key(key, mesh.public_key))
	{
		pthread_mutex_unlock(&mesh_lock);
		return 0;
	}
	for (index = 0; index < MESH_CANDIDATES; index++)
	{
		if (!mesh.candidates[index].used)
			continue;
		if (same_key(mesh.candidates[index].key, key))
		{
			found = party;
			break;
		}
		party++;
	}
	pthread_mutex_unlock(&mesh_lock);
	return found;
}

int peer_mesh_search_seconds(void)
{
	int seconds = 0;

	pthread_mutex_lock(&mesh_lock);
	if (mesh.searching)
		seconds = (int)((GetTickCount() - mesh.search_at) / 1000);
	pthread_mutex_unlock(&mesh_lock);
	return seconds;
}

void peer_mesh_publish_result(const char *body)
{
	if (!body || !body[0])
		return;
	pthread_mutex_lock(&mesh_lock);
	snprintf(mesh.result_out, sizeof(mesh.result_out), "%s", body);
	mesh.result_sends_left = 5;
	mesh.result_sent_at = 0;
	mesh.result_send = 1;
	pthread_mutex_unlock(&mesh_lock);
}

int peer_mesh_roster(unsigned char keys[][32], int max)
{
	int count = 0;
	int index;

	if (!keys || max < 1)
		return 0;
	pthread_mutex_lock(&mesh_lock);
	if (mesh.have_key)
		count = roster_add(keys, count, max, mesh.public_key);
	if (mesh.host_have)
		count = roster_add(keys, count, max, mesh.host_key);
	for (index = 0; index < MESH_MACHINES && count < max; index++)
	{
		if (mesh.machine_have[index])
			count = roster_add(keys, count, max, mesh.machine_key[index]);
	}
	pthread_mutex_unlock(&mesh_lock);
	return count;
}

int peer_mesh_submit_tally(const char *match_id, const char *name, const char *tally)
{
	unsigned char packet[MESH_MAX_PACKET];
	int text_len;
	int payload;
	int sent;

	if (!match_id || !match_id[0] || !tally)
		return 0;
	text_len = (int)strlen(tally);
	if (text_len <= 0 || text_len >= MESH_TALLY_TEXT)
		return 0;
	payload = 86 + text_len;
	if (8 + payload + 64 > MESH_MAX_PACKET)
		return 0;
	pthread_mutex_lock(&mesh_lock);
	load_identity();
	if (!mesh.have_key)
	{
		pthread_mutex_unlock(&mesh_lock);
		return 0;
	}
	memset(packet, 0, sizeof(packet));
	packet[0] = MESH_MAGIC0;
	packet[1] = MESH_MAGIC1;
	packet[2] = MESH_MAGIC2;
	packet[3] = MESH_MAGIC3;
	packet[4] = MESH_VERSION;
	packet[5] = MESH_TALLY;
	write16(packet + 6, (unsigned short)payload);
	memcpy(packet + 8, mesh.public_key, 32);
	snprintf((char *)(packet + 40), 40, "%s", match_id);
	if (name)
		snprintf((char *)(packet + 80), 12, "%s", name);
	write16(packet + 92, (unsigned short)text_len);
	memcpy(packet + 94, tally, (size_t)text_len);
	sent = packet_finish(packet, payload);
	if (sent > 0)
	{
		tally_store(mesh.public_key, match_id, name ? name : "", tally, packet, sent);
		send_out(packet, sent);
	}
	pthread_mutex_unlock(&mesh_lock);
	return sent > 0;
}

int peer_mesh_take_result(char *body, int size)
{
	int have = 0;

	if (!body || size <= 1)
		return 0;
	pthread_mutex_lock(&mesh_lock);
	if (mesh.result_have)
	{
		snprintf(body, (size_t)size, "%s", mesh.result_in);
		mesh.result_have = 0;
		have = 1;
	}
	pthread_mutex_unlock(&mesh_lock);
	return have;
}

int peer_mesh_public(unsigned char key[32])
{
	int have;

	if (!key)
		return 0;
	pthread_mutex_lock(&mesh_lock);
	load_identity();
	have = mesh.have_key;
	if (have)
		memcpy(key, mesh.public_key, 32);
	pthread_mutex_unlock(&mesh_lock);
	return have;
}

int peer_mesh_sign(const void *message, int size, unsigned char signature[64])
{
	int signed_ok;

	pthread_mutex_lock(&mesh_lock);
	load_identity();
	signed_ok = sign_unlocked(message, size, signature);
	pthread_mutex_unlock(&mesh_lock);
	return signed_ok;
}

int peer_mesh_verify(const unsigned char key[32], const void *message, int size, const unsigned char signature[64])
{
	return verify_key(key, message, size, signature);
}

int peer_mesh_match(unsigned char match[16])
{
	int have;

	if (!match)
		return 0;
	pthread_mutex_lock(&mesh_lock);
	have = mesh.have_match;
	if (have)
		memcpy(match, mesh.match_id, 16);
	else
		memset(match, 0, 16);
	pthread_mutex_unlock(&mesh_lock);
	return have;
}

void peer_mesh_note_machine(int machine, const unsigned char key[32])
{
	if (!key)
		return;
	pthread_mutex_lock(&mesh_lock);
	if (machine < 0)
	{
		memcpy(mesh.host_key, key, 32);
		mesh.host_have = 1;
	}
	else if (machine < MESH_MACHINES)
	{
		memcpy(mesh.machine_key[machine], key, 32);
		mesh.machine_have[machine] = 1;
	}
	pthread_mutex_unlock(&mesh_lock);
}

int peer_mesh_machine_key(int machine, unsigned char key[32])
{
	int have = 0;

	if (!key)
		return 0;
	pthread_mutex_lock(&mesh_lock);
	if (machine < 0)
	{
		have = mesh.host_have;
		if (have)
			memcpy(key, mesh.host_key, 32);
	}
	else if (machine < MESH_MACHINES && mesh.machine_have[machine])
	{
		have = 1;
		memcpy(key, mesh.machine_key[machine], 32);
	}
	pthread_mutex_unlock(&mesh_lock);
	return have;
}

void peer_mesh_add_report(const unsigned char voter[32], const unsigned char target[32],
	const unsigned char signature[64])
{
	unsigned char message[96];
	unsigned char match[16];
	int index;

	if (!voter || !target || !signature)
		return;
	pthread_mutex_lock(&mesh_lock);
	if (!mesh.have_key)
		load_identity();
	memset(match, 0, sizeof(match));
	if (mesh.have_match)
		memcpy(match, mesh.match_id, 16);
	report_message(message, target, match, mesh.have_key ? mesh.public_key : NULL);
	if (!mesh.have_key || !verify_key(voter, message, 96, signature))
	{
		pthread_mutex_unlock(&mesh_lock);
		return;
	}
	for (index = 0; index < MESH_REPORTS; index++)
	{
		if (!mesh.reports[index].used || !same_key(mesh.reports[index].voter, voter) ||
			!same_key(mesh.reports[index].target, target))
			continue;
		memcpy(mesh.reports[index].signature, signature, 64);
		pthread_mutex_unlock(&mesh_lock);
		return;
	}
	for (index = 0; index < MESH_REPORTS; index++)
	{
		if (mesh.reports[index].used)
			continue;
		memcpy(mesh.reports[index].voter, voter, 32);
		memcpy(mesh.reports[index].target, target, 32);
		memcpy(mesh.reports[index].signature, signature, 64);
		mesh.reports[index].used = 1;
		pthread_mutex_unlock(&mesh_lock);
		return;
	}
	memmove(&mesh.reports[0], &mesh.reports[1], sizeof(mesh.reports) - sizeof(mesh.reports[0]));
	memcpy(mesh.reports[MESH_REPORTS - 1].voter, voter, 32);
	memcpy(mesh.reports[MESH_REPORTS - 1].target, target, 32);
	memcpy(mesh.reports[MESH_REPORTS - 1].signature, signature, 64);
	mesh.reports[MESH_REPORTS - 1].used = 1;
	pthread_mutex_unlock(&mesh_lock);
}

int peer_mesh_report_count(const unsigned char target[32])
{
	unsigned char message[96];
	unsigned char match[16];
	int index;
	int count = 0;

	if (!target)
		return 0;
	pthread_mutex_lock(&mesh_lock);
	if (!mesh.have_key)
		load_identity();
	memset(match, 0, sizeof(match));
	if (mesh.have_match)
		memcpy(match, mesh.match_id, 16);
	report_message(message, target, match, mesh.have_key ? mesh.public_key : NULL);
	for (index = 0; index < MESH_REPORTS && mesh.have_key; index++)
	{
		if (!mesh.reports[index].used || !same_key(mesh.reports[index].target, target))
			continue;
		if (verify_key(mesh.reports[index].voter, message, 96, mesh.reports[index].signature))
			count++;
	}
	pthread_mutex_unlock(&mesh_lock);
	return count;
}

static int fill_report_packet(unsigned char *packet, const unsigned char *target, int players, int skip, int limit)
{
	int index;
	int seen = 0;
	int kept = 0;
	int payload;

	memset(packet, 0, MESH_MAX_PACKET);
	packet[0] = MESH_MAGIC0;
	packet[1] = MESH_MAGIC1;
	packet[2] = MESH_MAGIC2;
	packet[3] = MESH_MAGIC3;
	packet[4] = MESH_VERSION;
	packet[5] = MESH_REPORT;
	memcpy(packet + 8, mesh.public_key, 32);
	memcpy(packet + 40, target, 32);
	if (mesh.have_match)
		memcpy(packet + 72, mesh.match_id, 16);
	if (players < 0)
		players = 0;
	if (players > 255)
		players = 255;
	packet[88] = (unsigned char)players;
	/* The host key is the one the voters signed. Receivers do not trust the player count. */
	memcpy(packet + 90, mesh.public_key, 32);
	for (index = 0; index < MESH_REPORTS && kept < 8 && kept < limit; index++)
	{
		unsigned char *row;

		if (!mesh.reports[index].used || !same_key(mesh.reports[index].target, target))
			continue;
		if (seen < skip)
		{
			seen++;
			continue;
		}
		row = packet + 122 + kept * 96;
		memcpy(row, mesh.reports[index].voter, 32);
		memcpy(row + 32, mesh.reports[index].signature, 64);
		kept++;
	}
	packet[89] = (unsigned char)kept;
	payload = 114 + kept * 96;
	write16(packet + 6, (unsigned short)payload);
	return kept;
}

void peer_mesh_cast_vote(const unsigned char target[32], const unsigned char host[32],
	const unsigned char signature[64])
{
	unsigned char packet[MESH_MAX_PACKET];
	int payload = 114 + 96;
	int size = 0;

	if (!target || !host || !signature)
		return;
	pthread_mutex_lock(&mesh_lock);
	if (!mesh.have_key)
		load_identity();
	if (!mesh.have_key || !mesh.have_match)
	{
		pthread_mutex_unlock(&mesh_lock);
		return;
	}
	memset(packet, 0, sizeof(packet));
	packet[0] = MESH_MAGIC0;
	packet[1] = MESH_MAGIC1;
	packet[2] = MESH_MAGIC2;
	packet[3] = MESH_MAGIC3;
	packet[4] = MESH_VERSION;
	packet[5] = MESH_REPORT;
	write16(packet + 6, (unsigned short)payload);
	memcpy(packet + 8, mesh.public_key, 32);
	memcpy(packet + 40, target, 32);
	memcpy(packet + 72, mesh.match_id, 16);
	packet[89] = 1;
	memcpy(packet + 90, host, 32);
	memcpy(packet + 122, mesh.public_key, 32);
	memcpy(packet + 154, signature, 64);
	if (handle_report(packet + 8, payload) > 0)
		size = packet_finish(packet, payload);
	if (size > 0)
		send_out(packet, size);
	pthread_mutex_unlock(&mesh_lock);
}

void peer_mesh_publish_ban(const unsigned char target[32], int votes, int players)
{
	unsigned char packet[MESH_MAX_PACKET];
	int kept;
	int payload;

	if (!target || !peer_report_accepted(votes, players))
		return;
	pthread_mutex_lock(&mesh_lock);
	if (!mesh.have_key)
		load_identity();
	if (mesh.published && same_key(mesh.published_target, target))
	{
		pthread_mutex_unlock(&mesh_lock);
		return;
	}
	kept = fill_report_packet(packet, target, players, 0, votes);
	payload = 114 + kept * 96;
	if (peer_report_accepted(kept, players))
	{
		int size = 0;
		int status = handle_report(packet + 8, payload);

		if (status > 0)
			size = packet_finish(packet, payload);
		if (size > 0)
		{
			send_out(packet, size);
			memcpy(mesh.published_target, target, 32);
			mesh.published = 1;
		}
	}
	pthread_mutex_unlock(&mesh_lock);
}

void peer_mesh_publish_strikes(const unsigned char target[32], int players)
{
	unsigned char packet[MESH_MAX_PACKET];
	int total = 0;
	int index;
	int skip = 0;

	if (!target || players <= 0)
		return;
	pthread_mutex_lock(&mesh_lock);
	if (!mesh.have_key)
		load_identity();
	for (index = 0; index < MESH_REPORTS; index++)
	{
		if (mesh.reports[index].used && same_key(mesh.reports[index].target, target))
			total++;
	}
	if (total < 1 || peer_report_accepted(total, players))
	{
		pthread_mutex_unlock(&mesh_lock);
		return;
	}
	while (skip < total)
	{
		int kept = fill_report_packet(packet, target, players, skip, 8);
		int payload;
		int size;

		if (kept < 1)
			break;
		payload = 114 + kept * 96;
		handle_report(packet + 8, payload);
		size = packet_finish(packet, payload);
		if (size > 0)
		{
			send_out(packet, size);
			if (size <= MESH_MAX_PACKET)
			{
				memcpy(mesh.strike_packet, packet, (size_t)size);
				mesh.strike_packet_size = size;
				mesh.strike_relays = 4;
				mesh.strike_relay_at = GetTickCount();
			}
		}
		skip += kept;
	}
	pthread_mutex_unlock(&mesh_lock);
}
