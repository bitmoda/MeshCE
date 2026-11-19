/*
NETWORK_PEER.C

A machine tells the match its peer id, and a vote to kick signs a report
against that id. The host keeps the reports. More than half the players, and
at least two, ban the id on the peer mesh. That is the same count that
removes the player from the game. A vote that ends short of that count is
still reported. Thirty three of those votes ban the id, starting at the
middle of the cooldown. The network version stays as it is: a machine that
does not know these messages drops them.
*/

#include "cseries.h"
#include "game/game.h"
#include "networking/network_game_globals.h"
#include "network_distributed.h"
#include "peer_mesh.h"
#include "rank_result.h"

#include <string.h>

static short peer_vote_pending = -1;
static short peer_vote_left = 0;
static long peer_vote_sent = NONE;
static long peer_id_host_sent = NONE;
static long peer_id_client_sent = NONE;
static unsigned long peer_lobby_sent = 0;
/* a kick vote that already has enough votes; the ban waits for its reports */
static long peer_ban_machine = NONE;
static int peer_ban_players = 0;
static int peer_target_known = 0;
static unsigned char peer_target_key[32];
static int peer_ban_key_known = 0;
static unsigned char peer_ban_key[32];

int peer_report_accepted(int votes, int players);

static unsigned char const peer_id_text[16] =
{
	'O', 'p', 'e', 'n', 'C', 'E', ' ', 'p', 'e', 'e', 'r', ' ', 'i', 'd', ' ', '1'
};

static unsigned char const peer_report_text[16] =
{
	'O', 'p', 'e', 'n', 'C', 'E', ' ', 'r', 'e', 'p', 'o', 'r', 't', ' ', 'v', '1'
};

static int peer_id_body(unsigned char *body, unsigned char const *key)
{
	csmemset(body, 0, 48);
	csmemcpy(body, peer_id_text, 16);
	csmemcpy(body + 16, key, 32);
	return 48;
}

/* The machine index is the last signed byte. A relay that moves the key
onto another machine fails this check. */
static void peer_id_sign_machine(unsigned char *signed_bytes, unsigned char machine)
{
	signed_bytes[48] = machine;
}

static int peer_report_body(unsigned char *body, unsigned char const *target)
{
	unsigned char match[16];
	unsigned char host[32];

	csmemset(body, 0, 96);
	csmemcpy(body, peer_report_text, 16);
	csmemcpy(body + 16, target, 32);
	csmemset(match, 0, sizeof(match));
	peer_mesh_match(match);
	csmemcpy(body + 48, match, 16);
	if (global_network_game_server_get())
	{
		if (!peer_mesh_public(host))
			return 0;
	}
	else if (!peer_mesh_machine_key(-1, host))
		return 0;
	csmemcpy(body + 64, host, 32);
	return 96;
}

static void peer_send_id(int host)
{
	struct
	{
		struct distributed_message_header header;
		unsigned char machine;
		unsigned char key[32];
		unsigned char signature[64];
	} message;
	unsigned char key[32];
	unsigned char body[49];
	short local = network_game_client_get_local_machine_index();
	long *sent = host ? &peer_id_host_sent : &peer_id_client_sent;

	if (*sent != NONE && game_time_get() - *sent < 2 * TICKS_PER_SECOND)
		return;
	if (!host && (local < 0 || local > 254))
		return;
	if (!peer_mesh_public(key))
		return;
	peer_id_body(body, key);
	csmemset(&message, 0, sizeof(message));
	message.machine = host ? 255 : (unsigned char)local;
	peer_id_sign_machine(body, message.machine);
	csmemcpy(message.key, key, 32);
	if (!peer_mesh_sign(body, 49, message.signature))
		return;
	*sent = game_time_get();
	distributed_send(&message, _distributed_message_peer_id, 1, (word)sizeof(message),
		host ? _distributed_to_clients_reliably : _distributed_to_host_reliably);
	/* 255 names the host for a report. The same key at this machine's index
	is how the other clients pair the host with the players on the scoreboard. */
	if (host && local >= 0 && local < 255)
	{
		message.machine = (unsigned char)local;
		peer_id_sign_machine(body, message.machine);
		if (peer_mesh_sign(body, 49, message.signature))
			distributed_send(&message, _distributed_message_peer_id, 1, (word)sizeof(message),
				_distributed_to_clients_reliably);
	}
}

