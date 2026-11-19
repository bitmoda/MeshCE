/*
PEER_MESH.C

Clients find each other without a server browser. Each one listens on UDP
38952, says hello on the local network, and passes along clients it has
heard. Every client also says hello to the built-in introducer names.
The saved list holds 2048 peers. Up to 64 of them are all contacted.
A longer list is contacted in slices, and a peer that stays silent is dropped.
A join also exchanges the public mesh address each client learned
from STUN, and both try that port. Find Game stays off. A peer is saved
after a hello from that peer arrives. Two clients that are both behind
routers still need a path between them, and this does not add one.

Find Game answers clients looking for the same playlist. A client fills a
seat only by echoing the nonce in that answer. The search times the round
trip, and the best connection hosts. That client sends the invite it already
makes for a party. The others join it.

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
scoreboard cannot add a vote or stand in for a player. The first player
who left is one vote against that install. The earliest quit on the kept
scoreboard is that vote, so the player who left does not have to sign it.
A player who leaves later is not a vote. Votes already signed against
that later player still count. Players who leave on the same tick are
all first. One leave by itself stays short of a ban. The same count
removes the player from the game. A vote that falls short is kept. Thirty three of those votes ban the key,
starting at the middle of the cooldown, 16 hours. The first ban from a majority waits 1 hour. Each further ban
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
#include "port_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
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
	/* Reply to a looking broadcast, and the reply to that reply. */
	MESH_ANSWER = 11,
	/* The rest of a scoreboard that does not fit in one tally packet. */
	MESH_TALLY_PART = 12,
	MESH_MAX_PACKET = 1200,
	/* Directory of peers this client can try. The announcement does not
	   go to every entry once the list is longer than MESH_FULL_SEND. */
	MESH_PEERS_MAX = 2048,
	MESH_FULL_SEND = 64,
	MESH_FANOUT = 48,
	MESH_FANOUT_GREET = 16,
	MESH_FANOUT_LIVE = 16,
	/* One score, tally, or ban goes to peers heard recently, up to this many. */
	MESH_REACH_SEND = 256,
	/* A peer that has sent nothing for this long is dropped. A full
	   directory is walked about every 10 minutes while idle, so this
	   leaves several tries. */
	MESH_STALE_SECONDS = 45 * 60,
	/* An address this client never hears is dropped after this long.
	   Introductions go out 16 at a time with the hello, so this still
	   covers several passes of that queue. */
	MESH_UNHEARD_SECONDS = 5 * 60,
	/* With this many peers heard recently, the introducer is contacted
	   about once a minute. Fewer than this, a packet that is already
	   going out is copied there about every 3 seconds. */
	MESH_SEED_SPARSE = 8,
	MESH_SEED_BUSY_MS = 3000,
	MESH_SEED_IDLE_MS = 60000,
	MESH_SEED_PAIR_MS = 50,
	MESH_CANDIDATES = 8,
	MESH_GOSSIP = 8,
	MESH_REPORTS = 16,
	MESH_STRIKES = 512,
	MESH_APPLIED = 32,
	MESH_MACHINES = 128,
	MESH_STANDINGS = 128,
	MESH_POLLS = 4,
	MESH_VOTES = 16,
	MESH_TALLY_TEXT = PEER_MESH_TALLY_TEXT,
	/* A scoreboard past one packet is sent in pieces of this many bytes. */
	MESH_TALLY_CHUNK = 1024,
	MESH_TALLY_PARTS = 2,
	MESH_TALLY_GATHER = 8,
	MESH_GATHER_MS = 600,
	MESH_PING_GAP_MS = 200,
	MESH_PING_FLOOR_MS = 400,
	MESH_PING_CAP_MS = 2500,
	MESH_SCORE_MS = 250,
	MESH_INVITE_YOUNG_MS = 1000,
	MESH_INVITE_RETRY_MS = 500,
	MESH_INVITE_EXPIRE_MS = 2000,
	MESH_FIND_MS = 200,
	MESH_FIND_OFFER_MS = 1000,
	MESH_OFFER_MS = 15000,
	/* A lobby offer walks the directory while the lobby is open. */
	MESH_WIDE_OFFER_MS = 2000,
	/* After the match starts, the invite goes to the players already in it. */
	MESH_LATE_OFFER_MS = 500,
	/* A host lobby that still has only its own players then takes an offer. */
	MESH_HOST_ALONE_MS = 8000,
	MESH_HELLO_MS = 10000,
	/* A direct address stands for this long. Only those peers are passed on. */
	MESH_REACH_SECONDS = 30,
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
	/* first FIND of this search. not saved. a claimed search age is not used. */
	unsigned ranked_seen;
	/* last looking broadcast. not saved. The address is the one that sent it. */
	unsigned looking_at;
	unsigned long looking_address;
	unsigned short looking_port;
	char looking_playlist[16];
	char looking_name[12];
	int looking_seats;
	/* the score on the last looking broadcast. 0 is a real score. */
	int looking_level;
	int looking_level_known;
	/* 1 once this peer has been on the saved peer list. */
	int known;
	unsigned char answer_nonce[16];
	int answer_sent;
	unsigned answer_at;
	int answer_tries;
	/* 1 after this client echoed their nonce. Their offer is the lobby it agreed to enter. */
	int echoed;
	/* The looking broadcast this nonce answered. A later broadcast does not move these. */
	unsigned long answer_address;
	unsigned short answer_port;
	int answer_seats;
	char answer_name[12];
	int heard;
	int used;
	/* Unix time this slot was filled. Not written to peers.dat. */
	unsigned noted;
	/* 1 until an announcement has gone to this address. Not written. */
	int greet;
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
	unsigned char raw[MESH_TALLY_PARTS][MESH_MAX_PACKET];
	int raw_size[MESH_TALLY_PARTS];
	int raw_count;
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
	/* public address of this mesh socket, from STUN, network byte order */
	unsigned long mapped_address;
	unsigned short mapped_port;
	int mapped;
	int mapped_logged;
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
	/* 1 on announcements that carry the short peer list. */
	int list_toggle;
	int sweep;
	int live_at;
	int greet_at;
	int announce;
	int targets_ready;
	int used_targets;
	int target_count;
	int targets[MESH_FANOUT];
	unsigned last_seed;
	unsigned seed_pair;
	/* Hello clock for the introducer. A search packet uses last_seed and
	   must not keep a hello from getting through. */
	unsigned last_seed_hello;
	/* 1 when the hello just sent also went to the introducer. */
	int seed_sent;
	unsigned last_offer;
	unsigned last_find;
	unsigned last_save;
	int searching;
	/* 1 after this search has drawn its known peers once. */
	int drew;
	int phase;
	unsigned phase_at;
	unsigned search_at;
	int account_level;
	char account_name[12];
	int seats;
	int retries;
	/* bumped when a search starts, ends, or is given up. a join copied
	before that does not dial after the search has moved on. */
	unsigned generation;
	int give_up;
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
	int want_redial;
	/* after the match starts, the host keeps sending the invite until this window ends */
	int late_offer;
	/* 1 while the matchmaking lobby has no seat left. offers wait for one. */
	int lobby_full;
	/* humans sitting in this host's game. 0 until the server says. */
	int game_humans;
	/* machines connected to this host's game, including this one. */
	int game_machines;
	unsigned late_offer_at;
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
		/* A row saved before a hello arrived has no time. Leave that slot
		empty so it cannot crowd out a peer this client has heard. */
		if (!peer->last_seen && !peer->bans && !peer->cooldown_until)
		{
			memset(peer, 0, sizeof(*peer));
			continue;
		}
		peer->heard = peer->last_seen ? 1 : 0;
		peer->used = 1;
		peer->known = 1;
		peer->noted = unix_now();
		if (!peer->noted)
			peer->noted = 1;
	}
	fclose(file);
}

