/*
RANK_RESULT.C

When a matchmaking game reaches the scoreboard, this client writes the
scoreboard it calculated and hands that to the mesh. The mesh accepts a
rank change only when the other clients in the match calculated the same
one. The scoreboard shows each player's competitive level beside the
gamertag. The Mesh ID is drawn after the gamertag in smaller characters.
The kill feed keeps the gamertag alone.
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
#include "rasterizer/rasterizer.h"
#include "text/draw_string.h"
#include "text/unicode.h"

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
	char key_names[RANK_REPORT_LIMIT][12];
	int machines[RANK_REPORT_LIMIT];
	int humans;
	int key_count;
	int roster;
	int missing;
	int count;
	int index;
	int other;
	char tally[PEER_MESH_TALLY_TEXT];
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
	key_count = 0;
	missing = 0;
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
		/* The host's tick, folded to first or later before the tally is
		written. This client's clock can move the time the player leaves
		the simulation, and that moved time is not the tally. */
		{
			long noted = network_game_client_quit_tick(iterator.datum_index);
			long tick = noted != NONE ? noted : player->quit_out_of_game_time;

			if (tick != NONE && tick > 0)
				rows[count].quit = tick > 0x7FFFFFFF ? 0x7FFFFFFF : (int)tick;
			else if (player->quit_out_of_game)
				rows[count].quit = 0x7FFFFFFF;
			else
				rows[count].quit = 0;
		}
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
		{
			unsigned char install[32];
			short local_machine = network_game_client_get_local_machine_index();
			int have = 0;
			int slot;

			/* This machine's install is its own key. Every other machine is
			the key that machine sent, including the host at its machine index.
			The gamertag is the alphabetically first name on that install, so
			every client writes the same line. */
			if (machine >= 0 && machine == (int)local_machine)
				have = peer_mesh_public(install);
			else if (machine >= 0)
				have = peer_mesh_machine_key(machine, install);
			else
				have = peer_mesh_public(install);
			if (!have)
				missing = 1;
			else
			{
				for (slot = 0; slot < key_count; slot++)
				{
					if (!memcmp(keys[slot], install, 32))
						break;
				}
				if (slot == key_count && key_count < RANK_REPORT_LIMIT)
				{
					memcpy(keys[key_count], install, 32);
					snprintf(key_names[key_count], 12, "%s", rows[count].name);
					key_count++;
				}
				else if (slot < key_count && strcmp(rows[count].name, key_names[slot]) < 0)
					snprintf(key_names[slot], 12, "%s", rows[count].name);
			}
		}
		count++;
	}
	if (count < 1 || missing)
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
	{
		unsigned char roster_keys[RANK_REPORT_LIMIT][32];

		roster = peer_mesh_roster(roster_keys, RANK_REPORT_LIMIT);
	}
	/* Wait until every machine on the scoreboard has sent its install id.
	A shorter list would be a different tally. */
	if (roster != humans || key_count != humans)
		return;
	/* 1 is a first leave. 2 is a leave after that. The raw tick stays off
	the tally, because each client's clock can move it. */
	{
		int earliest = 0;

		for (index = 0; index < count; index++)
		{
			if (rows[index].quit > 0 && (earliest == 0 || rows[index].quit < earliest))
				earliest = rows[index].quit;
		}
		for (index = 0; index < count; index++)
		{
			if (rows[index].quit <= 0)
				rows[index].quit = 0;
			else if (rows[index].quit == earliest)
				rows[index].quit = 1;
			else
				rows[index].quit = 2;
		}
	}
	if (!mesh_tally_text(match_id, playlist, keys, key_names, key_count, rows, count, tally, (int)sizeof(tally)))
		return;
	if (!peer_mesh_submit_tally(match_id, account.name, tally))
		return;
	rank_result_sent();
}

static int rank_name_level(wchar_t const *name)
{
	char ascii[16];
	int index;
	int offer_level;

	if (!name || !name[0])
		return -1;
	index = 0;
	while (index < 11 && name[index])
	{
		ascii[index] = name[index] >= 32 && name[index] < 127 ? (char)name[index] : '?';
		index++;
	}
	ascii[index] = 0;
	offer_level = matchmaking_card_level(ascii);
	return offer_level >= 0 ? offer_level : rank_level_named(ascii);
}

