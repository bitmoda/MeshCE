/*
RANK_RESULT.C

When a matchmaking game reaches the scoreboard, the host publishes one
result. The scoreboard shows each player's competitive level beside the
gamertag. The kill feed keeps the gamertag alone.
*/

#include "cseries.h"
#include "game/players.h"
#include "game/game_engine.h"
#include "memory/data.h"
#include "networking/network_game_globals.h"
#include "../src/rank_account.h"
#include "../src/matchmaking.h"
#include "rank_result.h"

#include <string.h>

enum
{
	RANK_REPORT_LIMIT = 16
};

static int rank_name_listed(char names[][12], int count, const char *name)
{
	int index;

	for (index = 0; index < count; index++)
	{
		if (!strcmp(names[index], name))
			return 1;
	}
	return 0;
}

void rank_match_report(void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	char names[RANK_REPORT_LIMIT][12];
	char accounts[RANK_REPORT_LIMIT][48];
	int teams[RANK_REPORT_LIMIT];
	int quits[RANK_REPORT_LIMIT];
	long scores[RANK_REPORT_LIMIT];
	int places[RANK_REPORT_LIMIT];
	int count;
	int index;
	int other;
	int name_index;
	int card_index;
	int card_count;
	char body[2560];
	char piece[320];
	char escaped_name[32];
	char escaped_account[96];
	char escaped_token[160];
	char escaped_match[80];
	char escaped_playlist[32];
	char match_id[40];
	char playlist[16];
	char token[80];
	char card_name[12];
	char card_account[48];
	int match_teams;
	int used;
	int wrote;

	if (!game_engine_showing_postgame())
		return;
	if (!global_network_game_server_get())
		return;
	if (!rank_result_open())
		return;
	count = 0;
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		if (count >= RANK_REPORT_LIMIT)
			break;
		name_index = 0;
		while (name_index < 11 && player->name[name_index])
		{
			names[count][name_index] = player->name[name_index] >= 32 && player->name[name_index] < 127 ?
				(char)player->name[name_index] : '?';
			name_index++;
		}
		names[count][name_index] = 0;
		accounts[count][0] = 0;
		rank_account_for_name(names[count], accounts[count], 48);
		teams[count] = player->team_index < 0 ? 0 : (int)player->team_index;
		quits[count] = player->quit_out_of_game ? 1 : 0;
		scores[count] = game_engine && game_engine->get_player_score ?
			game_engine->get_player_score(iterator.datum_index, _get_score_individual) : 0;
		count++;
	}
	card_count = rank_match_card_count();
	for (card_index = 0; card_index < card_count && count < RANK_REPORT_LIMIT; card_index++)
	{
		if (!rank_match_card_at(card_index, card_name, (int)sizeof(card_name), card_account,
			(int)sizeof(card_account)))
			continue;
		if (!card_name[0] || rank_name_listed(names, count, card_name))
			continue;
		name_index = 0;
		while (name_index < 11 && card_name[name_index])
		{
			names[count][name_index] = card_name[name_index];
			name_index++;
		}
		names[count][name_index] = 0;
		name_index = 0;
		while (name_index < 47 && card_account[name_index])
		{
			accounts[count][name_index] = card_account[name_index];
			name_index++;
		}
		accounts[count][name_index] = 0;
		teams[count] = 0;
		quits[count] = 1;
		scores[count] = 0;
		count++;
	}
	if (count < 1)
		return;
	for (index = 0; index < count; index++)
	{
		int better;

		if (quits[index])
		{
			places[index] = 0;
			continue;
		}
		better = 0;
		for (other = 0; other < count; other++)
		{
			if (!quits[other] && scores[other] > scores[index])
				better++;
		}
		places[index] = better + 1;
	}
	if (!rank_result_context(match_id, (int)sizeof(match_id), playlist, (int)sizeof(playlist), token,
		(int)sizeof(token), &match_teams))
		return;
	rank_json_escape(escaped_match, (int)sizeof(escaped_match), match_id);
	rank_json_escape(escaped_playlist, (int)sizeof(escaped_playlist), playlist);
	rank_json_escape(escaped_token, (int)sizeof(escaped_token), token);
	wrote = snprintf(body, sizeof(body),
		"{\"token\":\"%s\",\"match\":\"%s\",\"playlist\":\"%s\",\"players\":[",
		escaped_token, escaped_match, escaped_playlist);
	if (wrote < 0 || wrote >= (int)sizeof(body))
		return;
	used = wrote;
	for (index = 0; index < count; index++)
	{
		rank_json_escape(escaped_name, (int)sizeof(escaped_name), names[index]);
		rank_json_escape(escaped_account, (int)sizeof(escaped_account), accounts[index]);
		if (match_teams && accounts[index][0])
			wrote = snprintf(piece, sizeof(piece),
				"%s{\"account\":\"%s\",\"name\":\"%s\",\"team\":%d,\"place\":%d,\"score\":%ld,\"quit\":%s}",
				index ? "," : "", escaped_account, escaped_name, teams[index], places[index], scores[index],
				quits[index] ? "true" : "false");
		else if (match_teams)
			wrote = snprintf(piece, sizeof(piece),
				"%s{\"name\":\"%s\",\"team\":%d,\"place\":%d,\"score\":%ld,\"quit\":%s}",
				index ? "," : "", escaped_name, teams[index], places[index], scores[index],
				quits[index] ? "true" : "false");
		else if (accounts[index][0])
			wrote = snprintf(piece, sizeof(piece),
				"%s{\"account\":\"%s\",\"name\":\"%s\",\"place\":%d,\"score\":%ld,\"quit\":%s}",
				index ? "," : "", escaped_account, escaped_name, places[index], scores[index],
				quits[index] ? "true" : "false");
		else
			wrote = snprintf(piece, sizeof(piece),
				"%s{\"name\":\"%s\",\"place\":%d,\"score\":%ld,\"quit\":%s}",
				index ? "," : "", escaped_name, places[index], scores[index],
				quits[index] ? "true" : "false");
		if (wrote < 0 || wrote >= (int)sizeof(piece) || used + wrote + 2 >= (int)sizeof(body))
			return;
		memcpy(body + used, piece, (size_t)wrote);
		used += wrote;
		body[used] = 0;
	}
	if (used + 2 >= (int)sizeof(body))
		return;
	body[used++] = ']';
	body[used++] = '}';
	body[used] = 0;
	rank_result_submit(body);
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