/* The previous file stays until the new bytes are complete. A short write
does not clear the dirty flag, and does not leave a header that claims
rows the file does not hold. */
static int replace_file(const char *path, const void *bytes, size_t size)
{
	char temporary[540];
	FILE *file;
	int path_len;
	int wrote;

	if (!path || !bytes)
		return 0;
	path_len = (int)strlen(path);
	if (path_len <= 0 || path_len > 500)
		return 0;
	memcpy(temporary, path, (size_t)path_len);
	memcpy(temporary + path_len, ".tmp", 5);
	file = fopen(temporary, "wb");
	if (!file)
		return 0;
	wrote = fwrite(bytes, 1, size, file) == size;
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

static void save_peers(void)
{
	char path[512];
	unsigned char *bytes;
	int index;
	int count = 0;
	int size = 8;

	if (!mesh.dirty)
		return;
	peers_path(path, (int)sizeof(path));
	for (index = 0; index < MESH_PEERS_MAX; index++)
	{
		struct mesh_peer *peer = &mesh.peers[index];

		if (peer->used && ((peer->heard && peer->last_seen) || peer->bans || peer->cooldown_until))
			count++;
	}
	bytes = malloc((size_t)(8 + count * 48));
	if (!bytes)
		return;
	memset(bytes, 0, 8);
	memcpy(bytes, "OPPR", 4);
	bytes[4] = 1;
	bytes[5] = (unsigned char)count;
	bytes[6] = (unsigned char)(count >> 8);
	bytes[7] = (unsigned char)(count >> 16);
	for (index = 0; index < MESH_PEERS_MAX; index++)
	{
		struct mesh_peer *peer = &mesh.peers[index];

		if (!peer->used || !((peer->heard && peer->last_seen) || peer->bans || peer->cooldown_until))
			continue;
		memset(bytes + size, 0, 48);
		memcpy(bytes + size, peer->key, 32);
		write32(bytes + size + 32, (unsigned)peer->address);
		write16(bytes + size + 36, peer->port);
		write16(bytes + size + 38, (unsigned short)peer->bans);
		write32(bytes + size + 40, peer->cooldown_until);
		write32(bytes + size + 44, peer->last_seen);
		size += 48;
	}
	if (!replace_file(path, bytes, (size_t)size))
	{
		free(bytes);
		return;
	}
	free(bytes);
	for (index = 0; index < MESH_PEERS_MAX; index++)
	{
		if (mesh.peers[index].used && ((mesh.peers[index].heard && mesh.peers[index].last_seen) ||
			mesh.peers[index].bans || mesh.peers[index].cooldown_until))
			mesh.peers[index].known = 1;
	}
	mesh.dirty = 0;
}

static void prune_peers(void)
{
	unsigned now = unix_now();
	int index;

	for (index = 0; index < MESH_PEERS_MAX; index++)
	{
		struct mesh_peer *peer = &mesh.peers[index];
		int ban_active;
		int saved;

		if (!peer->used)
			continue;
		ban_active = peer->cooldown_until && peer_in_cooldown(peer->cooldown_until, now);
		if (!peer_list_expired(peer->last_seen, peer->noted, now, peer->answer_sent, ban_active,
			peer->bans > 0, MESH_UNHEARD_SECONDS, MESH_STALE_SECONDS))
			continue;
		saved = peer->known || peer->heard || peer->last_seen || peer->bans || peer->cooldown_until;
		memset(peer, 0, sizeof(*peer));
		if (saved)
			mesh.dirty = 1;
	}
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
	static unsigned char bytes[8 + MESH_STRIKES * 145];
	int index;
	int count = 0;
	int size = 8;

	if (!mesh.strikes_dirty)
		return;
	strikes_path(path, (int)sizeof(path));
	for (index = 0; index < MESH_STRIKES; index++)
	{
		if (mesh.strikes[index].used)
			count++;
	}
	memset(bytes, 0, 8);
	memcpy(bytes, "OSTK", 4);
	bytes[4] = 1;
	write16(bytes + 5, (unsigned short)count);
	for (index = 0; index < MESH_STRIKES; index++)
	{
		struct mesh_strike *strike = &mesh.strikes[index];

		if (!strike->used)
			continue;
		memset(bytes + size, 0, 145);
		memcpy(bytes + size, strike->voter, 32);
		memcpy(bytes + size + 32, strike->target, 32);
		memcpy(bytes + size + 64, strike->match, 16);
		memcpy(bytes + size + 80, strike->signature, 64);
		bytes[size + 144] = (unsigned char)strike->used;
		size += 145;
	}
	if (!replace_file(path, bytes, (size_t)size))
		return;
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

static int book_played(const unsigned char *key);
static int match_text_is(const char *text, const unsigned char id[16]);
static int book_level(const unsigned char *key);
static int invites_young(unsigned now);

static struct mesh_peer *peer_slot_kind(const unsigned char *key, int create, int named_only)
{
	int index;
	int free_slot = -1;
	int oldest = -1;
	unsigned oldest_seen = 0xffffffffu;
	unsigned now;

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
	}
	if (!create)
		return NULL;
	/* A key named only in someone else's list can take an address this
	client has not heard. A key this client heard can take the slot that
	has been quiet the longest. A peer heard in the last 30 seconds stays.
	An open invite stays. A ban that is still running stays. A ban record
	stays unless this client heard the new key. */
	now = unix_now();
	if (free_slot < 0)
	{
		for (index = 0; index < MESH_PEERS_MAX; index++)
		{
			struct mesh_peer *peer = &mesh.peers[index];
			int heard = peer->last_seen != 0;
			int ban_record = peer->bans > 0;
			int ban_active = peer->cooldown_until && peer_in_cooldown(peer->cooldown_until, now);
			int recent = 0;

			if (peer->last_seen && now >= peer->last_seen &&
				now - peer->last_seen < (unsigned)MESH_REACH_SECONDS)
				recent = 1;
			if (!peer_list_replaceable(heard, peer->answer_sent, ban_active, ban_record, named_only,
				recent))
				continue;
			if (oldest < 0 || peer->last_seen <= oldest_seen)
			{
				oldest_seen = peer->last_seen;
				oldest = index;
			}
		}
		if (oldest < 0)
			return NULL;
		free_slot = oldest;
	}
	if (mesh.peers[free_slot].used && (mesh.peers[free_slot].known || mesh.peers[free_slot].last_seen ||
		mesh.peers[free_slot].bans || mesh.peers[free_slot].cooldown_until))
		mesh.dirty = 1;
	memset(&mesh.peers[free_slot], 0, sizeof(mesh.peers[free_slot]));
	memcpy(mesh.peers[free_slot].key, key, 32);
	mesh.peers[free_slot].used = 1;
	mesh.peers[free_slot].noted = now ? now : 1;
	return &mesh.peers[free_slot];
}

static struct mesh_peer *peer_slot(const unsigned char *key, int create)
{
	return peer_slot_kind(key, create, 0);
}

static void note_peer(const unsigned char *key, unsigned long address, unsigned short port, int direct)
{
	struct mesh_peer *peer;
	unsigned now = unix_now();
	int fresh;
	int moved;

	if (!key || same_key(key, mesh.public_key))
		return;
	peer = peer_slot_kind(key, 1, direct ? 0 : 1);
	if (!peer)
		return;
	/* A list passed along by someone else can name a stale address. An address
	this client heard itself stands for 30 seconds. After that, the list can
	move a peer that has gone quiet. A hello from someone already on the list
	does not jump the introduction queue. */
	fresh = peer->heard && peer->last_seen && now - peer->last_seen < MESH_REACH_SECONDS;
	moved = 0;
	if (direct || !fresh)
	{
		if (address && peer->address != address)
		{
			peer->address = address;
			mesh.dirty = 1;
			moved = 1;
		}
		if (port && peer->port != port)
		{
			peer->port = port;
			mesh.dirty = 1;
			moved = 1;
		}
	}
	if (!direct)
	{
		if (!peer->heard && peer->address && peer->port)
			peer->greet = 1;
		return;
	}
	if (!peer->heard || moved)
		peer->greet = 1;
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

/* Names built into every install. Each one is a MeshCE client that can be
reached on UDP 38952. A reply is an ordinary hello and is saved with the
other peers. The address is looked up again so a changed record is picked up. */
static const char *const mesh_seed_names[] = {
	"bitmoda.ddns.net"
};

enum
{
	MESH_SEED_COUNT = (int)(sizeof(mesh_seed_names) / sizeof(mesh_seed_names[0])),
	MESH_SEED_REFRESH_MS = 300000,
	MESH_SEED_RETRY_MS = 30000
};

struct mesh_seed
{
	unsigned long address;
	unsigned resolved_at;
	int failed;
	int logged_failure;
};

static struct mesh_seed mesh_seeds[MESH_SEED_COUNT];

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

static int peer_reachable(const struct mesh_peer *peer, unsigned now)
{
	if (!peer->used || !peer->heard || !peer->address || !peer->port || !peer->last_seen)
		return 0;
	return now - peer->last_seen < MESH_REACH_SECONDS;
}

static int reachable_count(unsigned now)
{
	int index;
	int count = 0;

	for (index = 0; index < MESH_PEERS_MAX; index++)
	{
		if (peer_reachable(&mesh.peers[index], now))
			count++;
	}
	return count;
}

static int peer_is_seed(const struct mesh_peer *peer);

static int addressed_count(void)
{
	int index;
	int count = 0;

	for (index = 0; index < MESH_PEERS_MAX; index++)
	{
		struct mesh_peer *peer = &mesh.peers[index];

		/* The introducer is contacted on its own schedule. */
		if (peer->used && peer->address && peer->port && !peer_is_seed(peer))
			count++;
	}
	return count;
}

static int seed_endpoint(unsigned long address, unsigned short port)
{
	int index;

	if (!address || !port)
		return 0;
	for (index = 0; index < MESH_SEED_COUNT; index++)
	{
		if (mesh_seeds[index].address == address && port == net16(MESH_PORT))
			return 1;
	}
	return 0;
}

static int peer_is_seed(const struct mesh_peer *peer)
{
	if (!peer)
		return 0;
	return seed_endpoint(peer->address, peer->port);
}

static int target_has(int slot)
{
	int index;

	for (index = 0; index < mesh.target_count; index++)
	{
		if (mesh.targets[index] == slot)
			return 1;
	}
	return 0;
}

static int target_add(int slot)
{
	if (slot < 0 || slot >= MESH_PEERS_MAX || mesh.target_count >= MESH_FANOUT || target_has(slot))
		return 0;
	mesh.targets[mesh.target_count++] = slot;
	return 1;
}

/* On an announcement, peers who just made contact go out first. The
cursor walks that queue so a later arrival is not stuck behind an early
slot. Then peers heard in the last 30 seconds, then the next slice of the
directory. The slice cursor walks the whole list, so a quiet address is
tried again. A search packet does not spend its slice on introductions.
Those wait for the next hello, which carries the peer list. */
static void build_targets(unsigned now, int include_greets)
{
	int index;
	int added;
	int scanned;
	int greets;
	int start;

	mesh.target_count = 0;
	if (include_greets)
	{
		greets = 0;
		scanned = 0;
		start = mesh.greet_at;
		if (start < 0 || start >= MESH_PEERS_MAX)
			start = 0;
		index = start;
		while (scanned < MESH_PEERS_MAX && greets < MESH_FANOUT_GREET &&
			mesh.target_count < MESH_FANOUT)
		{
			struct mesh_peer *peer = &mesh.peers[index];

			if (peer->used && peer->greet && peer->address && peer->port &&
				!peer_is_seed(peer) && target_add(index))
				greets++;
			index++;
			if (index >= MESH_PEERS_MAX)
				index = 0;
			scanned++;
		}
		mesh.greet_at = index;
	}
	added = 0;
	scanned = 0;
	index = mesh.live_at;
	if (index < 0 || index >= MESH_PEERS_MAX)
		index = 0;
	while (scanned < MESH_PEERS_MAX && added < MESH_FANOUT_LIVE && mesh.target_count < MESH_FANOUT)
	{
		if (peer_reachable(&mesh.peers[index], now) && !peer_is_seed(&mesh.peers[index]) &&
			target_add(index))
			added++;
		index++;
		if (index >= MESH_PEERS_MAX)
			index = 0;
		scanned++;
	}
	mesh.live_at = index;
	scanned = 0;
	index = mesh.sweep;
	if (index < 0 || index >= MESH_PEERS_MAX)
		index = 0;
	while (scanned < MESH_PEERS_MAX && mesh.target_count < MESH_FANOUT)
	{
		struct mesh_peer *peer = &mesh.peers[index];

		if (peer->used && peer->address && peer->port && !peer_is_seed(peer))
			target_add(index);
		index++;
		if (index >= MESH_PEERS_MAX)
			index = 0;
		scanned++;
	}
	mesh.sweep = index;
}

static unsigned seed_gap_ms(void)
{
	if (reachable_count(unix_now()) < MESH_SEED_SPARSE)
		return (unsigned)MESH_SEED_BUSY_MS;
	return (unsigned)MESH_SEED_IDLE_MS;
}

static int seeds_due(unsigned now_ms)
{
	unsigned gap;

	if (!now_ms)
		now_ms = 1;
	gap = seed_gap_ms();
	if (!mesh.last_seed || now_ms - mesh.last_seed >= gap)
	{
		mesh.last_seed = now_ms;
		mesh.seed_pair = now_ms;
		return 1;
	}
	return mesh.seed_pair && now_ms - mesh.seed_pair < (unsigned)MESH_SEED_PAIR_MS;
}

/* A hello keeps its own clock. A find or an offer that just reached the
introducer must not crowd this hello out, or the short list never follows. */
static int seed_announce_due(unsigned now_ms)
{
	unsigned gap;

	if (!now_ms)
		now_ms = 1;
	gap = seed_gap_ms();
	if (mesh.last_seed_hello && now_ms - mesh.last_seed_hello < gap)
		return 0;
	mesh.last_seed_hello = now_ms;
	mesh.last_seed = now_ms;
	mesh.seed_pair = now_ms;
	return 1;
}

static void send_seed_addresses(const unsigned char *packet, int size)
{
	int index;

	for (index = 0; index < MESH_SEED_COUNT; index++)
	{
		if (!mesh_seeds[index].address)
			continue;
		send_to(packet, size, mesh_seeds[index].address, net16(MESH_PORT));
	}
}

static void send_out(const unsigned char *packet, int size)
{
	unsigned long local;
	unsigned long broadcast;
	int index;
	int addressed;

	send_to(packet, size, 0xffffffffu, net16(MESH_PORT));
	local = posix_local_ipv4_address();
	broadcast = local ? (local | 0xff000000u) : 0;
	if (broadcast && broadcast != local)
		send_to(packet, size, broadcast, net16(MESH_PORT));
	addressed = addressed_count();
	/* The introducer has its own schedule on a short list and a long one.
	   A saved introducer is not also part of the normal burst. */
	if (mesh.announce && seed_announce_due(GetTickCount()))
	{
		send_seed_addresses(packet, size);
		mesh.seed_sent = 1;
	}
	else if (seeds_due(GetTickCount()))
	{
		send_seed_addresses(packet, size);
		mesh.seed_sent = 1;
	}
	/* A small list is the whole mesh this client can see. Contact every
	saved address, including on a search. */
	if (addressed <= MESH_FULL_SEND)
	{
		for (index = 0; index < MESH_PEERS_MAX; index++)
		{
			struct mesh_peer *peer = &mesh.peers[index];

			if (!peer->used || !peer->address || !peer->port || peer_is_seed(peer))
				continue;
			send_to(packet, size, peer->address, peer->port);
		}
		mesh.used_targets = 0;
		return;
	}
	if (!mesh.announce || !mesh.targets_ready)
	{
		build_targets(unix_now(), mesh.announce);
		if (mesh.announce)
			mesh.targets_ready = 1;
	}
	mesh.used_targets = 1;
	for (index = 0; index < mesh.target_count; index++)
	{
		struct mesh_peer *peer = &mesh.peers[mesh.targets[index]];

		if (!peer->used || !peer->address || !peer->port)
			continue;
		send_to(packet, size, peer->address, peer->port);
	}
}

/* A packet that is sent once still has to reach the peers who are online
now. The steady announcement stays on the shorter slice. */
static void send_reachable(const unsigned char *packet, int size)
{
	unsigned long local;
	unsigned long broadcast;
	unsigned now;
	int index;
	int sent;
	int start;

	if (addressed_count() <= MESH_FULL_SEND)
	{
		send_out(packet, size);
		return;
	}
	send_to(packet, size, 0xffffffffu, net16(MESH_PORT));
	local = posix_local_ipv4_address();
	broadcast = local ? (local | 0xff000000u) : 0;
	if (broadcast && broadcast != local)
		send_to(packet, size, broadcast, net16(MESH_PORT));
	now = unix_now();
	for (index = 0; index < MESH_PEERS_MAX; index++)
	{
		struct mesh_peer *peer = &mesh.peers[index];

		if (!peer->used || !peer->answer_sent || !peer->address || !peer->port)
			continue;
		send_to(packet, size, peer->address, peer->port);
	}
	for (index = 0; index < MESH_CANDIDATES; index++)
	{
		struct mesh_candidate *candidate = &mesh.candidates[index];

		if (!candidate->used || !candidate->address || !candidate->port)
			continue;
		send_to(packet, size, candidate->address, candidate->port);
	}
	if (reachable_count(now) <= MESH_REACH_SEND)
	{
		for (index = 0; index < MESH_PEERS_MAX; index++)
		{
			struct mesh_peer *peer = &mesh.peers[index];

			if (!peer_reachable(peer, now))
				continue;
			send_to(packet, size, peer->address, peer->port);
		}
		return;
	}
	start = mesh.live_at;
	if (start < 0 || start >= MESH_PEERS_MAX)
		start = 0;
	index = start;
	sent = 0;
	while (sent < MESH_REACH_SEND)
	{
		struct mesh_peer *peer = &mesh.peers[index];

		if (peer_reachable(peer, now))
		{
			send_to(packet, size, peer->address, peer->port);
			sent++;
		}
		index++;
		if (index >= MESH_PEERS_MAX)
			index = 0;
		if (index == start)
			break;
	}
	mesh.live_at = index;
}

static int packet_finish(unsigned char *packet, int payload)
{
	int size = 8 + payload;

	if (!sign_unlocked(packet, size, packet + size))
		return 0;
	return size + 64;
}

static int hello_packet(unsigned char *packet)
{
	int payload = MESH_HELLO_PAYLOAD;

	memset(packet, 0, MESH_MAX_PACKET);
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
	return packet_finish(packet, payload);
}

static void send_hello(void)
{
	unsigned char packet[MESH_MAX_PACKET];
	int size = hello_packet(packet);

	if (size)
		send_out(packet, size);
}

static void send_hello_to(unsigned long address, unsigned short port)
{
	unsigned char packet[MESH_MAX_PACKET];
	int size = hello_packet(packet);

	if (size)
		send_to(packet, size, address, port);
}

/* Reservoir of at most 64. The shuffle stops at 64, and 8 names are drawn
   from that sample. A longer reachable set stays an even draw. */
static void sample_index(int *sample, int cap, int *count, int *seen, int index,
	unsigned char *entropy, int entropy_len, int *used)
{
	unsigned span;
	unsigned draw;

	if (!sample || !count || !seen || cap < 1)
		return;
	if (*count < cap)
	{
		sample[*count] = index;
		*count += 1;
		*seen += 1;
		return;
	}
	*seen += 1;
	span = (unsigned)*seen;
	if (!span || !entropy || entropy_len < 4)
		return;
	if (*used + 4 > entropy_len)
	{
		posix_random_bytes(entropy, (size_t)entropy_len);
		*used = 0;
	}
	draw = (unsigned)entropy[*used] |
		((unsigned)entropy[*used + 1] << 8) |
		((unsigned)entropy[*used + 2] << 16) |
		((unsigned)entropy[*used + 3] << 24);
	*used += 4;
	if (draw % span < (unsigned)cap)
		sample[draw % cap] = index;
}

/* Up to MESH_GOSSIP peers this client has heard directly, in an order drawn
   from the operating system's random bytes. */
static int peers_packet(unsigned char *packet)
{
	int sample[64];
	int order[64];
	unsigned char entropy[512];
	unsigned char sample_entropy[64];
	int sample_count = 0;
	int seen = 0;
	int sample_used = 0;
	int count = 0;
	int index;
	int payload;
	unsigned now = unix_now();

	memset(packet, 0, MESH_MAX_PACKET);
	packet[0] = MESH_MAGIC0;
	packet[1] = MESH_MAGIC1;
	packet[2] = MESH_MAGIC2;
	packet[3] = MESH_MAGIC3;
	packet[4] = MESH_VERSION;
	packet[5] = MESH_PEERS;
	memcpy(packet + 8, mesh.public_key, 32);
	posix_random_bytes(sample_entropy, sizeof(sample_entropy));
	for (index = 0; index < MESH_PEERS_MAX; index++)
	{
		if (peer_reachable(&mesh.peers[index], now))
			sample_index(sample, 64, &sample_count, &seen, index, sample_entropy,
				(int)sizeof(sample_entropy), &sample_used);
	}
	posix_random_bytes(entropy, sizeof(entropy));
	if (sample_count > 0 && !peer_search_shuffle(order, sample_count, entropy, (int)sizeof(entropy)))
	{
		for (index = 0; index < sample_count; index++)
			order[index] = index;
	}
	if (sample_count > 0)
	{
		int share = sample_count < MESH_GOSSIP ? sample_count : MESH_GOSSIP;

		for (index = 0; index < share; index++)
		{
			unsigned char *row = packet + 41 + count * 38;
			struct mesh_peer *peer = &mesh.peers[sample[order[index]]];

			memcpy(row, peer->key, 32);
			write32(row + 32, (unsigned)peer->address);
			write16(row + 36, peer->port);
			count++;
		}
	}
	packet[40] = (unsigned char)count;
	payload = 33 + count * 38;
	write16(packet + 6, (unsigned short)payload);
	return packet_finish(packet, payload);
}

static int send_peers(void)
{
	unsigned char packet[MESH_MAX_PACKET];
	int size = peers_packet(packet);

	if (!size)
		return 0;
	send_out(packet, size);
	return 1;
}

static void send_peers_to(unsigned long address, unsigned short port)
{
	unsigned char packet[MESH_MAX_PACKET];
	int size = peers_packet(packet);

	if (size)
		send_to(packet, size, address, port);
}

/* The in-between hello does not carry the list to the directory. The
introducer still learns it from the hello that reached it. */
static void send_peers_to_seeds(void)
{
	unsigned char packet[MESH_MAX_PACKET];
	int size = peers_packet(packet);

	if (size)
		send_seed_addresses(packet, size);
}

/* An address from a join. A peer heard directly in the last reach window
   keeps the address that packet came from. */
static void hint_peer(const unsigned char *key, unsigned long address, unsigned short port)
{
	struct mesh_peer *peer;
	unsigned now = unix_now();
	int fresh;

	if (!key || same_key(key, mesh.public_key))
		return;
	peer = peer_slot(key, 1);
	if (!peer)
		return;
	fresh = peer->heard && peer->last_seen && now - peer->last_seen < MESH_REACH_SECONDS;
	if (fresh)
		return;
	if (address && peer->address != address)
	{
		peer->address = address;
		if (peer->heard)
			mesh.dirty = 1;
	}
	if (port && peer->port != port)
	{
		peer->port = port;
		if (peer->heard)
			mesh.dirty = 1;
	}
}

static void introduce_locked(const unsigned char *key, unsigned long address, unsigned short port)
{
	struct mesh_peer *peer;
	unsigned long dest_address = address;
	unsigned short dest_port = port ? port : net16(MESH_PORT);

	load_identity();
	if (!mesh.have_key || !key || !address || same_key(key, mesh.public_key))
		return;
	hint_peer(key, address, dest_port);
	peer = peer_slot(key, 0);
	if (peer && peer->address && peer->port)
	{
		dest_address = peer->address;
		dest_port = peer->port;
	}
	if (!mesh.ready)
		return;
	send_hello_to(dest_address, dest_port);
	send_peers_to(dest_address, dest_port);
}

static int our_rank_steps(unsigned now)
{
	int elapsed = (int)((now - mesh.search_at) / 1000);
	int steps = mesh_rank_steps(elapsed);

	/* 363 is the last step a 16-bit age can describe. Past that, this
	client's window and the window it advertises would disagree. */
	if (steps > 363)
		steps = 363;
	return steps;
}

static void ranked_seen_clear(void)
{
	int index;

	for (index = 0; index < MESH_PEERS_MAX; index++)
		mesh.peers[index].ranked_seen = 0;
}

static void ranked_seen_note(const unsigned char *key, unsigned now)
{
	struct mesh_peer *peer = peer_slot(key, 0);

	if (!peer || peer->ranked_seen)
		return;
	peer->ranked_seen = now ? now : 1;
}

static int ranked_seen_steps(const unsigned char *key, unsigned now)
{
	struct mesh_peer *peer = peer_slot(key, 0);

	if (!peer || !peer->ranked_seen || now < peer->ranked_seen)
		return peer_ranked_steps(0);
	return peer_ranked_steps((int)((now - peer->ranked_seen) / 1000u));
}

static int other_candidates(const unsigned char *key)
{
	int index;
	int count = 0;

	for (index = 0; index < MESH_CANDIDATES; index++)
	{
		if (!mesh.candidates[index].used)
			continue;
		if (key && same_key(mesh.candidates[index].key, key))
			continue;
		count++;
	}
	return count;
}

static int invites_waiting_except(const unsigned char *key)
{
	int index;
	int count = 0;

	for (index = 0; index < MESH_PEERS_MAX; index++)
	{
		if (!mesh.peers[index].used || !mesh.peers[index].answer_sent)
			continue;
		if (key && same_key(mesh.peers[index].key, key))
			continue;
		if (candidate_slot(mesh.peers[index].key, 0))
			continue;
		count++;
	}
	return count;
}

static int invites_waiting(void)
{
	return invites_waiting_except(NULL);
}

static void clear_answers(void)
{
	int index;

	mesh.drew = 0;
	for (index = 0; index < MESH_PEERS_MAX; index++)
	{
		mesh.peers[index].answer_sent = 0;
		mesh.peers[index].answer_at = 0;
		mesh.peers[index].answer_tries = 0;
		mesh.peers[index].echoed = 0;
		mesh.peers[index].answer_address = 0;
		mesh.peers[index].answer_port = 0;
		mesh.peers[index].answer_seats = 0;
		memset(mesh.peers[index].answer_nonce, 0, 16);
		memset(mesh.peers[index].answer_name, 0, sizeof(mesh.peers[index].answer_name));
	}
}

/* Still written on the announcement. A receiver does not open its window
from it. It counts from the first time this search heard that client. */
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

static int book_played(const unsigned char *key)
{
	int index;

	if (!key)
		return 0;
	for (index = 0; index < MESH_STANDINGS; index++)
	{
		if (mesh.standings[index].used && same_key(mesh.standings[index].key, key))
			return 1;
	}
	return 0;
}

static int looking_age_ms(unsigned at, unsigned now)
{
	unsigned gap;

	if (!at || now < at)
		return -1;
	gap = now - at;
	if (gap > 120000u)
		return 120001;
	return (int)gap;
}

static int ranked_open(const unsigned char *key, unsigned now)
{
	int steps;

	if (strcmp(mesh.playlist, "ranked") != 0)
		return 1;
	steps = ranked_seen_steps(key, now);
	return mesh_ranked_together(mesh.account_level, steps, book_level(key), steps);
}

static int ranked_allows(const unsigned char *key, unsigned now)
{
	if (strcmp(mesh.playlist, "ranked") != 0)
		return 1;
	/* A looking broadcast starts the clock. A reply does not. */
	ranked_seen_note(key, now);
	return ranked_open(key, now);
}

static int candidate_count(void)
{
	int index;
	int count = 0;

	for (index = 0; index < MESH_CANDIDATES; index++)
	{
		if (mesh.candidates[index].used)
			count++;
	}
	return count;
}

static int search_open(void)
{
	return mesh.searching && (mesh.phase == MESH_PHASE_GATHER || mesh.phase == MESH_PHASE_PING ||
		mesh.phase == MESH_PHASE_SCORE || mesh.phase == MESH_PHASE_OFFER);
}

static void send_answer_to(unsigned long address, unsigned short port, int role, const unsigned char nonce[16])
{
	unsigned char packet[256];
	int payload = 65;

	if (!address || !port || !nonce)
		return;
	memset(packet, 0, sizeof(packet));
	packet[0] = MESH_MAGIC0;
	packet[1] = MESH_MAGIC1;
	packet[2] = MESH_MAGIC2;
	packet[3] = MESH_MAGIC3;
	packet[4] = MESH_VERSION;
	packet[5] = MESH_ANSWER;
	write16(packet + 6, (unsigned short)payload);
	memcpy(packet + 8, mesh.public_key, 32);
	memcpy(packet + 40, mesh.playlist, 16);
	memcpy(packet + 56, nonce, 16);
	packet[72] = role ? 1 : 0;
	send_to(packet, packet_finish(packet, payload), address, port);
}

static int nonce_blank(const unsigned char nonce[16])
{
	int index;

	if (!nonce)
		return 1;
	for (index = 0; index < 16; index++)
	{
		if (nonce[index])
			return 0;
	}
	return 1;
}

static int endpoint_taken(unsigned long address, unsigned short port, const unsigned char *except)
{
	int index;

	if (!address || !port)
		return 1;
	for (index = 0; index < MESH_PEERS_MAX; index++)
	{
		struct mesh_peer *peer = &mesh.peers[index];

		if (!peer->used || !peer->answer_sent || (except && same_key(peer->key, except)))
			continue;
		if (peer->answer_address == address && peer->answer_port == port)
			return 1;
	}
	for (index = 0; index < MESH_CANDIDATES; index++)
	{
		struct mesh_candidate *candidate = &mesh.candidates[index];

		if (!candidate->used || (except && same_key(candidate->key, except)))
			continue;
		if (candidate->address == address && candidate->port == port)
			return 1;
	}
	return 0;
}

static int send_looking_answer(struct mesh_peer *peer)
{
	if (!peer || peer->answer_sent || candidate_count() + invites_waiting() >= MESH_CANDIDATES)
		return 0;
	if (!peer->looking_address || !peer->looking_port)
		return 0;
	/* One socket fills one seat. Extra keys from that socket do not. */
	if (endpoint_taken(peer->looking_address, peer->looking_port, peer->key))
		return 0;
	if (!peer_include_in_search((int)peer->bans, peer->cooldown_until, unix_now(), clean_count(),
		(int)(GetTickCount() - mesh.search_at)))
		return 0;
	posix_random_bytes(peer->answer_nonce, 16);
	if (nonce_blank(peer->answer_nonce))
		return 0;
	peer->answer_address = peer->looking_address;
	peer->answer_port = peer->looking_port;
	peer->answer_seats = peer->looking_seats > 0 && peer->looking_seats <= 4 ? peer->looking_seats : 1;
	memset(peer->answer_name, 0, sizeof(peer->answer_name));
	memcpy(peer->answer_name, peer->looking_name, 11);
	peer->answer_at = GetTickCount();
	if (!peer->answer_at)
		peer->answer_at = 1;
	peer->answer_tries = 1;
	peer->answer_sent = 1;
	send_answer_to(peer->answer_address, peer->answer_port, 0, peer->answer_nonce);
	return 1;
}

static int peer_is_new(const struct mesh_peer *peer)
{
	return peer && !peer->known && !book_played(peer->key);
}

static void answer_if_room(struct mesh_peer *peer, unsigned now)
{
	int age;
	int room;

	if (!mesh.drew || !search_open() || !peer || peer->answer_sent)
		return;
	if (memcmp(peer->looking_playlist, mesh.playlist, 16) != 0)
		return;
	age = looking_age_ms(peer->looking_at, now);
	room = candidate_count() + invites_waiting() < MESH_CANDIDATES;
	/* The draw already picked the peers who were looking. A later broadcast
	fills a seat that is still open, including a known peer. */
	if (!peer_search_fill(peer_is_new(peer), age, room) || !ranked_allows(peer->key, now))
		return;
	send_looking_answer(peer);
}

static void draw_search(unsigned now)
{
	int fresh[64];
	int known_ids[64];
	int order[64];
	unsigned char entropy[1024];
	unsigned char sample_entropy[64];
	int fresh_count = 0;
	int known_count = 0;
	int fresh_seen = 0;
	int known_seen = 0;
	int sample_used = 0;
	int index;
	int sent = 0;

	if (mesh.drew)
		return;
	mesh.drew = 1;
	posix_random_bytes(sample_entropy, sizeof(sample_entropy));
	for (index = 0; index < MESH_PEERS_MAX; index++)
	{
		struct mesh_peer *peer = &mesh.peers[index];
		int age;

		if (!peer->used || memcmp(peer->looking_playlist, mesh.playlist, 16) != 0)
			continue;
		age = looking_age_ms(peer->looking_at, now);
		if (!ranked_allows(peer->key, now))
			continue;
		if (peer_is_new(peer))
		{
			if (peer_search_new(0, age))
				sample_index(fresh, 64, &fresh_count, &fresh_seen, index, sample_entropy,
					(int)sizeof(sample_entropy), &sample_used);
		}
		else if (peer_search_known(age))
			sample_index(known_ids, 64, &known_count, &known_seen, index, sample_entropy,
				(int)sizeof(sample_entropy), &sample_used);
	}
	posix_random_bytes(entropy, sizeof(entropy));
	if (fresh_count > 0 && !peer_search_shuffle(order, fresh_count, entropy, 512))
	{
		for (index = 0; index < fresh_count; index++)
			order[index] = index;
	}
	if (fresh_count > 0)
	{
		for (index = 0; index < fresh_count && sent < MESH_CANDIDATES; index++)
			sent += send_looking_answer(&mesh.peers[fresh[order[index]]]);
	}
	if (known_count > 0 && sent < MESH_CANDIDATES &&
		!peer_search_shuffle(order, known_count, entropy + 512, 512))
	{
		for (index = 0; index < known_count; index++)
			order[index] = index;
	}
	if (known_count > 0 && sent < MESH_CANDIDATES)
	{
		for (index = 0; index < known_count && sent < MESH_CANDIDATES; index++)
			sent += send_looking_answer(&mesh.peers[known_ids[order[index]]]);
	}
}

static void admit_candidate(struct mesh_peer *peer)
{
	struct mesh_candidate *candidate;
	int bans;
	unsigned until;

	if (!peer || !search_open() || !peer->answer_address || !peer->answer_port)
		return;
	if (!mesh_gamertag_ok(peer->answer_name))
		return;
	if (endpoint_taken(peer->answer_address, peer->answer_port, peer->key))
		return;
	bans = (int)peer->bans;
	until = peer->cooldown_until;
	if (!peer_include_in_search(bans, until, unix_now(), clean_count(),
		(int)(GetTickCount() - mesh.search_at)))
		return;
	/* The first echo locks the address and the seats. A repeat does not move them. */
	candidate = candidate_slot(peer->key, 0);
	if (candidate)
		return;
	candidate = candidate_slot(peer->key, 1);
	if (!candidate)
		return;
	candidate->address = peer->answer_address;
	candidate->port = peer->answer_port;
	candidate->bans = bans;
	candidate->cooldown_until = until;
	candidate->level = peer->looking_level_known ? peer->looking_level : book_level(peer->key);
	candidate->seats = peer->answer_seats > 0 && peer->answer_seats <= 4 ? peer->answer_seats : 1;
	memset(candidate->name, 0, sizeof(candidate->name));
	memcpy(candidate->name, peer->answer_name, 11);
	if (mesh.phase == MESH_PHASE_OFFER)
		mesh.want_host_note = 1;
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
	send_reachable(packet, packet_finish(packet, payload));
}

static void send_offer(int wide)
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
		const char *map_name = mesh_map_for(mesh.match_id, mesh.playlist);

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
	{
		int sent = packet_finish(packet, payload);
		int index;

		if (sent <= 0)
			return;
		if (wide)
		{
			send_out(packet, sent);
			return;
		}
		/* A late offer reaches the players already in this search. A new
		search on another machine is not recruited into a match under way. */
		for (index = 0; index < MESH_CANDIDATES; index++)
		{
			if (!mesh.candidates[index].used || !mesh.candidates[index].address ||
				!mesh.candidates[index].port)
				continue;
			send_to(packet, sent, mesh.candidates[index].address, mesh.candidates[index].port);
		}
	}
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
			/* Past sixteen waiting votes for one player in one match, a further
			one is not a real roster. A different match gives its slot up too. */
			if (same_key(strike->target, target) && memcmp(strike->match, match, 16) == 0)
			{
				if (slot_kind > 3 && strike_waiting(target, match) > 16)
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

/* A leave is stored as a vote whose voter is the player who left. A packet
cannot add one: strike_add refuses a voter who is the target. */
static struct mesh_strike *strike_leave_find(const unsigned char *target, const unsigned char *match)
{
	int index;

	if (!target || !match)
		return NULL;
	for (index = 0; index < MESH_STRIKES; index++)
	{
		struct mesh_strike *strike = &mesh.strikes[index];

		if (!strike->used || !same_key(strike->voter, target) || !same_key(strike->target, target))
			continue;
		if (memcmp(strike->match, match, 16) != 0)
			continue;
		return strike;
	}
	return NULL;
}

static void strike_leave_put(const unsigned char *target, const unsigned char *match, int used)
{
	struct mesh_strike *strike;
	int index;
	int slot = -1;
	int slot_kind = 99;

	if (!target || !match || (used != 1 && used != 2))
		return;
	strike = strike_leave_find(target, match);
	if (strike)
	{
		if (strike->used == 2 || strike->used == used)
			return;
		strike->used = used;
		mesh.strikes_dirty = 1;
		return;
	}
	for (index = 0; index < MESH_STRIKES; index++)
	{
		if (!mesh.strikes[index].used && slot_kind > 0)
		{
			slot = index;
			slot_kind = 0;
		}
		else if (mesh.strikes[index].used == 2 && slot_kind > 2)
		{
			slot = index;
			slot_kind = 2;
		}
	}
	if (slot < 0)
		return;
	memset(&mesh.strikes[slot], 0, sizeof(mesh.strikes[slot]));
	memcpy(mesh.strikes[slot].voter, target, 32);
	memcpy(mesh.strikes[slot].target, target, 32);
	memcpy(mesh.strikes[slot].match, match, 16);
	mesh.strikes[slot].used = used;
	mesh.strikes_dirty = 1;
}

static void strike_leave_drop_open(const unsigned char *target, const unsigned char *match)
{
	struct mesh_strike *strike = strike_leave_find(target, match);

	if (!strike || strike->used != 1)
		return;
	strike->used = 0;
	mesh.strikes_dirty = 1;
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
	unsigned char bytes[8 + MESH_STANDINGS * 64];
	int index;
	int count = 0;
	int size = 8;

	standings_path(path, (int)sizeof(path));
	for (index = 0; index < MESH_STANDINGS; index++)
	{
		if (mesh.standings[index].used)
			count++;
	}
	memset(bytes, 0, 8);
	memcpy(bytes, "ORNK", 4);
	bytes[4] = 1;
	bytes[5] = (unsigned char)count;
	bytes[6] = (unsigned char)(count >> 8);
	for (index = 0; index < MESH_STANDINGS; index++)
	{
		struct mesh_standing *standing = &mesh.standings[index];

		if (!standing->used)
			continue;
		memset(bytes + size, 0, 64);
		memcpy(bytes + size, standing->key, 32);
		memcpy(bytes + size + 32, standing->name, 11);
		write32(bytes + size + 44, (unsigned)(standing->games < 0 ? 0 : standing->games));
		write32(bytes + size + 48, (unsigned)(standing->level < 0 ? 0 : standing->level));
		write32(bytes + size + 52, (unsigned)(standing->wins < 0 ? 0 : standing->wins));
		write32(bytes + size + 56, (unsigned)(standing->losses < 0 ? 0 : standing->losses));
		size += 64;
	}
	replace_file(path, bytes, (size_t)size);
}

/* Match ids whose tally was already added to the standings. The poll that
applied them is only in memory, so a replay after it is gone must not add
the same games again. A fuller roster in the same session forgets the id
before it applies the replacement. Older ids stay. Dropping one to store a
newer id would let that older tally change rank again. */
enum { APPLIED_LIMIT = 65535 };

struct applied_match
{
	char id[48];
	int keys;
};

static struct applied_match *applied_matches;
static int applied_count;
static int applied_cap;

static void applied_path(char *path, int size)
{
	snprintf(path, (size_t)size, "%s/applied.dat", platform_save_root());
}

static int applied_grow(int need)
{
	int cap;
	struct applied_match *next;

	if (need <= applied_cap)
		return 1;
	cap = applied_cap > 0 ? applied_cap : 64;
	while (cap < need)
	{
		if (cap > APPLIED_LIMIT / 2)
		{
			cap = APPLIED_LIMIT;
			break;
		}
		cap *= 2;
	}
	if (cap < need || cap > APPLIED_LIMIT)
		return 0;
	next = realloc(applied_matches, (size_t)cap * sizeof(*applied_matches));
	if (!next)
		return 0;
	applied_matches = next;
	applied_cap = cap;
	return 1;
}

static void load_applied(void)
{
	char path[512];
	FILE *file;
	unsigned char header[8];
	unsigned count;
	unsigned index;

	applied_count = 0;
	applied_path(path, (int)sizeof(path));
	file = fopen(path, "rb");
	if (!file)
		return;
	if (fread(header, 1, 8, file) != 8 || memcmp(header, "OAPL", 4) != 0 || header[4] != 1)
	{
		fclose(file);
		return;
	}
	count = (unsigned)header[5] | ((unsigned)header[6] << 8);
	if (count > APPLIED_LIMIT)
		count = APPLIED_LIMIT;
	for (index = 0; index < count; index++)
	{
		unsigned char row[52];
		struct applied_match *slot;

		if (fread(row, 1, sizeof(row), file) != sizeof(row))
			break;
		if (!row[0])
			continue;
		if (!applied_grow(applied_count + 1))
			break;
		slot = &applied_matches[applied_count++];
		memset(slot, 0, sizeof(*slot));
		memcpy(slot->id, row, 47);
		slot->id[47] = 0;
		slot->keys = (int)read32(row + 48);
	}
	fclose(file);
}

static void save_applied(void)
{
	char path[512];
	unsigned char *bytes;
	int index;
	int count = 0;
	int size = 8;

	applied_path(path, (int)sizeof(path));
	for (index = 0; index < applied_count; index++)
	{
		if (applied_matches[index].id[0])
			count++;
	}
	if (count > APPLIED_LIMIT)
		count = APPLIED_LIMIT;
	bytes = malloc((size_t)(8 + count * 52));
	if (!bytes)
		return;
	memset(bytes, 0, 8);
	memcpy(bytes, "OAPL", 4);
	bytes[4] = 1;
	bytes[5] = (unsigned char)count;
	bytes[6] = (unsigned char)(count >> 8);
	for (index = 0; index < applied_count && size < 8 + count * 52; index++)
	{
		if (!applied_matches[index].id[0])
			continue;
		memset(bytes + size, 0, 52);
		memcpy(bytes + size, applied_matches[index].id, 47);
		write32(bytes + size + 48, (unsigned)(applied_matches[index].keys < 0 ? 0 :
			applied_matches[index].keys));
		size += 52;
	}
	replace_file(path, bytes, (size_t)size);
	free(bytes);
}

static int applied_known(const char *id, int *keys)
{
	int index;

	if (keys)
		*keys = 0;
	if (!id || !id[0])
		return 0;
	for (index = 0; index < applied_count; index++)
	{
		if (!applied_matches[index].id[0] || strcmp(applied_matches[index].id, id) != 0)
			continue;
		if (keys)
			*keys = applied_matches[index].keys;
		return 1;
	}
	return 0;
}

static void applied_remember(const char *id, int keys)
{
	int index;

	if (!id || !id[0])
		return;
	for (index = 0; index < applied_count; index++)
	{
		if (!applied_matches[index].id[0] || strcmp(applied_matches[index].id, id) != 0)
			continue;
		applied_matches[index].keys = keys;
		save_applied();
		return;
	}
	if (applied_count >= APPLIED_LIMIT || !applied_grow(applied_count + 1))
		return;
	memset(&applied_matches[applied_count], 0, sizeof(applied_matches[applied_count]));
	snprintf(applied_matches[applied_count].id, sizeof(applied_matches[applied_count].id), "%s", id);
	applied_matches[applied_count].keys = keys;
	applied_count++;
	save_applied();
}

static void applied_forget(const char *id)
{
	int index;

	if (!id || !id[0])
		return;
	for (index = 0; index < applied_count; index++)
	{
		if (!applied_matches[index].id[0] || strcmp(applied_matches[index].id, id) != 0)
			continue;
		memmove(&applied_matches[index], &applied_matches[index + 1],
			(size_t)(applied_count - index - 1) * sizeof(*applied_matches));
		applied_count--;
		save_applied();
		return;
	}
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
			/* The fuller tally has to come from someone on this roster.
			The stranger's own padded ballot does not hold the match. */
			if (!mesh_tally_has_key(text, poll->votes[other].key))
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
	int newly = 0;
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
	{
		int stored_keys = 0;

		/* This match was already added. The poll that remembered it is gone,
		so the same signed tally must not move the standings again. */
		if (poll->applied_keys == 0 && !poll->committed &&
			applied_known(poll->match_id, &stored_keys))
		{
			snprintf(poll->decided, sizeof(poll->decided), "%s", best);
			poll->committed = 1;
			poll->applied_keys = stored_keys > 0 ? stored_keys : best_keys;
			return;
		}
	}
	/* A shorter tally can be kept before the rest arrive. The fuller one
	takes that rank change back. */
	if (poll->applied_keys > 0 && best_keys > poll->applied_keys)
	{
		standing_undo_match(poll->match_id);
		applied_forget(poll->match_id);
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
		newly = 1;
		save_standings();
		if (same_key(poll->votes[index].key, mesh.public_key))
			note_sync_ours();
	}
	poll->applied_keys = best_keys;
	if (newly)
		applied_remember(poll->match_id, best_keys);
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
	vote->raw_count = 0;
	if (raw && raw_size > 0 && raw_size <= MESH_MAX_PACKET)
	{
		memcpy(vote->raw[0], raw, (size_t)raw_size);
		vote->raw_size[0] = raw_size;
		vote->raw_count = 1;
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
			int part;

			if (!poll->votes[vote_index].used)
				continue;
			for (part = 0; part < poll->votes[vote_index].raw_count && part < MESH_TALLY_PARTS; part++)
			{
				if (poll->votes[vote_index].raw_size[part] <= 0)
					continue;
				send_reachable(poll->votes[vote_index].raw[part], poll->votes[vote_index].raw_size[part]);
			}
		}
		poll->relays--;
	}
}

struct mesh_tally_gather
{
	int used;
	unsigned char key[32];
	char match_id[40];
	char name[12];
	int total;
	int count;
	unsigned got;
	char text[MESH_TALLY_TEXT];
	unsigned char raw[MESH_TALLY_PARTS][MESH_MAX_PACKET];
	int raw_size[MESH_TALLY_PARTS];
};

static struct mesh_tally_gather tally_gather[MESH_TALLY_GATHER];

/* The signed packets travel with the scoreboard so a later client can pass
on the same signatures. */
static void tally_attach_raws(const unsigned char *key, const char *match_id, const char *text,
	const unsigned char packets[][MESH_MAX_PACKET], const int *sizes, int count)
{
	struct mesh_poll *poll;
	int index;
	int part;

	if (!key || !match_id || !text || !packets || !sizes || count < 1)
		return;
	poll = poll_for(match_id, 0);
	if (!poll)
		return;
	for (index = 0; index < MESH_VOTES; index++)
	{
		struct mesh_vote *vote = &poll->votes[index];

		if (!vote->used || !same_key(vote->key, key) || strcmp(vote->text, text) != 0)
			continue;
		vote->raw_count = 0;
		for (part = 0; part < count && part < MESH_TALLY_PARTS; part++)
		{
			if (sizes[part] <= 0 || sizes[part] > MESH_MAX_PACKET)
				continue;
			memcpy(vote->raw[part], packets[part], (size_t)sizes[part]);
			vote->raw_size[part] = sizes[part];
			vote->raw_count = part + 1;
		}
		return;
	}
}

/* A scoreboard that does not fit in one packet arrives in order-numbered
pieces. The text is the same one every client signs. */
static void tally_take_part(const unsigned char *key, const unsigned char *packet, int size,
	const unsigned char *body, int payload)
{
	struct mesh_tally_gather *slot = NULL;
	char match_id[40];
	char name[12];
	int total;
	int part_index;
	int part_count;
	int part_len;
	int offset;
	int index;
	int free_slot = -1;
	unsigned mask;

	if (!key || !packet || !body || payload < 90)
		return;
	total = (int)read16(body + 84);
	part_index = body[86];
	part_count = body[87];
	part_len = (int)read16(body + 88);
	if (part_count < 2 || part_count > MESH_TALLY_PARTS || part_index >= part_count)
		return;
	if (total < 1 || total >= MESH_TALLY_TEXT)
		return;
	if (part_index < part_count - 1)
	{
		if (part_len != MESH_TALLY_CHUNK)
			return;
	}
	else if (part_len <= 0 || part_len > MESH_TALLY_CHUNK ||
		part_len != total - (part_count - 1) * MESH_TALLY_CHUNK)
		return;
	offset = part_index * MESH_TALLY_CHUNK;
	if (offset < 0 || offset + part_len > total || offset + part_len >= MESH_TALLY_TEXT)
		return;
	if (payload < 90 + part_len || size < 8 + payload + 64 || size > MESH_MAX_PACKET)
		return;
	memset(match_id, 0, sizeof(match_id));
	memcpy(match_id, body + 32, 39);
	if (!match_id[0])
		return;
	memset(name, 0, sizeof(name));
	memcpy(name, body + 72, 11);
	for (index = 0; index < MESH_TALLY_GATHER; index++)
	{
		if (!tally_gather[index].used)
		{
			if (free_slot < 0)
				free_slot = index;
			continue;
		}
		if (same_key(tally_gather[index].key, key) && !strcmp(tally_gather[index].match_id, match_id))
		{
			slot = &tally_gather[index];
			break;
		}
	}
	if (!slot)
	{
		if (free_slot < 0)
			free_slot = 0;
		slot = &tally_gather[free_slot];
		memset(slot, 0, sizeof(*slot));
		slot->used = 1;
		memcpy(slot->key, key, 32);
		snprintf(slot->match_id, sizeof(slot->match_id), "%s", match_id);
		slot->total = total;
		slot->count = part_count;
	}
	if (slot->total != total || slot->count != part_count)
	{
		unsigned char kept[32];

		memcpy(kept, slot->key, 32);
		memset(slot, 0, sizeof(*slot));
		slot->used = 1;
		memcpy(slot->key, kept, 32);
		snprintf(slot->match_id, sizeof(slot->match_id), "%s", match_id);
		slot->total = total;
		slot->count = part_count;
	}
	if (name[0] && !slot->name[0])
		snprintf(slot->name, sizeof(slot->name), "%s", name);
	if ((slot->got & (1u << part_index)) &&
		memcmp(slot->text + offset, body + 90, (size_t)part_len) != 0)
	{
		/* This install sent a different scoreboard. Keep the latest piece. */
		slot->got = 0;
		memset(slot->text, 0, (size_t)total);
	}
	memcpy(slot->text + offset, body + 90, (size_t)part_len);
	slot->got |= 1u << part_index;
	memcpy(slot->raw[part_index], packet, (size_t)size);
	slot->raw_size[part_index] = size;
	mask = (1u << part_count) - 1u;
	if ((slot->got & mask) != mask)
		return;
	slot->text[total] = 0;
	tally_store(key, slot->match_id, slot->name, slot->text, slot->raw[0], slot->raw_size[0]);
	tally_attach_raws(key, slot->match_id, slot->text, slot->raw, slot->raw_size, part_count);
	slot->used = 0;
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
	memcpy(message, "MeshCE report v1", 16);
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

/* A vote counts after the tally is kept, and only from an install that
signed that tally. The first player who left counts as one vote against
that install. A player who leaves later does not. Votes already signed
against that later player still count, because the tally shows they
played. The player count in a report packet is not used. */
static void report_settle(const unsigned char *target, const unsigned char *match)
{
	struct mesh_poll *poll;
	int listed;
	int good = 0;
	int left;
	int quit;
	int index;

	if (!target || !match)
		return;
	poll = committed_poll(match);
	if (!poll)
		return;
	left = mesh_tally_key_left(poll->decided, target);
	quit = mesh_tally_key_quit(poll->decided, target);
	if (!quit)
		strike_leave_drop_open(target, match);
	if (!tally_signed(poll, target) && !left)
		return;
	listed = mesh_tally_key_count(poll->decided);
	for (index = 0; index < MESH_STRIKES; index++)
	{
		struct mesh_strike *strike = &mesh.strikes[index];

		if ((strike->used != 1 && strike->used != 3) || !same_key(strike->target, target) ||
			memcmp(strike->match, match, 16) != 0)
			continue;
		if (same_key(strike->voter, target))
			continue;
		if (!tally_signed(poll, strike->voter))
			continue;
		good++;
	}
	if (quit)
		good++;
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
			if (same_key(strike->voter, target) || !tally_signed(poll, strike->voter))
				continue;
			strike->used = 2;
			mesh.strikes_dirty = 1;
		}
		if (quit)
			strike_leave_put(target, match, 2);
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
		if (same_key(strike->voter, target) || !tally_signed(poll, strike->voter))
			continue;
		strike->used = 1;
		mesh.strikes_dirty = 1;
	}
	if (quit)
		strike_leave_put(target, match, 1);
	while (peer_strikes_ban(strike_open(target)))
		strike_apply(target);
	if (mesh.dirty)
		save_peers();
	if (mesh.strikes_dirty)
		save_strikes();
}

static void report_settle_match(const unsigned char match[16])
{
	unsigned char seen[32][32];
	struct mesh_poll *poll;
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
		if (!copy || seen_count >= 32)
			continue;
		memcpy(seen[seen_count], mesh.strikes[index].target, 32);
		seen_count++;
	}
	/* A leave with no other vote is still a vote. The scoreboard names the install. */
	poll = committed_poll(match);
	if (poll)
	{
		int keys = mesh_tally_key_count(poll->decided);

		for (index = 0; index < keys; index++)
		{
			unsigned char key[32];
			int earlier;
			int copy = 1;

			if (!mesh_tally_key_at(poll->decided, index, key) || !mesh_tally_key_quit(poll->decided, key))
				continue;
			for (earlier = 0; earlier < seen_count; earlier++)
			{
				if (same_key(seen[earlier], key))
					copy = 0;
			}
			if (!copy || seen_count >= 32)
				continue;
			memcpy(seen[seen_count], key, 32);
			seen_count++;
		}
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
		if (payload < 69)
			break;
		if (read16(body + 48) != HALO_PORT_NETWORK_VERSION)
			break;
		{
			char announced[12];
			struct mesh_peer *peer = peer_slot(key, 0);
			unsigned now_ms = GetTickCount();

			memset(announced, 0, sizeof(announced));
			memcpy(announced, body + 53, 11);
			if (!mesh_gamertag_ok(announced) || !peer)
				break;
			/* Remember the looking broadcast even when this client is not
			searching, so a search can see the last minute or two. */
			peer->looking_at = now_ms ? now_ms : 1;
			peer->looking_address = address;
			peer->looking_port = port;
			memcpy(peer->looking_playlist, body + 32, 16);
			peer->looking_seats = body[52] > 0 && body[52] <= 4 ? body[52] : 1;
			peer->looking_level = (int)read16(body + 50);
			peer->looking_level_known = 1;
			memset(peer->looking_name, 0, sizeof(peer->looking_name));
			memcpy(peer->looking_name, announced, 11);
			if (!mesh.searching || memcmp(peer->looking_playlist, mesh.playlist, 16) != 0)
				break;
			if (!ranked_allows(key, now_ms))
				break;
			answer_if_room(peer, now_ms);
		}
		break;
	case MESH_ANSWER:
		if (payload < 65 || !mesh.searching || memcmp(body + 32, mesh.playlist, 16) != 0)
			break;
		{
			struct mesh_peer *peer = peer_slot(key, 0);
			unsigned char nonce[16];
			unsigned now_ms = GetTickCount();

			if (!peer)
				break;
			memcpy(nonce, body + 48, 16);
			/* The clock starts on a looking broadcast. This packet does not move it. */
			if (!ranked_open(key, now_ms))
				break;
			if (body[64] == 0)
			{
				int others = candidate_count() > 0 || invites_waiting() > 0;

				/* Echo during the measurement, so both searches see the same
				players. Once this client is hosting, an echo would list it in
				a lobby it is not going to join. */
				if ((mesh.phase == MESH_PHASE_PING || mesh.phase == MESH_PHASE_SCORE ||
					mesh.phase == MESH_PHASE_OFFER) &&
					peer_lobby_echo(mesh.phase == MESH_PHASE_OFFER, peer->answer_sent, others) &&
					peer_include_in_search((int)peer->bans, peer->cooldown_until, unix_now(),
						clean_count(), (int)(GetTickCount() - mesh.search_at)))
				{
					send_answer_to(address, port, 1, nonce);
					peer->echoed = 1;
				}
			}
			else if (peer_answer_admits(body[64], peer->answer_sent,
				memcmp(nonce, peer->answer_nonce, 16) == 0, nonce_blank(peer->answer_nonce)))
				admit_candidate(peer);
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
		/* The answered count in the packet is not how many clients they reached. */
		candidate->score_answered = peer_score_answers(candidate->rtt_count);
		candidate->score_have = 1;
		break;
	}
	case MESH_OFFER:
		{
			struct mesh_peer *offer_peer = peer_slot(key, 0);

			if (offer_peer && payload >= 261)
			{
				offer_peer->looking_level = (int)read16(body + 259);
				offer_peer->looking_level_known = 1;
			}
		}
		/* the first offer can name a private address, or none, when the public
		one is not known yet. a later offer from that host replaces it. */
		if (mesh.have_match && !mesh.we_host && same_key(key, mesh.winner) &&
			(mesh.phase == MESH_PHASE_DONE || !mesh.searching))
		{
			int invite_len;
			char next[160];
			int next_reach;
			int have_reach;

			if (payload < 259 || memcmp(body + 32, mesh.playlist, 16) != 0)
				break;
			invite_len = body[50];
			if (invite_len <= 0 || invite_len > 159)
				break;
			memcpy(next, body + 51, (size_t)invite_len);
			next[invite_len] = 0;
			next_reach = p2p_invite_reach(next);
			have_reach = p2p_invite_reach(mesh.invite);
			if (next_reach > 0 && next_reach >= have_reach &&
				(next_reach > have_reach || strcmp(next, mesh.invite) != 0))
			{
				memcpy(mesh.invite, next, (size_t)invite_len + 1);
				mesh.want_join_note = 1;
				mesh.want_redial = 1;
			}
			break;
		}
		if (!mesh.searching || mesh.phase == MESH_PHASE_DONE || payload < 259)
			break;
		if (memcmp(body + 32, mesh.playlist, 16) != 0)
			break;
		/* An offer during the gather is early. After that, a search that has
		already seen someone else waits until it chooses a host. A reply
		that has not come back yet counts as someone else, so an offer cannot
		take the search while a selected peer can still answer. A search that
		is still alone can leave its lobby. A chosen host is the only offer
		that joins. */
		{
			unsigned now_ms = GetTickCount();
			int gathering = mesh.phase == MESH_PHASE_GATHER;
			int host_chosen = mesh.phase == MESH_PHASE_JOIN;
			/* The first moment of the measurement is still collecting replies.
			An offer in that moment does not take the search. */
			int hearing = mesh.phase == MESH_PHASE_PING &&
				(invites_young(now_ms) || now_ms - mesh.phase_at < (unsigned)MESH_PING_FLOOR_MS);
			int waiting = peer_offer_invites(mesh.phase == MESH_PHASE_OFFER, candidate_count(),
				invites_waiting_except(key));
			int seen_others = peer_search_seen(other_candidates(key), waiting,
				(mesh.phase == MESH_PHASE_OFFER && mesh.grouped) || hearing);
			int yields = mesh_solo_yields(!strcmp(mesh.playlist, "ranked"), mesh.account_level,
				book_level(key), memcmp(key, mesh.public_key, 32) < 0);
			struct mesh_peer *offer_peer = peer_slot(key, 0);
			int known_offer = candidate_slot(key, 0) != NULL;
			int echoed_join = peer_offer_from_echo(offer_peer && offer_peer->echoed,
				known_offer, other_candidates(key), invites_waiting_except(key),
				hearing, gathering, host_chosen);
			int alone_join = 0;
			int locals = mesh.seats > 0 ? mesh.seats : 1;
			int remote_seated = mesh.phase == MESH_PHASE_OFFER && mesh.we_host &&
				peer_lobby_occupied(mesh.game_humans, locals, mesh.game_machines);

			/* a lobby that already has its players does not leave for an echo.
			a rematch sets grouped for that reason. */
			if (mesh.grouped)
				echoed_join = 0;
			if (mesh.phase == MESH_PHASE_OFFER && mesh.we_host && known_offer && !remote_seated)
			{
				int waited = mesh.phase_at && now_ms - mesh.phase_at >= (unsigned)MESH_HOST_ALONE_MS;

				alone_join = peer_host_joins(0, memcmp(key, mesh.public_key, 32) < 0, waited);
			}

			if (mesh.phase != MESH_PHASE_JOIN && mesh.phase != MESH_PHASE_GATHER &&
				mesh.phase != MESH_PHASE_PING && mesh.phase != MESH_PHASE_SCORE &&
				mesh.phase != MESH_PHASE_OFFER)
				break;
			/* Answered their invite, and they are not in this lobby: their
			offer is the game. The lower key would otherwise stay here alone.
			A host lobby that still has only its own players takes the offer.
			A player already sitting in the game keeps this host. */
			if (!peer_offer_taken(peer_offer_accepted(gathering, host_chosen, same_key(key, mesh.winner),
				seen_others, yields), echoed_join, alone_join, remote_seated))
				break;
		}
		if (!strcmp(mesh.playlist, "ranked"))
		{
			unsigned now_ms = GetTickCount();
			int steps;

			if (payload < 263)
				break;
			ranked_seen_note(key, now_ms);
			steps = ranked_seen_steps(key, now_ms);
			if (!mesh_ranked_together(mesh.account_level, steps, book_level(key), steps))
				break;
		}
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
	case MESH_TALLY_PART:
		tally_take_part(key, packet, size, body, payload);
		break;
	default:
		break;
	}
}