wchar_t const *rank_scoreboard_name(wchar_t const *name)
{
	static wchar_t shown[32];
	int level;
	int length;

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
	level = rank_name_level(name);
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

int rank_mesh_id_text(int machine, int local_install, wchar_t id[10])
{
	unsigned char key[32];
	char text[12];
	int index;
	int have;

	if (id)
		id[0] = 0;
	if (!id)
		return 0;
	have = 0;
	if (local_install)
		have = peer_mesh_public(key);
	else if (machine >= 0)
		have = peer_mesh_machine_key(machine, key);
	if (!have)
		return 0;
	mesh_identity(key, NULL, 0, NULL, 0, text, (int)sizeof(text));
	if (!text[0])
		return 0;
	for (index = 0; text[index] && index < 9; index++)
		id[index] = (wchar_t)(unsigned char)text[index];
	id[index] = 0;
	return index > 0;
}

void rank_player_install(struct player_datum const *player, int *machine, int *local_install)
{
	short local;
	int owned;

	if (machine)
		*machine = -1;
	if (local_install)
		*local_install = 0;
	if (!player)
		return;
	local = network_game_client_get_local_machine_index();
	owned = player->local_player_index != NONE ||
		(local != NONE && player->network_player_data.machine_index == (char)local);
	if (machine)
		*machine = (int)player->network_player_data.machine_index;
	if (local_install)
		*local_install = owned ? 1 : 0;
}

/* layout x whose screen position is screen, under the scale already set */
static short rank_layout_x(real screen, real scale, real origin_x)
{
	if (!(scale > 0.05f) || (scale > 0.999f && scale < 1.001f))
		return (short)screen;
	return (short)(origin_x + (screen - origin_x) / scale);
}

short rank_draw_name_tail(
	rectangle2d const *line_bounds,
	wchar_t const *name,
	int machine,
	int local_install,
	long font_index,
	real_argb_color const *color,
	int with_level)
{
	/* about two thirds of the gamertag, so the two read as different text */
	real const mesh_id_scale = 0.62f;
	real scale;
	real origin_x;
	real origin_y;
	rectangle2d ink;
	rectangle2d cursor;
	wchar_t id[10];
	short middle;
	real screen_right;
	real screen_mid;
	int have_id;
	int level;

	if (!line_bounds)
		return 0;
	if (!name || !name[0] || font_index == NONE || !color)
		return line_bounds->x0;
	rasterizer_text_get_scale(&scale, &origin_x, &origin_y);
	if (!(scale > 0.0f))
		scale = 1.0f;
	draw_unicode_string_compute_bounds(line_bounds, name, &ink, &cursor);
	if (ink.x0 > ink.x1)
		return line_bounds->x0;
	middle = draw_unicode_string_capital_middle(line_bounds, name);
	screen_right = origin_x + ((real)ink.x1 - origin_x) * scale;
	screen_mid = origin_y + ((real)middle - origin_y) * scale;
	have_id = rank_mesh_id_text(machine, local_install, id);
	if (have_id)
	{
		rectangle2d id_bounds;
		rectangle2d id_ink;
		rectangle2d id_cursor;
		real small;
		real id_origin_y;
		real denom;
		short id_middle;

		small = scale * mesh_id_scale;
		if (small < 0.2f)
			small = 0.2f;
		screen_right += 3.0f;
		id_bounds.x0 = (short)screen_right;
		id_bounds.x1 = (short)(id_bounds.x0 + 160);
		id_bounds.y0 = line_bounds->y0;
		id_bounds.y1 = line_bounds->y1;
		draw_string_set_draw_mode(font_index, NONE, 0, 0, color);
		id_middle = draw_unicode_string_capital_middle(&id_bounds, id);
		draw_unicode_string_compute_bounds(&id_bounds, id, &id_ink, &id_cursor);
		denom = 1.0f - small;
		if (denom > -0.05f && denom < 0.05f)
			id_origin_y = screen_mid;
		else
			id_origin_y = (screen_mid - (real)id_middle * small) / denom;
		rasterizer_text_set_scale(small, (real)id_bounds.x0, id_origin_y);
		rasterizer_draw_unicode_string(&id_bounds, NULL, NULL, 0, id);
		rasterizer_text_set_scale(scale, origin_x, origin_y);
		if (id_ink.x1 >= id_ink.x0)
			screen_right = (real)id_bounds.x0 + ((real)id_ink.x1 - (real)id_bounds.x0) * small;
		screen_right += 3.0f;
	}
	level = with_level ? rank_name_level(name) : -1;
	if (level >= 0)
	{
		wchar_t extra[8];
		rectangle2d level_bounds;
		rectangle2d level_ink;
		rectangle2d level_cursor;
		short layout_x;

		usnprintf(extra, 8, L" %d", level);
		extra[7] = 0;
		layout_x = rank_layout_x(screen_right, scale, origin_x);
		level_bounds.x0 = layout_x;
		level_bounds.x1 = (short)(layout_x + 64);
		level_bounds.y0 = line_bounds->y0;
		level_bounds.y1 = line_bounds->y1;
		draw_string_set_draw_mode(font_index, NONE, 0, 0, color);
		rasterizer_draw_unicode_string(&level_bounds, NULL, NULL, 0, extra);
		draw_unicode_string_compute_bounds(&level_bounds, extra, &level_ink, &level_cursor);
		if (level_ink.x1 >= level_ink.x0)
			screen_right = origin_x + ((real)level_ink.x1 - origin_x) * scale;
	}
	return rank_layout_x(screen_right, scale, origin_x);
}
