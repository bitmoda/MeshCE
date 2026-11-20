/*
MESH_SERVICE.C

The broker's matchmaking and rank rules, with no sockets and no files.
*/

#include "mesh_service.h"

#include <stdio.h>
#include <string.h>

static int hex_write(char *out, int size, const unsigned char *bytes, int count)
{
	int index;

	if (!out || size < count * 2 + 1)
		return 0;
	for (index = 0; index < count; index++)
		snprintf(out + index * 2, 3, "%02x", bytes[index]);
	return 1;
}

static int ascii_letter(unsigned char character)
{
	return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
}

static int ascii_digit(unsigned char character)
{
	return character >= '0' && character <= '9';
}

static int latin_letter(const unsigned char *text, int *used)
{
	if (text[0] < 0x80)
	{
		*used = 1;
		return 0;
	}
	if ((text[0] == 0xC3 && text[1] >= 0x80 && text[1] <= 0xBF && text[1] != 0x97 && text[1] != 0xB7) ||
		(text[0] == 0xC4 && text[1] >= 0x80 && text[1] <= 0xBF) ||
		(text[0] == 0xC5 && text[1] >= 0x80 && text[1] <= 0xBF))
	{
		*used = 2;
		return 1;
	}
	*used = 1;
	return 0;
}

int mesh_gamertag_ok(const char *name)
{
	const unsigned char *text = (const unsigned char *)name;
	int characters = 0;
	int marks = 0;
	int length;

	if (!name || !name[0])
		return 0;
	length = (int)strlen(name);
	if (name[0] == ' ' || name[length - 1] == ' ')
		return 0;
	while (*text)
	{
		int used = 1;

		if (*text == '|')
			return 0;
		if (*text < 0x80)
		{
			if (*text < 0x20 || *text > 0x7E)
				return 0;
			if (ascii_letter(*text) || ascii_digit(*text))
				marks = 1;
			used = 1;
		}
		else if (latin_letter(text, &used))
			marks = 1;
		else
			return 0;
		text += used;
		characters++;
		if (characters > 11)
			return 0;
	}
	return marks;
}

void mesh_identity(const unsigned char key[32], char *account, int account_size,
	char *token, int token_size, char *mesh_id, int mesh_size)
{
	if (account && account_size > 0)
		account[0] = 0;
	if (token && token_size > 0)
		token[0] = 0;
	if (mesh_id && mesh_size > 0)
		mesh_id[0] = 0;
	if (!key)
		return;
	hex_write(account, account_size, key, 16);
	hex_write(token, token_size, key + 16, 16);
	if (mesh_id && mesh_size > 9)
		snprintf(mesh_id, (size_t)mesh_size, "M%02X%02X%02X%02X", key[0], key[1], key[2], key[3]);
}

static const char *json_value(const char *json, const char *key)
{
	char pattern[32];
	const char *found;

	if (!json || !key)
		return NULL;
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	found = strstr(json, pattern);
	if (!found)
		return NULL;
	found += strlen(pattern);
	while (*found == ' ' || *found == '\t')
		found++;
	if (*found != ':')
		return NULL;
	found++;
	while (*found == ' ' || *found == '\t')
		found++;
	return found;
}

static int json_copy_string(const char *json, const char *key, char *out, int size)
{
	const char *value = json_value(json, key);
	int index = 0;

	if (!out || size < 2)
		return 0;
	out[0] = 0;
	if (!value || *value != '"')
		return 0;
	value++;
	while (*value && *value != '"' && index < size - 1)
	{
		if (*value == '\\' && value[1])
			value++;
		out[index++] = *value++;
	}
	out[index] = 0;
	return index > 0;
}

int mesh_register_reply(const char *request, const unsigned char key[32], char *reply, int size)
{
	char name[16];
	char account[40];
	char token[40];
	char mesh_id[16];

	if (!reply || size < 32)
		return 0;
	reply[0] = 0;
	if (!key)
		return 0;
	name[0] = 0;
	json_copy_string(request, "gamertag", name, (int)sizeof(name));
	if (!mesh_gamertag_ok(name))
	{
		snprintf(reply, (size_t)size, "{\"ok\":false,\"error\":\"gamertag\"}");
		return 1;
	}
	mesh_identity(key, account, (int)sizeof(account), token, (int)sizeof(token), mesh_id, (int)sizeof(mesh_id));
	snprintf(reply, (size_t)size, "{\"account\":\"%s\",\"token\":\"%s\",\"core\":\"%s\"}", account, token, mesh_id);
	return reply[0] ? 1 : 0;
}