/* The mesh socket's public address. The game tunnel's STUN answer names a
different port, and a hello sent there never reaches this socket. */
enum
{
	MESH_STUN_SERVERS = 2,
	MESH_STUN_ATTEMPTS = 6,
	MESH_STUN_RETRY_MS = 500,
	MESH_STUN_REFRESH_MS = 25000
};

struct mesh_stun
{
	char host[64];
	unsigned long address;
	unsigned short port;
	unsigned char transaction[12];
	int attempts;
	unsigned sent_at;
	int has_mapped;
	unsigned long mapped_address;
	unsigned short mapped_port;
};

static struct mesh_stun mesh_stun_servers[MESH_STUN_SERVERS];
static int mesh_stun_count;
static int mesh_stun_parsed;
static int mesh_stun_symmetric;

static void mesh_stun_prepare(void)
{
	const char *text;

	if (mesh_stun_parsed)
		return;
	mesh_stun_parsed = 1;
	text = config_string("network.stun_servers");
	if (!text)
		return;
	while (*text && mesh_stun_count < MESH_STUN_SERVERS)
	{
		const char *end = text + strcspn(text, ",");
		struct mesh_stun *server = &mesh_stun_servers[mesh_stun_count];
		const char *colon;
		int length;

		while (text < end && *text == ' ')
			text++;
		length = (int)(end - text);
		while (length > 0 && text[length - 1] == ' ')
			length--;
		if (length > 0 && length < (int)sizeof(server->host))
		{
			memset(server, 0, sizeof(*server));
			memcpy(server->host, text, (size_t)length);
			colon = strchr(server->host, ':');
			server->port = net16(3478);
			if (colon)
			{
				server->port = net16((unsigned short)atoi(colon + 1));
				server->host[colon - server->host] = 0;
			}
			mesh_stun_count++;
		}
		text = *end ? end + 1 : end;
	}
}

