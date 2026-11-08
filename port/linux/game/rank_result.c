/*
RANK_RESULT.C

When a matchmaking game reaches the scoreboard, this client writes the
scoreboard it calculated and hands that to the mesh. The mesh accepts a
rank change only when the other clients in the match calculated the same
one. The scoreboard shows each player's competitive level beside the
gamertag. The kill feed keeps the gamertag alone.
*/

#include "cseries.h"
#include "game/players.h"
#include "game/game_engine.h"
#include "memory/data.h"
#include "networking/network_game_globals.h"
#include "../src/rank_account.h"
#include "../src/matchmaking.h"
#include "../src/mesh_service.h"
#include "peer_mesh.h"
#include "rank_result.h"

#include <string.h>

enum
{
	RANK_REPORT_LIMIT = 16
};

void rank_match_report(void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	struct mesh_tally_row rows[RANK_REPORT_LIMIT];
	struct rank_account account;
	unsigned char keys[RANK_REPORT_LIMIT][32];
	int machines[RANK_REPORT_LIMIT];
	int humans;
	int key_count;
	int count;
	int index;
	int other;
	char tally[896];
	char match_id[40];
	char playlist[16];
	char token[80];
	int match_teams;

	if (!game_engine_showing_postgame())
		return;
	if (!rank_result_open())
		return;
	count = 0;
	humans = 0;
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		int name_index;
		int machine;
		int seen;
		int already;

		if (count >= RANK_REPORT_LIMIT)
			break;
		/* A bot seat is controller 4 or higher on that machine. The seat is
		in the player record every client has. */
		if (player->network_player_data.controller_index >= 4)
			continue;
		name_index = 0;
		while (name_index < 11 && player->name[name_index])
		{
			rows[count].name[name_index] = player->name[name_index] >= 32 && player->name[name_index] < 127 ?
				(char)player->name[name_index] : '?';
			name_index++;
		}
		rows[count].name[name_index] = 0;
		if (!rows[count].name[0])
			continue;
		rows[count].team = player->team_index < 0 ? 0 : (int)player->team_index;
		rows[count].quit = player->quit_out_of_game ? 1 : 0;
		rows[count].score = game_engine && game_engine->get_player_score ?
			(int)game_engine->get_player_score(iterator.datum_index, _get_score_individual) : 0;
		rows[count].place = 0;
		machine = player->network_player_data.machine_index;
		already = 0;
		if (machine >= 0)
		{
			for (seen = 0; seen < humans; seen++)
			{
				if (machines[seen] == machine)
					already = 1;
			}
			if (!already && humans < RANK_REPORT_LIMIT)
				machines[humans++] = machine;
		}
		else if (humans < RANK_REPORT_LIMIT)
			humans++;
		count++;
	}
	if (count < 1)
		return;
	if (!rank_result_context(match_id, (int)sizeof(match_id), playlist, (int)sizeof(playlist), token,
		(int)sizeof(token), &match_teams))
		return;
	for (index = 0; index < count; index++)
	{
		int better;

		if (!match_teams)
			rows[index].team = -1;
		if (rows[index].quit)
		{
			rows[index].place = 0;
			continue;
		}
		better = 0;
		for (other = 0; other < count; other++)
		{
			if (!rows[other].quit && rows[other].score > rows[index].score)
				better++;
		}
		rows[index].place = better + 1;
	}
	rank_account_get(&account);
	key_count = peer_mesh_roster(keys, RANK_REPORT_LIMIT);
	/* Wait until every machine on the scoreboard has sent its install id.
	A shorter list would be a different tally. */
	if (key_count != humans)
		return;
	if (!mesh_tally_text(match_id, playlist, keys, key_count, rows, count, tally, (int)sizeof(tally)))
		return;
	if (!peer_mesh_submit_tally(match_id, account.name, tally))
		return;
	rank_result_sent();
}

wchar_t const *rank_scoreboard_name(wchar_t const *name)
{
	static wchar_t shown[32];
	char ascii[16];
	int level;
	int index;
	int length;
	int offer_level;

	shown[0] = 0;
	if (!name)
		return shown;
	length = 0;
	while (name[length] && length < 31)
	{
		shown[length] = name[length];
		length++;
	}
	shown[length] = 0;
	index = 0;
	while (index < 11 && name[index])
	{
		ascii[index] = name[index] >= 32 && name[index] < 127 ? (char)name[index] : '?';
		index++;
	}
	ascii[index] = 0;
	offer_level = matchmaking_card_level(ascii);
	level = offer_level >= 0 ? offer_level : rank_level_named(ascii);
	if (level >= 0 && length + 6 < 32)
	{
		wchar_t extra[8];
		int extra_index;

		usnprintf(extra, 8, L" %d", level);
		extra[7] = 0;
		extra_index = 0;
		while (extra[extra_index] && length < 31)
			shown[length++] = extra[extra_index++];
		shown[length] = 0;
	}
	return shown;
}