void network_peer_new_game(void)
{
	peer_vote_pending = -1;
	peer_vote_left = 0;
	peer_vote_sent = NONE;
	peer_id_host_sent = NONE;
	peer_id_client_sent = NONE;
	peer_lobby_sent = 0;
	if (peer_ban_machine != NONE)
		network_peer_consider(peer_ban_machine, peer_ban_players);
	peer_ban_machine = NONE;
	peer_ban_players = 0;
	peer_ban_key_known = 0;
	peer_target_known = 0;
	peer_mesh_clear_game();
}

static int peer_vote_due(void)
{
	if (peer_vote_pending < 0)
		return 0;
	if (peer_vote_sent == NONE)
		return 1;
	return game_time_get() - peer_vote_sent >= 2 * TICKS_PER_SECOND;
}

void network_peer_host_tick(void)
{
	peer_send_id(1);
	rank_match_report();
	if (peer_ban_machine != NONE)
		network_peer_consider(peer_ban_machine, peer_ban_players);
	if (peer_vote_due())
		network_peer_vote(peer_vote_pending);
}

void network_peer_client_tick(void)
{
	if (global_network_game_server_get())
		return;
	peer_send_id(0);
	rank_match_report();
	if (peer_vote_due())
		network_peer_vote(peer_vote_pending);
}

/* The lobby's clock does not move, so the in-game send would go out once.
The ids are what the lobby rows show. */
void network_peer_lobby_tick(void)
{
	unsigned long now = system_milliseconds();

	if (peer_lobby_sent != 0 && now - peer_lobby_sent < 1000)
		return;
	peer_lobby_sent = now;
	peer_id_host_sent = NONE;
	peer_id_client_sent = NONE;
	if (global_network_game_server_get())
		peer_send_id(1);
	else if (game_connection() == _game_connection_network_client)
		peer_send_id(0);
}

boolean network_peer_handles_message(
	word const *message,
	word size)
{
	struct distributed_message_header header;

	if (!message || size < sizeof(header))
		return FALSE;
	csmemcpy(&header, message, sizeof(header));
	return header.type == _distributed_message_peer_id;
}

word network_peer_id_entry_size(void)
{
	return (word)(1 + 32 + 64);
}

word network_peer_report_entry_size(void)
{
	return (word)(32 + 64);
}

void network_peer_handle_id(long machine_index, void const *entries, int from_stream)
{
	unsigned char const *bytes;
	unsigned char body[49];
	struct
	{
		struct distributed_message_header header;
		unsigned char machine;
		unsigned char key[32];
		unsigned char signature[64];
	} message;

	if (!entries)
		return;
	bytes = (unsigned char const *)entries;
	peer_id_body(body, bytes + 1);
	peer_id_sign_machine(body, bytes[0]);
	if (!peer_mesh_verify(bytes + 1, body, 49, bytes + 33))
		return;
	if (game_connection() == _game_connection_network_server)
	{
		unsigned char self_key[32];

		if (!from_stream || machine_index < 0 || machine_index > 255)
			return;
		/* The signed index is the connection this packet arrived on. */
		if (bytes[0] != (unsigned char)machine_index)
			return;
		/* A client can repeat this host's id. That does not put the host's
		install on the client's machine. */
		if (peer_mesh_public(self_key) && !memcmp(self_key, bytes + 1, 32))
			return;
		peer_mesh_note_machine((int)machine_index, bytes + 1);
		csmemset(&message, 0, sizeof(message));
		message.machine = bytes[0];
		csmemcpy(message.key, bytes + 1, 32);
		csmemcpy(message.signature, bytes + 33, 64);
		distributed_send(&message, _distributed_message_peer_id, 1, (word)sizeof(message),
			_distributed_to_clients_reliably);
		return;
	}
	if (bytes[0] == 255)
		peer_mesh_note_machine(-1, bytes + 1);
	else
		peer_mesh_note_machine((int)bytes[0], bytes + 1);
}

void network_peer_handle_report(long machine_index, void const *entries, int from_stream)
{
	unsigned char const *bytes;
	unsigned char voter[32];

	if (!entries || !from_stream || machine_index < 0)
		return;
	if (game_connection() != _game_connection_network_server)
		return;
	bytes = (unsigned char const *)entries;
	if (!peer_mesh_machine_key((int)machine_index, voter))
		return;
	peer_mesh_add_report(voter, bytes, bytes + 32);
	if (peer_ban_machine != NONE)
		network_peer_consider(peer_ban_machine, peer_ban_players);
}