static void mesh_stun_note(unsigned long address, unsigned short port)
{
	int index;
	int differ = 0;

	mesh.mapped_address = address;
	mesh.mapped_port = port;
	mesh.mapped = address && port;
	if (!mesh.mapped_logged && mesh.mapped)
	{
		unsigned char bytes[4];

		memcpy(bytes, &address, 4);
		platform_log("peer mesh public address %u.%u.%u.%u:%u",
			bytes[0], bytes[1], bytes[2], bytes[3], (unsigned)net16(port));
		mesh.mapped_logged = 1;
	}
	for (index = 0; index < mesh_stun_count; index++)
	{
		if (mesh_stun_servers[index].has_mapped && mesh_stun_servers[index].mapped_port != port)
			differ = 1;
	}
	if (differ && !mesh_stun_symmetric)
	{
		mesh_stun_symmetric = 1;
		platform_log("peer mesh: this network gives each destination its own port, so a join can still fail to meet");
	}
}

static int mesh_stun_take(const unsigned char *packet, int size, const struct sockaddr_in *from)
{
	int index;
	int offset;

	if (!packet || size < 20 || packet[0] != 0x01 || packet[1] != 0x01 || !from)
		return 0;
	for (index = 0; index < mesh_stun_count; index++)
	{
		struct mesh_stun *server = &mesh_stun_servers[index];

		if (server->address && server->address == from->sin_addr.s_addr &&
			server->port == from->sin_port && !memcmp(packet + 8, server->transaction, 12))
			break;
	}
	if (index == mesh_stun_count)
		return 0;
	for (offset = 20; offset + 4 <= size; )
	{
		int type = (packet[offset] << 8) | packet[offset + 1];
		int length = (packet[offset + 2] << 8) | packet[offset + 3];
		const unsigned char *value = packet + offset + 4;

		if (offset + 4 + length > size)
			break;
		if ((type == 0x0020 || type == 0x0001) && length >= 8 && value[1] == 0x01)
		{
			unsigned char port_bytes[2];
			unsigned char ip[4];
			int byte;
			unsigned long address;
			unsigned short port;

			memcpy(port_bytes, value + 2, 2);
			memcpy(ip, value + 4, 4);
			if (type == 0x0020)
			{
				for (byte = 0; byte < 2; byte++)
					port_bytes[byte] ^= packet[4 + byte];
				for (byte = 0; byte < 4; byte++)
					ip[byte] ^= packet[4 + byte];
			}
			memcpy(&address, ip, 4);
			memcpy(&port, port_bytes, 2);
			mesh_stun_servers[index].mapped_address = address;
			mesh_stun_servers[index].mapped_port = port;
			mesh_stun_servers[index].has_mapped = 1;
			mesh_stun_note(address, port);
			break;
		}
		offset += 4 + ((length + 3) & ~3);
	}
	return 1;
}