void mesh_rank_window(int level, int steps, int *floor_out, int *ceiling_out)
{
	int floor;
	int ceiling;
	int step;

	if (level < 0)
		level = 0;
	if (steps < 0)
		steps = 0;
	ceiling = level;
	if (level < 50)
		floor = (level / 10) * 10;
	else
	{
		int block = 50 + ((level - 50) / 100) * 100;

		floor = block - 100;
		if (floor < 50)
			floor = 50;
	}
	for (step = 0; step < steps; step++)
	{
		if (level >= 50 && step == 0)
		{
			if (floor > 350)
				floor = 350;
			else
				floor -= 100;
		}
		else if (floor > 50)
			floor -= level < 50 ? 10 : 100;
		else
			floor -= 10;
		if (floor < 0)
			floor = 0;
	}
	if (floor < 0)
		floor = 0;
	if (floor > ceiling)
		floor = ceiling;
	if (floor_out)
		*floor_out = floor;
	if (ceiling_out)
		*ceiling_out = ceiling;
}

int mesh_rank_steps(int elapsed_seconds)
{
	if (elapsed_seconds < 300)
		return 0;
	return 1 + (elapsed_seconds - 300) / 180;
}

int mesh_ranked_together(int level_a, int steps_a, int level_b, int steps_b)
{
	int high_level;
	int high_steps;
	int low_level;
	int floor;
	int ceiling;

	if (level_a >= level_b)
	{
		high_level = level_a;
		high_steps = steps_a;
		low_level = level_b;
	}
	else
	{
		high_level = level_b;
		high_steps = steps_b;
		low_level = level_a;
	}
	mesh_rank_window(high_level, high_steps, &floor, &ceiling);
	return low_level >= floor && low_level <= ceiling;
}

int mesh_solo_yields(int ranked, int our_level, int their_level, int their_key_lower)
{
	if (!ranked)
		return their_key_lower ? 1 : 0;
	if (their_level > our_level)
		return 1;
	if (their_level < our_level)
		return 0;
	return their_key_lower ? 1 : 0;
}

void mesh_level_apply(int level, int wins, int losses, int outcome,
	int *level_out, int *wins_out, int *losses_out)
{
	int next;

	if (level < 0)
		level = 0;
	if (wins < 0)
		wins = 0;
	if (losses < 0)
		losses = 0;
	if (outcome < 0)
	{
		if (level_out)
			*level_out = level;
		if (wins_out)
			*wins_out = wins;
		if (losses_out)
			*losses_out = losses;
		return;
	}
	if (outcome)
	{
		wins++;
		losses = 0;
	}
	else
	{
		losses++;
		wins = 0;
	}
	next = level;
	if (level < 50)
	{
		if (wins >= 2)
			next = level + 1;
		else if (losses >= 3 && level > 0)
			next = level - 1;
	}
	else if (level == 50)
	{
		if (wins >= 3)
			next = 51;
	}
	else
	{
		if (wins >= 3)
			next = level + 1;
		else if (losses >= 1)
			next = level - 1;
		if (next < 50)
			next = 50;
	}
	if (next != level)
	{
		wins = 0;
		losses = 0;
	}
	if (level_out)
		*level_out = next;
	if (wins_out)
		*wins_out = wins;
	if (losses_out)
		*losses_out = losses;
}

int mesh_games_apply(const char *playlist, int games, int quit)
{
	if (games < 0)
		games = 0;
	if (quit || !playlist)
		return games;
	if (!strcmp(playlist, "ffa") || !strcmp(playlist, "hardcore"))
		return games + 1;
	return games;
}

int mesh_wait_seconds(const char *playlist, int humans, int cap, int elapsed_seconds)
{
	if (cap < 1)
		cap = 1;
	(void)elapsed_seconds;
	if (humans >= cap)
		return 30;
	/* free for all is ready as soon as two people are seated. the lobby holds sixteen. */
	if (playlist && !strcmp(playlist, "ffa") && humans >= 2)
		return 30;
	return -1;
}

int mesh_seats_unfilled(int advertised, int cap, int present)
{
	if (cap < 1)
		cap = 1;
	if (present < 0)
		present = 0;
	if (advertised < cap)
		return 0;
	return present < cap;
}