void network_peer_vote(short player_index)
{
	long machine;
	int body_size;
	unsigned char target[32];
	unsigned char self_key[32];
	unsigned char body[96];
	unsigned char signature[64];
	struct
	{
		struct distributed_message_header header;
		unsigned char target[32];
		unsigned char signature[64];
	} message;

	if (!distributed_player(player_index))
	{
		peer_vote_pending = -1;
		peer_vote_left = 0;
		peer_vote_sent = NONE;
		return;
	}
	machine = distributed_player_machine(player_index);
	if (machine < 0 || !peer_mesh_machine_key((int)machine, target) || !peer_mesh_public(self_key))
	{
		peer_vote_pending = player_index;
		return;
	}
	body_size = peer_report_body(body, target);
	if (body_size <= 0 || !peer_mesh_sign(body, body_size, signature))
	{
		peer_vote_pending = player_index;
		return;
	}
	/* The host is not the only copy of this vote. The voter sends it too. */
	peer_mesh_cast_vote(target, body + 64, signature);
	if (global_network_game_server_get())
	{
		peer_mesh_add_report(self_key, target, signature);
		peer_vote_pending = -1;
		peer_vote_left = 0;
		peer_vote_sent = NONE;
		if (peer_ban_machine != NONE)
			network_peer_consider(peer_ban_machine, peer_ban_players);
		return;
	}
	csmemset(&message, 0, sizeof(message));
	csmemcpy(message.target, target, 32);
	csmemcpy(message.signature, signature, 64);
	distributed_send(&message, _distributed_message_peer_report, 1, (word)sizeof(message),
		_distributed_to_host_reliably);
	/* The host drops this until it knows the voter's id, so it is sent again. */
	peer_vote_sent = game_time_get();
	if (peer_vote_left <= 0)
		peer_vote_left = 4;
	else
		peer_vote_left--;
	if (peer_vote_left > 0)
		peer_vote_pending = player_index;
	else
	{
		peer_vote_pending = -1;
		peer_vote_left = 0;
	}
}

static int peer_vote_key(long target_machine, unsigned char target[32])
{
	unsigned char live[32];
	int live_have;

	live_have = 0;
	if (target_machine >= 0 && peer_mesh_machine_key((int)target_machine, live))
		live_have = 1;
	/* the slot can belong to someone else after the voted player leaves */
	if (peer_target_known && (!live_have || csmemcmp(live, peer_target_key, 32) != 0))
	{
		csmemcpy(target, peer_target_key, 32);
		return 1;
	}
	if (!live_have)
		return 0;
	csmemcpy(target, live, 32);
	return 1;
}

void network_peer_consider(long target_machine, int players)
{
	unsigned char target[32];
	int votes;

	if (players <= 0 || !global_network_game_server_get())
		return;
	if (peer_ban_key_known)
		csmemcpy(target, peer_ban_key, 32);
	else if (!peer_vote_key(target_machine, target))
		return;
	votes = peer_mesh_report_count(target);
	if (!peer_report_accepted(votes, players))
		return;
	peer_mesh_publish_ban(target, votes, players);
	peer_ban_machine = NONE;
	peer_ban_key_known = 0;
}

void network_peer_ban_due(long target_machine, int players)
{
	unsigned char target[32];

	if (target_machine < 0 || players <= 0)
		return;
	if (peer_ban_machine != target_machine)
	{
		peer_ban_machine = target_machine;
		peer_ban_players = players;
		peer_ban_key_known = 0;
	}
	if (!peer_ban_key_known && peer_vote_key(target_machine, target))
	{
		csmemcpy(peer_ban_key, target, 32);
		peer_ban_key_known = 1;
	}
	network_peer_consider(peer_ban_machine, peer_ban_players);
}

void network_peer_remember_target(long target_machine)
{
	unsigned char key[32];

	if (target_machine < 0 || !peer_mesh_machine_key((int)target_machine, key))
		return;
	csmemcpy(peer_target_key, key, 32);
	peer_target_known = 1;
}

void network_peer_forget_target(void)
{
	peer_target_known = 0;
}

void network_peer_publish_strikes(long target_machine, int players)
{
	unsigned char target[32];

	if (players <= 0 || !global_network_game_server_get())
		return;
	if (peer_target_known)
	{
		peer_mesh_publish_strikes(peer_target_key, players);
		return;
	}
	if (target_machine >= 0 && peer_mesh_machine_key((int)target_machine, target))
		peer_mesh_publish_strikes(target, players);
}