static void mesh_stun_send(unsigned now)
{
	int index;

	if (mesh.socket < 0)
		return;
	for (index = 0; index < mesh_stun_count; index++)
	{
		struct mesh_stun *server = &mesh_stun_servers[index];
		unsigned char request[20];
		struct sockaddr_in to;
		int due;

		if (!server->address)
			continue;
		if (server->has_mapped || server->attempts >= MESH_STUN_ATTEMPTS)
			due = !server->sent_at || now - server->sent_at >= MESH_STUN_REFRESH_MS;
		else
			due = !server->sent_at || now - server->sent_at >= MESH_STUN_RETRY_MS;
		if (!due)
			continue;
		if (server->has_mapped || server->attempts >= MESH_STUN_ATTEMPTS)
			server->attempts = 0;
		memset(request, 0, sizeof(request));
		request[1] = 0x01;
		request[4] = 0x21;
		request[5] = 0x12;
		request[6] = 0xA4;
		request[7] = 0x42;
		if (!server->attempts)
			posix_random_bytes(server->transaction, sizeof(server->transaction));
		memcpy(request + 8, server->transaction, sizeof(server->transaction));
		memset(&to, 0, sizeof(to));
		to.sin_family = AF_INET;
		to.sin_addr.s_addr = server->address;
		to.sin_port = server->port;
		posix_socket_sendto(mesh.socket, request, sizeof(request), 0, &to, (int)sizeof(to));
		server->sent_at = now ? now : 1;
		server->attempts++;
	}
}