const char *mesh_map_for(const unsigned char match[16], const char *playlist)
{
	static const char *const arena[] = {
		"hangemhigh", "prisoner", "beavercreek", "carousel", "chillout", "damnation"
	};
	static const char *const ffa[] = {
		"hangemhigh", "prisoner", "beavercreek", "carousel", "chillout",
		"sidewinder", "damnation", "ratrace", "boardingaction", "bloodgulch",
		"wizard", "putput", "longest"
	};
	const char *const *maps = arena;
	int count = (int)(sizeof(arena) / sizeof(arena[0]));

	if (playlist && !strcmp(playlist, "ffa"))
	{
		maps = ffa;
		count = (int)(sizeof(ffa) / sizeof(ffa[0]));
	}
	if (!match)
		return maps[0];
	return maps[match[0] % count];
}

int mesh_score(const char *playlist)
{
	return playlist && (!strcmp(playlist, "hardcore") || !strcmp(playlist, "ranked")) ? 25 : 50;
}

void mesh_loadout(const unsigned char match[16], const char *playlist, int *primary, int *secondary)
{
	int sniper = playlist && !strcmp(playlist, "ffa") && match && (match[0] % 3) == 0;

	if (primary)
		*primary = sniper ? 5 : 2;
	if (secondary)
		*secondary = 3;
}

struct mesh_row
{
	char account[48];
	char name[12];
	int quit;
	int score;
	int place;
	int team;
	int have_place;
	int have_team;
};

static int object_number(const char *object, const char *end, const char *key, int *value)
{
	const char *limit = end ? end : object + strlen(object);
	char pattern[32];
	const char *found;
	int number = 0;
	int sign = 1;

	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	found = object;
	while (found && found < limit)
	{
		const char *next = strstr(found, pattern);

		if (!next || next >= limit)
			return 0;
		next += strlen(pattern);
		while (next < limit && (*next == ' ' || *next == '\t'))
			next++;
		if (next < limit && *next == ':')
		{
			next++;
			while (next < limit && (*next == ' ' || *next == '\t'))
				next++;
			if (next < limit && *next == '-')
			{
				sign = -1;
				next++;
			}
			if (next >= limit || *next < '0' || *next > '9')
				return 0;
			while (next < limit && *next >= '0' && *next <= '9')
				number = number * 10 + (*next++ - '0');
			*value = number * sign;
			return 1;
		}
		found = next;
	}
	return 0;
}

static int object_text(const char *object, const char *end, const char *key, char *out, int size)
{
	const char *limit = end ? end : object + strlen(object);
	char pattern[32];
	const char *found;
	int index = 0;

	if (!out || size < 2)
		return 0;
	out[0] = 0;
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	found = strstr(object, pattern);
	if (!found || found >= limit)
		return 0;
	found += strlen(pattern);
	while (found < limit && (*found == ' ' || *found == '\t'))
		found++;
	if (found >= limit || *found != ':')
		return 0;
	found++;
	while (found < limit && (*found == ' ' || *found == '\t'))
		found++;
	if (found >= limit || *found != '"')
		return 0;
	found++;
	while (found < limit && *found != '"' && index < size - 1)
		out[index++] = *found++;
	out[index] = 0;
	return index > 0;
}

static int object_quit(const char *object, const char *end)
{
	const char *limit = end ? end : object + strlen(object);
	const char *found = strstr(object, "\"quit\"");

	if (!found || found >= limit)
		return 0;
	found = strchr(found, ':');
	if (!found || found >= limit)
		return 0;
	found++;
	while (found < limit && (*found == ' ' || *found == '\t'))
		found++;
	return found + 4 <= limit && !strncmp(found, "true", 4);
}

