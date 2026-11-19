/* The host tells every other machine to play the map's own tags. A machine
that does not answer is dropped, so an older client cannot keep its own
tags in that game. The network version stays as it is. */
#include "cseries.h"
#include "errors.h"
#include "game/game.h"
#include "network_distributed.h"
#include "network_mods.h"
#include <string.h>

short network_distributed_server_machines(long *machine_indices, short maximum);
void network_game_server_kick_machine(long machine_index, boolean kept_out);
boolean network_connection_last_read_was_unreliable(void);
unsigned long system_milliseconds(void);
short network_game_client_get_local_machine_index(void);

/* the same bound as HALO_PORT_MAXIMUM_NETWORK_MACHINES */
#define MODS_MACHINES 128
/* ask again, then drop a machine that still has not answered */
#define MODS_RETRY_MILLISECONDS 2000
#define MODS_DROP_MILLISECONDS 5000

struct mods_policy_message
{
	struct distributed_message_header header;
	word allowed;
	word reserved;
};
typedef char mods_policy_message_size_assert[sizeof(struct mods_policy_message) == 12 ? 1 : -1];

/* NONE until this host has sent the choice. The choice is always the map's own tags. */
static short sent_allowed = NONE;
static byte acked[MODS_MACHINES];
static unsigned long asked_ms[MODS_MACHINES];
static unsigned long retry_ms[MODS_MACHINES];

static void network_mods_send(long machine_index, int allowed)
{
	struct mods_policy_message message;
	unsigned long now;

	if (machine_index < 0 || machine_index >= MODS_MACHINES)
		return;
	memset(&message, 0, sizeof(message));
	message.allowed = allowed ? 1 : 0;
	distributed_send_to_machine_reliably(machine_index, &message,
		_distributed_message_mods_policy, 1, sizeof(message));
	now = system_milliseconds();
	if (!asked_ms[machine_index])
		asked_ms[machine_index] = now ? now : 1;
	retry_ms[machine_index] = now;
}

static void network_mods_broadcast(int allowed)
{
	struct mods_policy_message message;

	memset(&message, 0, sizeof(message));
	message.allowed = allowed ? 1 : 0;
	distributed_send(&message, _distributed_message_mods_policy, 1,
		sizeof(message), _distributed_to_clients_reliably);
}

static void network_mods_clear_answers(void)
{
	short index;

	for (index = 0; index < MODS_MACHINES; index++)
	{
		acked[index] = 0;
		asked_ms[index] = 0;
		retry_ms[index] = 0;
	}
}

void network_mods_reset(void)
{
	sent_allowed = NONE;
	network_mods_clear_answers();
}

void network_mods_send_policy(long machine_index)
{
	if (game_connection() != _game_connection_network_server)
		return;
	sent_allowed = 0;
	if (machine_index >= 0 && machine_index < MODS_MACHINES)
	{
		acked[machine_index] = 0;
		asked_ms[machine_index] = 0;
	}
	network_mods_send(machine_index, 0);
}

void network_mods_update(void)
{
	short connection;
	long machines[MODS_MACHINES];
	short count;
	short index;
	unsigned long now;

	connection = game_connection();
	if (connection != _game_connection_network_server)
	{
		if (connection != _game_connection_network_client)
			sent_allowed = NONE;
		return;
	}
	if (sent_allowed != 0)
	{
		sent_allowed = 0;
		network_mods_clear_answers();
		network_mods_broadcast(0);
	}
	now = system_milliseconds();
	count = network_distributed_server_machines(machines, MODS_MACHINES);
	for (index = 0; index < count; index++)
	{
		long machine = machines[index];

		if (machine < 0 || machine >= MODS_MACHINES || acked[machine] ||
			machine == network_game_client_get_local_machine_index())
			continue;
		if (!asked_ms[machine] || now - retry_ms[machine] >= MODS_RETRY_MILLISECONDS)
			network_mods_send(machine, 0);
		if (asked_ms[machine] && now - asked_ms[machine] >= MODS_DROP_MILLISECONDS)
		{
			error(_error_log, "machine #%ld did not confirm the map's own tags: dropped", machine);
			network_game_server_kick_machine(machine, FALSE);
			acked[machine] = 1;
		}
	}
}

boolean network_mods_handles_message(word const *message, word size)
{
	struct distributed_message_header header;

	if (size < sizeof(header))
		return FALSE;
	memcpy(&header, message, sizeof(header));
	return header.type == _distributed_message_mods_policy ||
		header.type == _distributed_message_mods_ack;
}

void network_mods_handle_message(long machine_index, word const *data, word size)
{
	struct mods_policy_message message;

	if (network_connection_last_read_was_unreliable() || size != sizeof(message))
		return;
	memcpy(&message, data, sizeof(message));
	if (GET_MESSAGE_TYPE(message.header.header) != 2 || message.header.count != 1 ||
		message.allowed > 1 || message.reserved != 0)
		return;
	if (message.header.type == _distributed_message_mods_policy)
	{
		struct mods_policy_message answer;

		/* a client, and only from the host. another machine cannot set it. */
		if (machine_index != NONE || game_connection() != _game_connection_network_client)
			return;
		memset(&answer, 0, sizeof(answer));
		answer.allowed = message.allowed;
		distributed_send(&answer, _distributed_message_mods_ack, 1, sizeof(answer),
			_distributed_to_host_reliably);
		return;
	}
	if (message.header.type != _distributed_message_mods_ack ||
		game_connection() != _game_connection_network_server ||
		machine_index < 0 || machine_index >= MODS_MACHINES)
		return;
	if (sent_allowed == 0 || sent_allowed == 1)
	{
		if (message.allowed == (word)sent_allowed)
			acked[machine_index] = 1;
	}
}