static void mesh_stun_resolve(void)
{
	char host[64];
	int index = -1;
	unsigned long address;
	unsigned now;
	int found;

	pthread_mutex_lock(&mesh_lock);
	now = GetTickCount();
	for (found = 0; found < mesh_stun_count; found++)
	{
		struct mesh_stun *server = &mesh_stun_servers[found];

		if (!server->host[0] || server->address)
			continue;
		if (server->attempts && (server->attempts < MESH_STUN_ATTEMPTS ||
			(server->sent_at && now - server->sent_at < MESH_STUN_REFRESH_MS)))
			continue;
		memcpy(host, server->host, sizeof(host));
		server->attempts = 0;
		index = found;
		break;
	}
	pthread_mutex_unlock(&mesh_lock);
	if (index < 0)
		return;
	address = posix_resolve_ipv4(host);
	pthread_mutex_lock(&mesh_lock);
	if (index < mesh_stun_count && !strcmp(mesh_stun_servers[index].host, host) &&
		!mesh_stun_servers[index].address)
	{
		if (address)
			mesh_stun_servers[index].address = address;
		else
		{
			platform_log("peer mesh: cannot look up the STUN server %s", host);
			mesh_stun_servers[index].attempts = MESH_STUN_ATTEMPTS;
			mesh_stun_servers[index].sent_at = GetTickCount();
		}
	}
	pthread_mutex_unlock(&mesh_lock);
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
		if (got < 0)
			return;
		if (mesh_stun_take(packet, got, &from))
			continue;
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
	/* Reached nobody: the same 65535 as any other client that did not answer.
	A 0 here would win against them, and every client would elect itself. */
	if (answered <= 0)
		mine = (unsigned short)65535;
	memset(best_key, 0, sizeof(best_key));
	/* A client in cooldown does not host. Leaving it in the comparison made
	the search wait for an offer this machine will never send. */
	if (!peer_in_cooldown(mesh.self_until, unix_now()))
	{
		memcpy(best_key, mesh.public_key, 32);
		best_median = mine;
		/* One or none, the same rule as every other client. The number of
		people this client reached would win a median tie on this machine only. */
		best_answered = peer_score_answers(answered);
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
				answers = peer_score_answers(candidate->rtt_count);
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

static void maybe_find(unsigned now)
{
	unsigned gap;

	if (mesh.phase != MESH_PHASE_GATHER && mesh.phase != MESH_PHASE_PING &&
		mesh.phase != MESH_PHASE_SCORE && mesh.phase != MESH_PHASE_OFFER)
		return;
	gap = mesh.phase == MESH_PHASE_OFFER ? (unsigned)MESH_FIND_OFFER_MS : (unsigned)MESH_FIND_MS;
	if (mesh.last_find && now - mesh.last_find < gap)
		return;
	send_find();
	mesh.last_find = now ? now : 1;
}

static void release_invite(struct mesh_peer *peer)
{
	peer->answer_sent = 0;
	peer->answer_at = 0;
	peer->answer_tries = 0;
	peer->answer_address = 0;
	peer->answer_port = 0;
	peer->answer_seats = 0;
	memset(peer->answer_nonce, 0, 16);
	memset(peer->answer_name, 0, sizeof(peer->answer_name));
}

static void refresh_invites(unsigned now)
{
	int index;

	for (index = 0; index < MESH_PEERS_MAX; index++)
	{
		struct mesh_peer *peer = &mesh.peers[index];
		unsigned age;

		if (!peer->used || !peer->answer_sent)
			continue;
		if (candidate_slot(peer->key, 0))
			continue;
		if (!peer->answer_at || now < peer->answer_at)
			peer->answer_at = now ? now : 1;
		age = now - peer->answer_at;
		if (age >= (unsigned)MESH_INVITE_EXPIRE_MS)
		{
			release_invite(peer);
			continue;
		}
		/* Send again until they answer. One send can land while they are
		still gathering, and the next one arrives once they can echo it. */
		if (peer->answer_tries < 4 && age >= (unsigned)peer->answer_tries * (unsigned)MESH_INVITE_RETRY_MS &&
			peer->answer_address && peer->answer_port && !nonce_blank(peer->answer_nonce))
		{
			peer->answer_tries++;
			send_answer_to(peer->answer_address, peer->answer_port, 0, peer->answer_nonce);
		}
	}
}

static int invites_young(unsigned now)
{
	int index;
	int count = 0;

	for (index = 0; index < MESH_PEERS_MAX; index++)
	{
		struct mesh_peer *peer = &mesh.peers[index];
		unsigned age;

		if (!peer->used || !peer->answer_sent)
			continue;
		if (candidate_slot(peer->key, 0))
			continue;
		age = peer->answer_at && now >= peer->answer_at ? now - peer->answer_at : 0;
		if (age < (unsigned)MESH_INVITE_YOUNG_MS)
			count++;
	}
	return count;
}

static int pings_settled(unsigned now)
{
	int index;

	for (index = 0; index < MESH_CANDIDATES; index++)
	{
		struct mesh_candidate *candidate = &mesh.candidates[index];

		if (!candidate->used || candidate->rtt_count > 0)
			continue;
		if (candidate->pings_sent < 1 || !candidate->sent_at || now - candidate->sent_at < 300)
			return 0;
	}
	return 1;
}

static int scores_ready(void)
{
	int index;

	for (index = 0; index < MESH_CANDIDATES; index++)
	{
		if (mesh.candidates[index].used && mesh.candidates[index].rtt_count > 0 &&
			!mesh.candidates[index].score_have)
			return 0;
	}
	return 1;
}

static void search_tick(void)
{
	unsigned now = GetTickCount();
	int index;

	if (!mesh.searching)
		return;
	if (mesh.phase == MESH_PHASE_COOLDOWN)
	{
		if (!peer_in_cooldown(mesh.self_until, unix_now()))
		{
			mesh.phase = MESH_PHASE_GATHER;
			mesh.phase_at = now;
			mesh.last_find = 0;
		}
		return;
	}
	maybe_find(now);
	refresh_invites(now);
	if (mesh.phase == MESH_PHASE_GATHER)
	{
		if (now - mesh.phase_at >= MESH_GATHER_MS)
		{
			/* New peers from the last minute, then one random draw of the
			known peers who broadcast in the last two minutes. The replies
			go out together. A peer fills a seat only after they answer. */
			draw_search(now);
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
			if (candidate->sent_at && now - candidate->sent_at < (unsigned)MESH_PING_GAP_MS)
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
		/* Wait for the replies that are still on the way, and for one round
		trip from each peer who answered. Then choose the host. A reply that
		never comes back does not hold the search open. */
		if (now - mesh.phase_at >= (unsigned)MESH_PING_CAP_MS ||
			(now - mesh.phase_at >= (unsigned)MESH_PING_FLOOR_MS &&
				!invites_young(now) && pings_settled(now)))
		{
			send_score();
			mesh.phase = MESH_PHASE_SCORE;
			mesh.phase_at = now;
		}
	}
	if (mesh.phase == MESH_PHASE_SCORE && (scores_ready() || now - mesh.phase_at >= (unsigned)MESH_SCORE_MS))
		decide();
	if (mesh.phase == MESH_PHASE_OFFER)
	{
		if (!mesh.lobby_full && (!mesh.last_offer || now - mesh.last_offer >= (unsigned)MESH_WIDE_OFFER_MS))
		{
			send_offer(1);
			mesh.last_offer = now;
		}
		/* The lobby's start time follows the search. Free for all starts once
		two people are seated. 2v2 starts when all four seats are filled.
		That time has to reach the host. */
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
		{
			/* Two waits produced no game. Leave the search, so Find Game
			can be opened again instead of staying up and deaf. */
			mesh.phase = MESH_PHASE_IDLE;
			mesh.searching = 0;
			mesh.have_match = 0;
			mesh.give_up = 1;
			mesh.generation++;
		}
		else
		{
			memset(mesh.candidates, 0, sizeof(mesh.candidates));
			clear_answers();
			mesh.last_find = 0;
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
		send_reachable(packet, sent);
	}
	mesh.result_sends_left--;
	mesh.result_sent_at = now ? now : 1;
	if (mesh.result_sends_left <= 0)
		mesh.result_send = 0;
}

/* the search has stopped, and the host still sends the invite. a public
address learned from STUN after the lobby opened has to reach the joiners. */
static void offer_late(unsigned now)
{
	if (!mesh.late_offer || mesh.searching || !mesh.we_host || !mesh.have_match || mesh.lobby_full)
		return;
	if (!mesh.late_offer_at)
		mesh.late_offer_at = now ? now : 1;
	if (now - mesh.late_offer_at >= 30000)
	{
		mesh.late_offer = 0;
		return;
	}
	if (mesh.last_offer && now - mesh.last_offer < (unsigned)MESH_LATE_OFFER_MS)
		return;
	send_offer(0);
	mesh.last_offer = now ? now : 1;
}

static int directory_has_greet(void)
{
	int index;

	for (index = 0; index < MESH_PEERS_MAX; index++)
	{
		struct mesh_peer *peer = &mesh.peers[index];

		if (peer->used && peer->greet && peer->address && peer->port && !peer_is_seed(peer))
			return 1;
	}
	return 0;
}

static void mesh_tick_locked(void)
{
	unsigned now;

	load_identity();
	if (!mesh.standings_ready)
	{
		load_standings();
		load_applied();
		mesh.standings_ready = 1;
		note_sync_ours();
	}
	if (mesh.have_key)
		mesh.account_level = book_level(mesh.public_key);
	open_socket();
	if (!mesh.ready)
		return;
	mesh_stun_prepare();
	receive_all();
	now = GetTickCount();
	mesh_stun_send(now);
	if (!mesh.last_hello || now - mesh.last_hello >= MESH_HELLO_MS)
	{
		int carry_list;
		int sent_list;

		prune_peers();
		mesh.announce = 1;
		mesh.targets_ready = 0;
		mesh.used_targets = 0;
		/* Every other hello carries the short list. An introduction
		   carries it too. The introducer is sent the list with whichever
		   hello reached it. Greet stays set until that list goes out, so a
		   search packet cannot spend the introduction. */
		mesh.list_toggle ^= 1;
		carry_list = mesh.list_toggle || directory_has_greet();
		mesh.seed_sent = 0;
		sent_list = 0;
		send_hello();
		if (carry_list)
			sent_list = send_peers();
		else if (mesh.seed_sent)
			send_peers_to_seeds();
		/* The pair window is only for this hello and its list. A find or
		   an offer later in this tick stays on the ordinary schedule. */
		mesh.seed_pair = 0;
		if (sent_list && mesh.used_targets)
		{
			int slot;

			for (slot = 0; slot < mesh.target_count; slot++)
			{
				int index = mesh.targets[slot];

				if (index >= 0 && index < MESH_PEERS_MAX)
					mesh.peers[index].greet = 0;
			}
		}
		else if (sent_list)
		{
			int index;

			for (index = 0; index < MESH_PEERS_MAX; index++)
				mesh.peers[index].greet = 0;
		}
		mesh.announce = 0;
		mesh.targets_ready = 0;
		mesh.last_hello = now;
	}
	search_tick();
	offer_late(now);
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
		send_reachable(mesh.strike_packet, mesh.strike_packet_size);
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

/* one name per pass. the lookup blocks, so it stays off mesh_lock. */
static void mesh_seed_resolve(void)
{
	char host[64];
	int index = -1;
	unsigned long address;
	unsigned now;
	int found;

	pthread_mutex_lock(&mesh_lock);
	now = GetTickCount();
	if (!now)
		now = 1;
	for (found = 0; found < MESH_SEED_COUNT; found++)
	{
		struct mesh_seed *seed = &mesh_seeds[found];
		unsigned wait = seed->failed ? (unsigned)MESH_SEED_RETRY_MS : (unsigned)MESH_SEED_REFRESH_MS;

		if (seed->resolved_at && now - seed->resolved_at < wait)
			continue;
		snprintf(host, sizeof(host), "%s", mesh_seed_names[found]);
		index = found;
		break;
	}
	pthread_mutex_unlock(&mesh_lock);
	if (index < 0)
		return;
	address = posix_resolve_ipv4(host);
	pthread_mutex_lock(&mesh_lock);
	if (index < MESH_SEED_COUNT && !strcmp(mesh_seed_names[index], host))
	{
		struct mesh_seed *seed = &mesh_seeds[index];
		unsigned stamped = GetTickCount();

		if (!stamped)
			stamped = 1;
		seed->resolved_at = stamped;
		if (address)
		{
			if (seed->address != address || seed->failed)
			{
				unsigned char bytes[4];

				memcpy(bytes, &address, 4);
				platform_log("peer mesh seed %s is %u.%u.%u.%u", host,
					bytes[0], bytes[1], bytes[2], bytes[3]);
			}
			seed->address = address;
			seed->failed = 0;
			seed->logged_failure = 0;
		}
		else
		{
			seed->failed = 1;
			if (!seed->logged_failure)
			{
				platform_log("peer mesh: cannot look up the seed %s", host);
				seed->logged_failure = 1;
			}
		}
	}
	pthread_mutex_unlock(&mesh_lock);
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
		int searching = 0;
		int redial = 0;
		int give_up = 0;
		unsigned snap_generation = 0;
		char playlist[16];
		char invite[160];
		unsigned char match[16];

		mesh_seed_resolve();
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
		if (mesh.give_up)
		{
			mesh.give_up = 0;
			give_up = 1;
		}
		if (mesh.want_join_note)
		{
			mesh.want_join_note = 0;
			join = 1;
			redial = mesh.want_redial;
			mesh.want_redial = 0;
			snap_generation = mesh.generation;
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
		searching = mesh.searching;
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
		if (give_up)
			matchmaking_reset();
		if (join && !give_up)
		{
			char text[40];
			int index;
			int dial = 0;

			for (index = 0; index < 16; index++)
				snprintf(text + index * 2, 3, "%02x", match[index]);
			matchmaking_note_mesh(0, playlist, 0, invite, text);
			if (redial)
			{
				pthread_mutex_lock(&mesh_lock);
				dial = mesh.generation == snap_generation && mesh.have_match &&
					strcmp(mesh.invite, invite) == 0;
				pthread_mutex_unlock(&mesh_lock);
			}
			if (dial)
				p2p_join_invite(invite);
		}
		mesh_account_serve();
		mesh_stun_resolve();
		posix_socket_select(NULL, NULL, NULL, NULL, NULL, NULL, 0, searching ? 50000 : 200000, 0);
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
	mesh.lobby_full = 0;
	mesh.we_host = 0;
	mesh.retries = 0;
	mesh.want_host_note = 0;
	mesh.want_join_note = 0;
	mesh.want_redial = 0;
	mesh.late_offer = 0;
	mesh.late_offer_at = 0;
	mesh.have_match = 0;
	mesh.give_up = 0;
	mesh.generation++;
	mesh.published = 0;
	mesh.grouped = 0;
	memset(mesh.reports, 0, sizeof(mesh.reports));
	mesh.seats = seats;
	mesh.game_humans = 0;
	mesh.game_machines = 0;
	/* The id is 16 bytes on the wire. A shorter playlist has to clear the
	tail of the previous one, or the compare misses every player. */
	memset(mesh.playlist, 0, sizeof(mesh.playlist));
	snprintf(mesh.playlist, sizeof(mesh.playlist), "%s", playlist);
	mesh.last_offer = 0;
	mesh.search_at = GetTickCount();
	mesh.phase_at = mesh.search_at;
	mesh.last_find = 0;
	ranked_seen_clear();
	clear_answers();
	if (peer_in_cooldown(mesh.self_until, unix_now()))
		mesh.phase = MESH_PHASE_COOLDOWN;
	else
		mesh.phase = MESH_PHASE_GATHER;
	pthread_mutex_unlock(&mesh_lock);
}

void peer_mesh_set_lobby_full(int full)
{
	pthread_mutex_lock(&mesh_lock);
	mesh.lobby_full = full ? 1 : 0;
	pthread_mutex_unlock(&mesh_lock);
}

void peer_mesh_set_game_humans(int humans)
{
	if (humans < 0)
		humans = 0;
	pthread_mutex_lock(&mesh_lock);
	mesh.game_humans = humans;
	pthread_mutex_unlock(&mesh_lock);
}

void peer_mesh_set_game_machines(int machines)
{
	if (machines < 0)
		machines = 0;
	pthread_mutex_lock(&mesh_lock);
	mesh.game_machines = machines;
	pthread_mutex_unlock(&mesh_lock);
}

void peer_mesh_set_rematch(const unsigned char id[16])
{
	if (!id)
		return;
	pthread_mutex_lock(&mesh_lock);
	memcpy(mesh.match_id, id, 16);
	mesh.have_match = 1;
	mesh.we_host = 1;
	mesh.searching = 1;
	/* the lobby already has its players, so another host's offer does not
	take this one. */
	mesh.grouped = 1;
	mesh.phase = MESH_PHASE_OFFER;
	mesh.phase_at = GetTickCount();
	mesh.late_offer = 0;
	mesh.late_offer_at = 0;
	mesh.lobby_full = 0;
	mesh.give_up = 0;
	mesh.want_host_note = 1;
	mesh.host_note_at = 0;
	mesh.last_offer = 0;
	pthread_mutex_unlock(&mesh_lock);
}

void peer_mesh_stop(void)
{
	pthread_mutex_lock(&mesh_lock);
	mesh.searching = 0;
	mesh.phase = MESH_PHASE_IDLE;
	mesh.want_host_note = 0;
	mesh.want_join_note = 0;
	mesh.want_redial = 0;
	mesh.give_up = 0;
	mesh.generation++;
	mesh.late_offer = 0;
	mesh.late_offer_at = 0;
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

static int level_for_key_locked(const unsigned char key[32])
{
	int index;

	for (index = 0; index < MESH_PEERS_MAX; index++)
	{
		struct mesh_peer *peer = &mesh.peers[index];

		if (!peer->used || !same_key(peer->key, key))
			continue;
		if (peer->looking_level_known)
			return peer->looking_level < 0 ? 0 : peer->looking_level;
		break;
	}
	return book_level(key);
}

/* the server numbers a second copy of a name ("New001 2"). that row is the
install that announced the name before the number. */
static int row_name_matches(const char *row, const char *announced)
{
	size_t length;
	const char *suffix;

	if (!row || !row[0] || !announced || !announced[0])
		return 0;
	if (strcmp(row, announced) == 0)
		return 1;
	length = strlen(announced);
	if (strncmp(row, announced, length) != 0 || row[length] != ' ')
		return 0;
	suffix = row + length + 1;
	if (!suffix[0])
		return 0;
	while (*suffix)
	{
		if (*suffix < '0' || *suffix > '9')
			return 0;
		suffix++;
	}
	return 1;
}

/* 1 keeps the only key. 2 is two installs, which is not one player's. */
static void note_name_key(unsigned char key[32], int *matches, const unsigned char *candidate)
{
	if (*matches == 1 && !same_key(key, candidate))
	{
		*matches = 2;
		return;
	}
	if (*matches == 0)
	{
		memcpy(key, candidate, 32);
		*matches = 1;
	}
}

int peer_mesh_player(const char *name, int machine, char *mesh_id, int mesh_id_size, int *level,
	unsigned long address)
{
	unsigned char key[32];
	char text[16];
	int found = 0;
	int score = 0;
	int index;

	if (mesh_id && mesh_id_size > 0)
		mesh_id[0] = 0;
	if (level)
		*level = -1;
	pthread_mutex_lock(&mesh_lock);
	if (machine >= 0 && machine < MESH_MACHINES && mesh.machine_have[machine])
	{
		memcpy(key, mesh.machine_key[machine], 32);
		found = 1;
	}
	/* two machines can share a gamertag. a row in the game is not this
	install just because the name matches. */
	if (!found && machine < 0 && name && name[0] && mesh.have_key && mesh.account_name[0] &&
		!strcmp(mesh.account_name, name))
	{
		memcpy(key, mesh.public_key, 32);
		found = 1;
	}
	if (!found && name && name[0])
	{
		int matches = 0;

		for (index = 0; index < MESH_PEERS_MAX; index++)
		{
			struct mesh_peer *peer = &mesh.peers[index];

			if (!peer->used || !row_name_matches(name, peer->looking_name))
				continue;
			note_name_key(key, &matches, peer->key);
		}
		for (index = 0; index < MESH_CANDIDATES; index++)
		{
			if (!mesh.candidates[index].used || !row_name_matches(name, mesh.candidates[index].name))
				continue;
			note_name_key(key, &matches, mesh.candidates[index].key);
		}
		found = matches == 1;
	}
	/* the name on the row can differ from the name that search announced.
	the game connection is the install that spoke from that address. two
	installs on one address stay unnamed. */
	if (!found && address)
	{
		int matches = 0;

		for (index = 0; index < MESH_PEERS_MAX; index++)
		{
			struct mesh_peer *peer = &mesh.peers[index];

			if (!peer->used || !peer->heard)
				continue;
			if (peer->address != address && peer->looking_address != address)
				continue;
			if (matches && !same_key(key, peer->key))
			{
				matches = 2;
				break;
			}
			memcpy(key, peer->key, 32);
			matches = 1;
		}
		found = matches == 1;
	}
	if (found)
		score = level_for_key_locked(key);
	pthread_mutex_unlock(&mesh_lock);
	if (!found)
		return 0;
	text[0] = 0;
	mesh_identity(key, NULL, 0, NULL, 0, text, (int)sizeof(text));
	if (!text[0])
		return 0;
	if (mesh_id && mesh_id_size > 0)
		snprintf(mesh_id, (size_t)mesh_id_size, "%s", text);
	if (level)
		*level = score < 0 ? 0 : score;
	return 1;
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
	/* A regular internet game has no mesh match. It does not change rank. */
	if (!mesh.have_match)
	{
		pthread_mutex_unlock(&mesh_lock);
		return;
	}
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
	unsigned char packets[MESH_TALLY_PARTS][MESH_MAX_PACKET];
	int sizes[MESH_TALLY_PARTS];
	int text_len;
	int count;
	int index;
	int sent = 0;

	if (!match_id || !match_id[0] || !tally)
		return 0;
	text_len = (int)strlen(tally);
	if (text_len <= 0 || text_len >= MESH_TALLY_TEXT)
		return 0;
	/* One packet when the text fits. A free for all of sixteen names does
	not, so the same text goes out in numbered pieces. */
	count = 8 + 86 + text_len + 64 <= MESH_MAX_PACKET ? 1 :
		(text_len + MESH_TALLY_CHUNK - 1) / MESH_TALLY_CHUNK;
	if (count < 1 || count > MESH_TALLY_PARTS)
		return 0;
	pthread_mutex_lock(&mesh_lock);
	load_identity();
	/* Games played and rank come from this mesh match only. A later game keeps
	its own scoreboard, even when an older match flag is still set. */
	if (!mesh.have_key || !mesh.have_match || !match_text_is(match_id, mesh.match_id))
	{
		pthread_mutex_unlock(&mesh_lock);
		return 0;
	}
	for (index = 0; index < count; index++)
	{
		int offset = index * MESH_TALLY_CHUNK;
		int part_len = count == 1 ? text_len : text_len - offset;
		int payload;
		unsigned char *packet = packets[index];

		if (count > 1 && part_len > MESH_TALLY_CHUNK)
			part_len = MESH_TALLY_CHUNK;
		payload = count == 1 ? 86 + part_len : 90 + part_len;
		if (part_len <= 0 || 8 + payload + 64 > MESH_MAX_PACKET)
		{
			pthread_mutex_unlock(&mesh_lock);
			return 0;
		}
		memset(packet, 0, MESH_MAX_PACKET);
		packet[0] = MESH_MAGIC0;
		packet[1] = MESH_MAGIC1;
		packet[2] = MESH_MAGIC2;
		packet[3] = MESH_MAGIC3;
		packet[4] = MESH_VERSION;
		packet[5] = count == 1 ? MESH_TALLY : MESH_TALLY_PART;
		write16(packet + 6, (unsigned short)payload);
		memcpy(packet + 8, mesh.public_key, 32);
		snprintf((char *)(packet + 40), 40, "%s", match_id);
		if (name)
			snprintf((char *)(packet + 80), 12, "%s", name);
		if (count == 1)
		{
			write16(packet + 92, (unsigned short)text_len);
			memcpy(packet + 94, tally, (size_t)text_len);
		}
		else
		{
			write16(packet + 92, (unsigned short)text_len);
			packet[94] = (unsigned char)index;
			packet[95] = (unsigned char)count;
			write16(packet + 96, (unsigned short)part_len);
			memcpy(packet + 98, tally + offset, (size_t)part_len);
		}
		sizes[index] = packet_finish(packet, payload);
		if (sizes[index] <= 0)
		{
			pthread_mutex_unlock(&mesh_lock);
			return 0;
		}
	}
	tally_store(mesh.public_key, match_id, name ? name : "", tally, packets[0], sizes[0]);
	if (count > 1)
		tally_attach_raws(mesh.public_key, match_id, tally, packets, sizes, count);
	for (index = 0; index < count; index++)
	{
		send_reachable(packets[index], sizes[index]);
		sent = 1;
	}
	pthread_mutex_unlock(&mesh_lock);
	return sent;
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

void peer_mesh_introduce(const unsigned char key[32], unsigned long address, unsigned short port)
{
	pthread_mutex_lock(&mesh_lock);
	introduce_locked(key, address, port);
	pthread_mutex_unlock(&mesh_lock);
}

void peer_mesh_greet(unsigned long address, unsigned short port)
{
	unsigned short dest;

	if (!address)
		return;
	dest = port ? port : net16(MESH_PORT);
	pthread_mutex_lock(&mesh_lock);
	load_identity();
	if (mesh.ready)
	{
		send_hello_to(address, dest);
		send_peers_to(address, dest);
	}
	pthread_mutex_unlock(&mesh_lock);
}

int peer_mesh_mapped(unsigned long *address, unsigned short *port)
{
	int have;

	pthread_mutex_lock(&mesh_lock);
	have = mesh.mapped && mesh.mapped_address && mesh.mapped_port;
	if (have)
	{
		if (address)
			*address = mesh.mapped_address;
		if (port)
			*port = mesh.mapped_port;
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

static int match_text_is(const char *text, const unsigned char id[16])
{
	char hex[40];
	int index;

	if (!text || !id)
		return 0;
	for (index = 0; index < 16; index++)
		snprintf(hex + index * 2, 3, "%02x", id[index]);
	return strcmp(hex, text) == 0;
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

int peer_mesh_match_is(const char *text)
{
	int same;

	pthread_mutex_lock(&mesh_lock);
	same = mesh.have_match && match_text_is(text, mesh.match_id);
	pthread_mutex_unlock(&mesh_lock);
	return same;
}

void peer_mesh_drop_match(void)
{
	pthread_mutex_lock(&mesh_lock);
	mesh.have_match = 0;
	pthread_mutex_unlock(&mesh_lock);
}

void peer_mesh_arm_late_offer(void)
{
	pthread_mutex_lock(&mesh_lock);
	if (mesh.we_host && mesh.have_match)
	{
		mesh.late_offer = 1;
		mesh.late_offer_at = 0;
	}
	pthread_mutex_unlock(&mesh_lock);
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
		send_reachable(packet, size);
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
	{
		int skip = 0;
		int sent_any = 0;

		/* One packet holds eight signatures. A larger game sends the rest
		in further packets, and the receivers add them before they decide. */
		while (skip < votes)
		{
			int size = 0;
			int status;

			kept = fill_report_packet(packet, target, players, skip, 8);
			if (kept < 1)
				break;
			payload = 114 + kept * 96;
			status = handle_report(packet + 8, payload);
			if (status > 0)
				size = packet_finish(packet, payload);
			if (size > 0)
			{
				send_reachable(packet, size);
				sent_any = 1;
			}
			skip += kept;
		}
		if (sent_any)
		{
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
			send_reachable(packet, size);
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