static int read_rows(const char *body, char *playlist, int playlist_size, struct mesh_row *rows, int max)
{
	const char *players;
	const char *cursor;
	int count = 0;

	if (playlist && playlist_size > 0)
		playlist[0] = 0;
	if (!body)
		return 0;
	json_copy_string(body, "playlist", playlist, playlist_size);
	players = strstr(body, "\"players\"");
	if (!players)
		return 0;
	cursor = strchr(players, '[');
	if (!cursor)
		return 0;
	cursor++;
	while (*cursor && count < max)
	{
		const char *start;
		const char *finish;
		int number;

		while (*cursor && *cursor != '{' && *cursor != ']')
			cursor++;
		if (*cursor != '{')
			break;
		start = cursor;
		finish = strchr(start, '}');
		if (!finish)
			break;
		memset(&rows[count], 0, sizeof(rows[count]));
		rows[count].place = -1;
		rows[count].team = -1;
		object_text(start, finish, "account", rows[count].account, (int)sizeof(rows[count].account));
		object_text(start, finish, "name", rows[count].name, (int)sizeof(rows[count].name));
		rows[count].quit = object_quit(start, finish);
		if (object_number(start, finish, "score", &number))
			rows[count].score = number;
		if (object_number(start, finish, "place", &number))
		{
			rows[count].place = number;
			rows[count].have_place = 1;
		}
		if (object_number(start, finish, "team", &number))
		{
			rows[count].team = number;
			rows[count].have_team = 1;
		}
		count++;
		cursor = finish + 1;
	}
	return count;
}

static int row_outcome(const struct mesh_row *rows, int count, int index)
{
	int teams = 0;
	int places = 0;
	int row;
	int best_place = 0;
	int best_score = 0;
	int best_count = 0;
	int team_score[16];
	int team_seen[16];
	int best_team = 0;
	int best_team_score = 0;
	int best_team_count = 0;

	if (index < 0 || index >= count)
		return 0;
	if (rows[index].quit)
		return 0;
	for (row = 0; row < count; row++)
	{
		if (rows[row].have_team)
			teams = 1;
		if (rows[row].have_place)
			places = 1;
	}
	if (teams)
	{
		memset(team_score, 0, sizeof(team_score));
		memset(team_seen, 0, sizeof(team_seen));
		for (row = 0; row < count; row++)
		{
			int team = rows[row].have_team ? rows[row].team : 0;

			if (team < 0 || team >= 16)
				continue;
			team_seen[team] = 1;
			team_score[team] += rows[row].score;
		}
		for (row = 0; row < 16; row++)
		{
			if (!team_seen[row])
				continue;
			if (best_team_count == 0 || team_score[row] > best_team_score)
			{
				best_team_score = team_score[row];
				best_team = row;
				best_team_count = 1;
			}
			else if (team_score[row] == best_team_score)
				best_team_count++;
		}
		if (best_team_count != 1)
			return -1;
		return (rows[index].have_team ? rows[index].team : 0) == best_team ? 1 : 0;
	}
	for (row = 0; row < count; row++)
	{
		if (rows[row].quit)
			continue;
		if (places)
		{
			if (!rows[row].have_place)
				continue;
			if (best_count == 0 || rows[row].place < best_place)
			{
				best_place = rows[row].place;
				best_count = 1;
			}
			else if (rows[row].place == best_place)
				best_count++;
		}
		else
		{
			if (best_count == 0 || rows[row].score > best_score)
			{
				best_score = rows[row].score;
				best_count = 1;
			}
			else if (rows[row].score == best_score)
				best_count++;
		}
	}
	if (best_count != 1)
	{
		/* The players who share first stay as they are. Everyone else loses. */
		if (places)
			return rows[index].have_place && rows[index].place == best_place ? -1 : 0;
		return rows[index].score == best_score ? -1 : 0;
	}
	if (places)
		return rows[index].have_place && rows[index].place == best_place ? 1 : 0;
	return rows[index].score == best_score ? 1 : 0;
}

int mesh_result_apply(const char *body, const char *account, const char *name, int *games, int *level,
	int *wins, int *losses)
{
	struct mesh_row rows[16];
	char playlist[16];
	int count;
	int index;
	int found = -1;
	int named = -1;
	int named_count = 0;
	int outcome;

	if (!account || !account[0] || !games || !level || !wins || !losses)
		return 0;
	count = read_rows(body, playlist, (int)sizeof(playlist), rows, 16);
	for (index = 0; index < count; index++)
	{
		if (rows[index].account[0] && !strcmp(rows[index].account, account))
			found = index;
		if (!name || !name[0] || !rows[index].name[0] || strcmp(rows[index].name, name) != 0)
			continue;
		if (rows[index].account[0] && strcmp(rows[index].account, account) != 0)
			continue;
		named = index;
		named_count++;
	}
	/* The host often knows the gamertag and not the account. Two rows with
	the same gamertag are left alone, including when both carry this account. */
	if (named_count > 1)
		return 0;
	if (found < 0 && named_count == 1)
		found = named;
	if (found < 0)
		return 0;
	/* someone left. the scoreboard can still name a winner. the list stays. */
	for (index = 0; index < count; index++)
	{
		if (rows[index].quit)
			return 1;
	}
	*games = mesh_games_apply(playlist, *games, rows[found].quit);
	outcome = row_outcome(rows, count, found);
	if (!strcmp(playlist, "ranked"))
		mesh_level_apply(*level, *wins, *losses, outcome, level, wins, losses);
	return 1;
}

static int tally_before(const struct mesh_tally_row *left, const struct mesh_tally_row *right)
{
	int name = strcmp(left->name, right->name);

	if (name)
		return name < 0;
	if (left->team != right->team)
		return left->team < right->team;
	if (left->score != right->score)
		return left->score < right->score;
	if (left->place != right->place)
		return left->place < right->place;
	return left->quit < right->quit;
}

int mesh_tally_text(const char *match_id, const char *playlist,
	const unsigned char keys[][32], const char key_names[][12], int key_count,
	const struct mesh_tally_row *rows, int count, char *out, int size)
{
	struct mesh_tally_row ordered[16];
	unsigned char ordered_keys[16][32];
	char ordered_names[16][12];
	int index;
	int used;
	int wrote;

	if (!out || size < 8 || !match_id || !match_id[0] || !playlist || !playlist[0])
		return 0;
	if (key_count < 1 || key_count > 16 || !keys || !key_names)
		return 0;
	if (count < 0 || count > 16 || (count > 0 && !rows))
		return 0;
	for (index = 0; index < key_count; index++)
	{
		int place;

		memcpy(ordered_keys[index], keys[index], 32);
		snprintf(ordered_names[index], sizeof(ordered_names[index]), "%s", key_names[index]);
		if (!ordered_names[index][0] || strchr(ordered_names[index], '|') || strchr(ordered_names[index], '\n'))
			return 0;
		for (place = index; place > 0 && memcmp(ordered_keys[place], ordered_keys[place - 1], 32) < 0; place--)
		{
			unsigned char swap[32];
			char swap_name[12];

			memcpy(swap, ordered_keys[place - 1], 32);
			memcpy(ordered_keys[place - 1], ordered_keys[place], 32);
			memcpy(ordered_keys[place], swap, 32);
			memcpy(swap_name, ordered_names[place - 1], sizeof(swap_name));
			memcpy(ordered_names[place - 1], ordered_names[place], sizeof(swap_name));
			memcpy(ordered_names[place], swap_name, sizeof(swap_name));
		}
	}
	for (index = 0; index < count; index++)
	{
		int place;

		ordered[index] = rows[index];
		ordered[index].name[11] = 0;
		for (place = index; place > 0 && tally_before(&ordered[place], &ordered[place - 1]); place--)
		{
			struct mesh_tally_row swap = ordered[place - 1];

			ordered[place - 1] = ordered[place];
			ordered[place] = swap;
		}
	}
	wrote = snprintf(out, (size_t)size, "v1|%s|%s|%d|%d\n", match_id, playlist, key_count, count);
	if (wrote < 0 || wrote >= size)
		return 0;
	used = wrote;
	for (index = 0; index < key_count; index++)
	{
		char hex[65];

		if (!hex_write(hex, (int)sizeof(hex), ordered_keys[index], 32))
			return 0;
		wrote = snprintf(out + used, (size_t)(size - used), "k|%s|%s\n", hex, ordered_names[index]);
		if (wrote < 0 || wrote >= size - used)
			return 0;
		used += wrote;
	}
	for (index = 0; index < count; index++)
	{
		if (strchr(ordered[index].name, '|') || strchr(ordered[index].name, '\n'))
			return 0;
		wrote = snprintf(out + used, (size_t)(size - used), "%s|%d|%d|%d|%d\n",
			ordered[index].name, ordered[index].team, ordered[index].score, ordered[index].place,
			ordered[index].quit > 0 ? ordered[index].quit : 0);
		if (wrote < 0 || wrote >= size - used)
			return 0;
		used += wrote;
	}
	return 1;
}

int mesh_tally_humans(const char *text)
{
	char match_id[40];
	char playlist[16];
	int humans = 0;
	int count = 0;

	if (!text || sscanf(text, "v1|%39[^|\n]|%15[^|\n]|%d|%d", match_id, playlist, &humans, &count) != 4)
		return 0;
	if (humans < 1 || count < 0 || count > 16)
		return 0;
	return humans;
}

int mesh_tally_majority(int votes, int humans)
{
	return humans > 1 && votes >= 2 && votes * 2 > humans;
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

static const char *tally_key_lines(const char *text)
{
	const char *line;

	if (!mesh_tally_humans(text))
		return NULL;
	line = strchr(text, '\n');
	if (!line)
		return NULL;
	return line + 1;
}

/* bytes in one install line, including its newline. 0 when it is not one. */
static int tally_key_line(const char *line)
{
	int index;
	int length;

	if (!line || line[0] != 'k' || line[1] != '|')
		return 0;
	for (index = 0; index < 64; index++)
	{
		if (hex_nibble(line[2 + index]) < 0)
			return 0;
	}
	if (line[66] != '|')
		return 0;
	length = 67;
	while (line[length] && line[length] != '\n')
	{
		if (line[length] == '|' || length >= 67 + 11)
			return 0;
		length++;
	}
	if (line[length] != '\n')
		return 0;
	return length + 1;
}

int mesh_tally_key_count(const char *text)
{
	const char *line;
	int humans;
	int count = 0;

	humans = mesh_tally_humans(text);
	line = tally_key_lines(text);
	if (!line || humans < 1)
		return 0;
	while (tally_key_line(line))
	{
		count++;
		line += tally_key_line(line);
	}
	if (count != humans)
		return 0;
	return count;
}

int mesh_tally_key_at(const char *text, int index, unsigned char key[32])
{
	const char *line;
	int seen = 0;

	if (!key || index < 0)
		return 0;
	line = tally_key_lines(text);
	if (!line || !mesh_tally_key_count(text))
		return 0;
	while (tally_key_line(line))
	{
		int byte;

		if (seen == index)
		{
			for (byte = 0; byte < 32; byte++)
			{
				int high = hex_nibble(line[2 + byte * 2]);
				int low = hex_nibble(line[3 + byte * 2]);

				if (high < 0 || low < 0)
					return 0;
				key[byte] = (unsigned char)((high << 4) | low);
			}
			return 1;
		}
		seen++;
		line += tally_key_line(line);
	}
	return 0;
}

int mesh_tally_key_name(const char *text, const unsigned char key[32], char name[12])
{
	const char *line;

	if (!name)
		return 0;
	name[0] = 0;
	if (!key)
		return 0;
	line = tally_key_lines(text);
	if (!line || !mesh_tally_key_count(text))
		return 0;
	while (tally_key_line(line))
	{
		unsigned char found[32];
		int byte;
		int span = tally_key_line(line);
		int length = span - 68;

		for (byte = 0; byte < 32; byte++)
		{
			int high = hex_nibble(line[2 + byte * 2]);
			int low = hex_nibble(line[3 + byte * 2]);

			if (high < 0 || low < 0)
				return 0;
			found[byte] = (unsigned char)((high << 4) | low);
		}
		if (!memcmp(found, key, 32))
		{
			if (length < 0 || length > 11)
				return 0;
			memcpy(name, line + 67, (size_t)length);
			name[length] = 0;
			return name[0] ? 1 : 0;
		}
		line += span;
	}
	return 0;
}

int mesh_tally_has_key(const char *text, const unsigned char key[32])
{
	unsigned char found[32];
	int index;
	int count;

	if (!key)
		return 0;
	count = mesh_tally_key_count(text);
	for (index = 0; index < count; index++)
	{
		if (mesh_tally_key_at(text, index, found) && !memcmp(found, key, 32))
			return 1;
	}
	return 0;
}

static int tally_rows(const char *text, char *playlist, int playlist_size, struct mesh_tally_row *rows, int max,
	int *count_out)
{
	const char *line;
	int count = 0;

	if (count_out)
		*count_out = 0;
	if (!mesh_tally_humans(text))
		return 0;
	line = tally_key_lines(text);
	if (!line || !mesh_tally_key_count(text))
		return 0;
	while (tally_key_line(line))
		line += tally_key_line(line);
	while (*line && count < max)
	{
		char name[12];
		int team = 0;
		int score = 0;
		int place = 0;
		int quit = 0;
		int length = 0;

		if (*line == '\n')
			break;
		memset(name, 0, sizeof(name));
		while (line[length] && line[length] != '|' && line[length] != '\n' && length < 11)
		{
			name[length] = line[length];
			length++;
		}
		if (line[length] != '|')
			return 0;
		if (sscanf(line + length + 1, "%d|%d|%d|%d", &team, &score, &place, &quit) != 4)
			return 0;
		snprintf(rows[count].name, sizeof(rows[count].name), "%s", name);
		rows[count].team = team;
		rows[count].score = score;
		rows[count].place = place;
		rows[count].quit = quit > 0 ? quit : 0;
		count++;
		line = strchr(line, '\n');
		if (!line)
			break;
		line++;
	}
	if (playlist && playlist_size > 0)
	{
		char match_id[40];
		int humans = 0;
		int listed = 0;

		playlist[0] = 0;
		if (sscanf(text, "v1|%39[^|\n]|%15[^|\n]|%d|%d", match_id, playlist, &humans, &listed) != 4)
			return 0;
	}
	if (count_out)
		*count_out = count;
	return 1;
}

static int tally_name_count(const char *text, const char *name)
{
	int index;
	int keys;
	int count = 0;

	if (!name || !name[0])
		return 0;
	keys = mesh_tally_key_count(text);
	for (index = 0; index < keys; index++)
	{
		unsigned char key[32];
		char found[12];

		if (!mesh_tally_key_at(text, index, key))
			continue;
		if (mesh_tally_key_name(text, key, found) && !strcmp(found, name))
			count++;
	}
	return count;
}

static int tally_key_row_quit(const char *text, const unsigned char key[32], struct mesh_tally_row *rows, int max,
	int *count_out, int *earliest_out)
{
	char name[12];
	int count = 0;
	int index;
	int found = 0;
	int quit = 0;
	int earliest = 0;

	if (count_out)
		*count_out = 0;
	if (earliest_out)
		*earliest_out = 0;
	if (!mesh_tally_key_name(text, key, name) || tally_name_count(text, name) != 1)
		return 0;
	if (!tally_rows(text, NULL, 0, rows, max, &count))
		return 0;
	for (index = 0; index < count; index++)
	{
		if (rows[index].quit > 0 && (earliest == 0 || rows[index].quit < earliest))
			earliest = rows[index].quit;
		if (strcmp(rows[index].name, name) != 0)
			continue;
		found++;
		quit = rows[index].quit;
	}
	if (count_out)
		*count_out = count;
	if (earliest_out)
		*earliest_out = earliest;
	if (found != 1 || quit < 1)
		return 0;
	return quit;
}

int mesh_tally_key_left(const char *text, const unsigned char key[32])
{
	struct mesh_tally_row rows[16];

	return tally_key_row_quit(text, key, rows, 16, NULL, NULL) > 0;
}

int mesh_tally_key_quit(const char *text, const unsigned char key[32])
{
	struct mesh_tally_row rows[16];
	int earliest = 0;
	int quit;

	quit = tally_key_row_quit(text, key, rows, 16, NULL, &earliest);
	if (quit < 1 || earliest < 1)
		return 0;
	return quit == earliest;
}

int mesh_standings_commit(struct mesh_standing *book, int capacity, const char *tally,
	const struct mesh_claim *claims, int claim_count)
{
	struct mesh_tally_row rows[16];
	struct mesh_claim bound[16];
	char playlist[16];
	char body[4096];
	int count = 0;
	int claim;
	int row;
	int used;
	int wrote;
	int bound_count = 0;

	if (!book || capacity < 1 || !mesh_tally_humans(tally))
		return 0;
	if (claim_count > 16)
		claim_count = 16;
	for (claim = 0; claim < claim_count; claim++)
	{
		char tally_name[12];

		if (!claims || !mesh_tally_key_name(tally, claims[claim].key, tally_name))
			continue;
		if (tally_name_count(tally, tally_name) != 1)
			continue;
		memset(&bound[bound_count], 0, sizeof(bound[bound_count]));
		memcpy(bound[bound_count].key, claims[claim].key, 32);
		snprintf(bound[bound_count].name, sizeof(bound[bound_count].name), "%s", tally_name);
		bound_count++;
	}
	claims = bound;
	claim_count = bound_count;
	if (!tally_rows(tally, playlist, (int)sizeof(playlist), rows, 16, &count))
		return 0;
	wrote = snprintf(body, sizeof(body), "{\"playlist\":\"%s\",\"players\":[", playlist);
	if (wrote < 0 || wrote >= (int)sizeof(body))
		return 0;
	used = wrote;
	for (row = 0; row < count; row++)
	{
		const struct mesh_claim *owner = NULL;
		int owners = 0;
		char account[40];
		char piece[256];

		account[0] = 0;
		for (claim = 0; claim < claim_count; claim++)
		{
			if (!claims[claim].name[0] || strcmp(claims[claim].name, rows[row].name) != 0)
				continue;
			owners++;
			owner = &claims[claim];
		}
		if (owners == 1 && owner)
		{
			char token[40];
			char mesh_id[16];

			mesh_identity(owner->key, account, (int)sizeof(account), token, (int)sizeof(token), mesh_id,
				(int)sizeof(mesh_id));
		}
		if (account[0] && rows[row].team >= 0)
			wrote = snprintf(piece, sizeof(piece),
				"%s{\"account\":\"%s\",\"name\":\"%s\",\"team\":%d,\"place\":%d,\"score\":%d,\"quit\":%s}",
				row ? "," : "", account, rows[row].name, rows[row].team, rows[row].place, rows[row].score,
				rows[row].quit ? "true" : "false");
		else if (rows[row].team >= 0)
			wrote = snprintf(piece, sizeof(piece),
				"%s{\"name\":\"%s\",\"team\":%d,\"place\":%d,\"score\":%d,\"quit\":%s}",
				row ? "," : "", rows[row].name, rows[row].team, rows[row].place, rows[row].score,
				rows[row].quit ? "true" : "false");
		else if (account[0])
			wrote = snprintf(piece, sizeof(piece),
				"%s{\"account\":\"%s\",\"name\":\"%s\",\"place\":%d,\"score\":%d,\"quit\":%s}",
				row ? "," : "", account, rows[row].name, rows[row].place, rows[row].score,
				rows[row].quit ? "true" : "false");
		else
			wrote = snprintf(piece, sizeof(piece),
				"%s{\"name\":\"%s\",\"place\":%d,\"score\":%d,\"quit\":%s}",
				row ? "," : "", rows[row].name, rows[row].place, rows[row].score,
				rows[row].quit ? "true" : "false");
		if (wrote < 0 || wrote >= (int)sizeof(piece) || used + wrote + 2 >= (int)sizeof(body))
			return 0;
		memcpy(body + used, piece, (size_t)wrote);
		used += wrote;
	}
	if (used + 2 >= (int)sizeof(body))
		return 0;
	body[used++] = ']';
	body[used++] = '}';
	body[used] = 0;
	for (claim = 0; claim < claim_count; claim++)
	{
		char account[40];
		char token[40];
		char mesh_id[16];
		int owners = 0;
		int games;
		int level;
		int wins;
		int losses;
		int slot = -1;
		int index;

		if (!claims[claim].name[0])
			continue;
		for (index = 0; index < claim_count; index++)
		{
			if (claims[index].name[0] && !strcmp(claims[index].name, claims[claim].name))
				owners++;
		}
		if (owners != 1)
			continue;
		mesh_identity(claims[claim].key, account, (int)sizeof(account), token, (int)sizeof(token), mesh_id,
			(int)sizeof(mesh_id));
		for (index = 0; index < capacity; index++)
		{
			if (book[index].used && !memcmp(book[index].key, claims[claim].key, 32))
			{
				slot = index;
				break;
			}
			if (!book[index].used && slot < 0)
				slot = index;
		}
		if (slot < 0)
			continue;
		if (!book[slot].used || memcmp(book[slot].key, claims[claim].key, 32) != 0)
		{
			memset(&book[slot], 0, sizeof(book[slot]));
			memcpy(book[slot].key, claims[claim].key, 32);
			book[slot].used = 1;
		}
		games = book[slot].games;
		level = book[slot].level;
		wins = book[slot].wins;
		losses = book[slot].losses;
		if (!mesh_result_apply(body, account, claims[claim].name, &games, &level, &wins, &losses))
			continue;
		book[slot].games = games;
		book[slot].level = level;
		book[slot].wins = wins;
		book[slot].losses = losses;
		snprintf(book[slot].name, sizeof(book[slot].name), "%s", claims[claim].name);
	}
	return 1;
}
