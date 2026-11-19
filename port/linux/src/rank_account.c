/*
RANK_ACCOUNT.C

One rank.txt in each profile's directory. The server assigns the account
id, the token, the Core ID, the games played, and the level. This file
keeps those, and the emblem the player picked.

The server accepts emblem ids 0 through 63. The picker names the first
eight and sends those. A higher id from the server is kept until the
player steps.

Foreground: 0 Sword, 1 Dog tags, 2 Helmet, 3 Bullet, 4 Ring, 5 Wings,
6 Mark, 7 Crest.

Background: 0 Circle, 1 Diamond, 2 Square, 3 Triangle, 4 Star, 5 Plus,
6 Shield, 7 Bar.

Colors are the profile editor's list, indexes 0 through 17.
*/

#include "platform.h"
#include "rank_account.h"

#include <string.h>

enum
{
	RANK_PARTY = 4,
	RANK_CARDS = 16,
	RANK_KICKS = 4,
	RANK_EMBLEM_LAST = 63,
	RANK_SUBSCRIBED = 10,
	RANK_RESULT_TEXT = 2560
};

static const char *const rank_foregrounds[] =
{
	"Sword", "Dog tags", "Helmet", "Bullet", "Ring", "Wings", "Mark", "Crest"
};

static const char *const rank_backgrounds[] =
{
	"Circle", "Diamond", "Square", "Triangle", "Star", "Plus", "Shield", "Bar"
};

static const struct
{
	int games;
	const char *name;
} rank_badges[] =
{
	{ 100, "Recruit" },
	{ 200, "Regular" },
	{ 400, "Veteran" },
	{ 800, "Elite" },
	{ 1600, "Legend" },
	{ 3200, "Mythic" },
	{ 6400, "Paragon" },
	{ 12800, "Ascendant" }
};

struct rank_card
{
	char name[12];
	char account[48];
	int level;
};

static pthread_mutex_t rank_lock_mutex = PTHREAD_MUTEX_INITIALIZER;
/* set by the public-account topic, whose id is not in the payload */
static char rank_forced_account[48];

static struct
{
	char current_path[300];
	struct rank_account current;
	char party_path[RANK_PARTY][300];
	struct rank_account party[RANK_PARTY];
	int party_count;
	int search_ranked;
	char register_path[300];
	char register_name[12];
	char register_refused[12];
	char kick_account[RANK_KICKS][48];
	int kick_count;
	char subscribed[RANK_SUBSCRIBED][96];
	int subscribed_count;
	char profile_path[300];
	unsigned profile_generation;
	unsigned profile_sent;
	int profile_when_registered;
	char match_id[40];
	char match_playlist[16];
	int match_teams;
	struct rank_card cards[RANK_CARDS];
	int card_count;
	int result_pending;
	int result_sent;
	char result_body[RANK_RESULT_TEXT];
} rank_state;

static void rank_lock(void)
{
	pthread_mutex_lock(&rank_lock_mutex);
}

static void rank_unlock(void)
{
	pthread_mutex_unlock(&rank_lock_mutex);
}

static int rank_clamp(int value, int last)
{
	if (value < 0)
		return 0;
	if (value > last)
		return last;
	return value;
}

static void rank_blank(struct rank_account *record)
{
	memset(record, 0, sizeof(*record));
}

static void rank_copy_text(char *out, int size, const char *text)
{
	int index = 0;

	if (!out || size < 1)
		return;
	if (!text)
		text = "";
	while (text[index] && index < size - 1)
	{
		if (text[index] == '\r' || text[index] == '\n')
			break;
		out[index] = text[index];
		index++;
	}
	out[index] = 0;
}

static void rank_file_path(char *out, int size, const char *directory)
{
	int length;
	int wrote;

	if (!out || size < 1)
		return;
	out[0] = 0;
	if (!directory || !directory[0])
		return;
	length = (int)strlen(directory);
	if (directory[length - 1] == '/' || directory[length - 1] == '\\')
		wrote = snprintf(out, (size_t)size, "%srank.txt", directory);
	else
		wrote = snprintf(out, (size_t)size, "%s/rank.txt", directory);
	if (wrote < 0 || wrote >= size)
		out[0] = 0;
}

static void rank_trim(char *text)
{
	int length;

	if (!text)
		return;
	length = (int)strlen(text);
	while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r' || text[length - 1] == ' '))
		text[--length] = 0;
}

/* profile folders arrive as Xbox paths such as z:\saved\... fopen needs the host path. */
static FILE *rank_open_path(const char *path, const char *mode)
{
	char host[1024];
	const char *open_path;

	if (!path || !path[0] || !mode)
		return NULL;
	open_path = path;
	if (path[1] == ':' && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')))
	{
		platform_translate_path(path, host, sizeof(host));
		if (!host[0])
			return NULL;
		open_path = host;
	}
	return fopen(open_path, mode);
}

static void rank_load_path(const char *path, struct rank_account *record)
{
	FILE *file;
	char line[256];

	rank_blank(record);
	if (!path || !path[0])
		return;
	file = rank_open_path(path, "r");
	if (!file)
		return;
	while (fgets(line, (int)sizeof(line), file))
	{
		char *value = strchr(line, '=');

		if (!value)
			continue;
		*value++ = 0;
		rank_trim(value);
		if (!strcmp(line, "account"))
			rank_copy_text(record->account, (int)sizeof(record->account), value);
		else if (!strcmp(line, "token"))
			rank_copy_text(record->token, (int)sizeof(record->token), value);
		else if (!strcmp(line, "core"))
			rank_copy_text(record->core, (int)sizeof(record->core), value);
		else if (!strcmp(line, "name"))
			rank_copy_text(record->name, (int)sizeof(record->name), value);
		else if (!strcmp(line, "foreground"))
			record->foreground = rank_clamp(atoi(value), RANK_EMBLEM_LAST);
		else if (!strcmp(line, "background"))
			record->background = rank_clamp(atoi(value), RANK_EMBLEM_LAST);
		else if (!strcmp(line, "foregroundColor"))
			record->foreground_color = rank_clamp(atoi(value), 17);
		else if (!strcmp(line, "backgroundColor"))
			record->background_color = rank_clamp(atoi(value), 17);
		else if (!strcmp(line, "games"))
			record->games = atoi(value) < 0 ? 0 : atoi(value);
		else if (!strcmp(line, "level"))
			record->level = atoi(value) < 0 ? 0 : atoi(value);
		else if (!strcmp(line, "wins"))
			record->wins = atoi(value) < 0 ? 0 : atoi(value);
		else if (!strcmp(line, "losses"))
			record->losses = atoi(value) < 0 ? 0 : atoi(value);
		else if (!strcmp(line, "chosen"))
			record->chosen = atoi(value) ? 1 : 0;
	}
	fclose(file);
	record->ready = record->token[0] ? 1 : 0;
}

static void rank_save_path(const char *path, const struct rank_account *record)
{
	FILE *file;

	if (!path || !path[0] || !record)
		return;
	file = rank_open_path(path, "w");
	if (!file)
		return;
	fprintf(file, "account=%s\n", record->account);
	fprintf(file, "token=%s\n", record->token);
	fprintf(file, "core=%s\n", record->core);
	fprintf(file, "name=%s\n", record->name);
	fprintf(file, "foreground=%d\n", rank_clamp(record->foreground, RANK_EMBLEM_LAST));
	fprintf(file, "background=%d\n", rank_clamp(record->background, RANK_EMBLEM_LAST));
	fprintf(file, "foregroundColor=%d\n", rank_clamp(record->foreground_color, 17));
	fprintf(file, "backgroundColor=%d\n", rank_clamp(record->background_color, 17));
	fprintf(file, "games=%d\n", record->games < 0 ? 0 : record->games);
	fprintf(file, "level=%d\n", record->level < 0 ? 0 : record->level);
	fprintf(file, "wins=%d\n", record->wins < 0 ? 0 : record->wins);
	fprintf(file, "losses=%d\n", record->losses < 0 ? 0 : record->losses);
	fprintf(file, "chosen=%d\n", record->chosen ? 1 : 0);
	fclose(file);
}

static void rank_topic_piece(char *out, int size, const char *text)
{
	int index = 0;

	if (!out || size < 1)
		return;
	if (!text)
		text = "";
	while (*text && index < size - 1)
	{
		char character = *text++;

		if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9') || character == '-' || character == '_')
			out[index++] = character;
	}
	out[index] = 0;
}

static const char *rank_json_at(const char *json, const char *key)
{
	char pattern[64];
	const char *found;
	int wrote;

	if (!json || !key)
		return NULL;
	wrote = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	if (wrote < 0 || wrote >= (int)sizeof(pattern))
		return NULL;
	found = strstr(json, pattern);
	if (!found)
		return NULL;
	found += strlen(pattern);
	while (*found == ' ' || *found == '\t' || *found == '\n' || *found == '\r')
		found++;
	if (*found != ':')
		return NULL;
	found++;
	while (*found == ' ' || *found == '\t' || *found == '\n' || *found == '\r')
		found++;
	return found;
}

static int rank_json_string(const char *json, const char *key, char *out, int size)
{
	const char *value = rank_json_at(json, key);
	int index = 0;

	if (out && size > 0)
		out[0] = 0;
	if (!value || *value != '"' || !out || size < 2)
		return 0;
	value++;
	while (*value && *value != '"' && index < size - 1)
	{
		char character = *value++;

		if (character == '\\' && *value)
			character = *value++;
		out[index++] = character;
	}
	out[index] = 0;
	return 1;
}

static int rank_json_number(const char *json, const char *key, long *number)
{
	const char *value = rank_json_at(json, key);
	char *end;
	long parsed;

	if (!value || !number)
		return 0;
	parsed = strtol(value, &end, 10);
	if (end == value)
		return 0;
	*number = parsed;
	return 1;
}

static int rank_party_ready_locked(void)
{
	int index;

	if (rank_state.party_count < 1)
		return 0;
	for (index = 0; index < rank_state.party_count; index++)
	{
		if (!rank_state.party[index].token[0])
			return 0;
	}
	return 1;
}

static void rank_queue_profile_locked(const char *path)
{
	if (!path || !path[0])
		return;
	rank_copy_text(rank_state.profile_path, (int)sizeof(rank_state.profile_path), path);
	rank_state.profile_generation++;
}

void rank_json_escape(char *out, int size, const char *text)
{
	int used = 0;

	if (!out || size < 1)
		return;
	if (!text)
		text = "";
	while (*text && used < size - 1)
	{
		char character = *text++;

		if ((character == '"' || character == '\\') && used + 1 < size - 1)
			out[used++] = '\\';
		if (character < 32)
			character = '?';
		out[used++] = character;
	}
	out[used] = 0;
}

int rank_foreground_count(void)
{
	return (int)(sizeof(rank_foregrounds) / sizeof(rank_foregrounds[0]));
}

int rank_background_count(void)
{
	return (int)(sizeof(rank_backgrounds) / sizeof(rank_backgrounds[0]));
}

int rank_color_count(void)
{
	return 18;
}

const char *rank_foreground_name(int index)
{
	if (index < 0 || index >= rank_foreground_count())
		return "Emblem";
	return rank_foregrounds[index];
}

const char *rank_background_name(int index)
{
	if (index < 0 || index >= rank_background_count())
		return "Shape";
	return rank_backgrounds[index];
}

int rank_badge_index(int games)
{
	int index;

	if (games < 0)
		games = 0;
	for (index = 0; index < (int)(sizeof(rank_badges) / sizeof(rank_badges[0])); index++)
	{
		if (games <= rank_badges[index].games)
			return index;
	}
	return (int)(sizeof(rank_badges) / sizeof(rank_badges[0])) - 1;
}

const char *rank_badge_name(int games)
{
	return rank_badges[rank_badge_index(games)].name;
}

unsigned rank_level_rgba(int level)
{
	if (level < 10)
		return 0x5ED38Cu;
	if (level < 20)
		return 0xE8C547u;
	if (level < 30)
		return 0x2896FFu;
	if (level < 40)
		return 0xE86AB8u;
	if (level < 50)
		return 0xA078E0u;
	return 0xE86A5Au;
}

void rank_account_bind(const char *directory, const char *name)
{
	char path[300];
	char safe[12];

	rank_file_path(path, (int)sizeof(path), directory);
	rank_copy_text(safe, (int)sizeof(safe), name);
	if (!path[0])
		return;
	rank_lock();
	if (!strcmp(path, rank_state.current_path))
	{
		if (safe[0] && strcmp(rank_state.current.name, safe))
		{
			rank_copy_text(rank_state.current.name, (int)sizeof(rank_state.current.name), safe);
			if (rank_state.current.token[0])
				rank_queue_profile_locked(rank_state.current_path);
		}
		rank_unlock();
		return;
	}
	rank_copy_text(rank_state.current_path, (int)sizeof(rank_state.current_path), path);
	rank_load_path(rank_state.current_path, &rank_state.current);
	if (safe[0])
		rank_copy_text(rank_state.current.name, (int)sizeof(rank_state.current.name), safe);
	rank_state.current.ready = rank_state.current.token[0] ? 1 : 0;
	rank_unlock();
}

int rank_account_bound(void)
{
	int bound;

	rank_lock();
	bound = rank_state.current_path[0] ? 1 : 0;
	rank_unlock();
	return bound;
}

void rank_account_get(struct rank_account *out)
{
	if (!out)
		return;
	rank_lock();
	*out = rank_state.current;
	out->ready = rank_state.current.token[0] ? 1 : 0;
	rank_unlock();
}

static int rank_mesh_id_ok(const char *mesh_id)
{
	int index;

	if (!mesh_id || mesh_id[0] != 'M')
		return 0;
	for (index = 1; index < 9; index++)
	{
		char character = mesh_id[index];

		if (!((character >= '0' && character <= '9') || (character >= 'A' && character <= 'F')))
			return 0;
	}
	return mesh_id[9] == 0;
}

void rank_account_remember_mesh(const char *mesh_id)
{
	int index;

	if (!rank_mesh_id_ok(mesh_id))
		return;
	rank_lock();
	if (!rank_state.current_path[0] || !strcmp(rank_state.current.core, mesh_id))
	{
		rank_unlock();
		return;
	}
	rank_copy_text(rank_state.current.core, (int)sizeof(rank_state.current.core), mesh_id);
	rank_save_path(rank_state.current_path, &rank_state.current);
	for (index = 0; index < rank_state.party_count; index++)
	{
		if (!strcmp(rank_state.party_path[index], rank_state.current_path))
			rank_copy_text(rank_state.party[index].core, (int)sizeof(rank_state.party[index].core), mesh_id);
	}
	rank_unlock();
}

void rank_emblem_step(int field, int delta)
{
	int *value;
	int count;

	rank_lock();
	if (field == 0)
	{
		value = &rank_state.current.foreground;
		count = rank_foreground_count();
	}
	else if (field == 1)
	{
		value = &rank_state.current.background;
		count = rank_background_count();
	}
	else if (field == 2)
	{
		value = &rank_state.current.foreground_color;
		count = rank_color_count();
	}
	else if (field == 3)
	{
		value = &rank_state.current.background_color;
		count = rank_color_count();
	}
	else
	{
		rank_unlock();
		return;
	}
	if (count < 1)
	{
		rank_unlock();
		return;
	}
	if (*value < 0 || *value >= count)
		*value = delta < 0 ? count - 1 : 0;
	else
	{
		*value += delta;
		while (*value < 0)
			*value += count;
		while (*value >= count)
			*value -= count;
	}
	rank_state.current.chosen = 1;
	rank_unlock();
}

void rank_emblem_set(int field, int value)
{
	int *slot;
	int count;

	rank_lock();
	if (field == 0)
	{
		slot = &rank_state.current.foreground;
		count = rank_foreground_count();
	}
	else if (field == 1)
	{
		slot = &rank_state.current.background;
		count = rank_background_count();
	}
	else if (field == 2)
	{
		slot = &rank_state.current.foreground_color;
		count = rank_color_count();
	}
	else if (field == 3)
	{
		slot = &rank_state.current.background_color;
		count = rank_color_count();
	}
	else
	{
		rank_unlock();
		return;
	}
	if (count < 1)
	{
		rank_unlock();
		return;
	}
	if (value < 0)
		value = 0;
	if (value >= count)
		value = count - 1;
	if (*slot != value)
	{
		*slot = value;
		rank_state.current.chosen = 1;
	}
	rank_unlock();
}

void rank_account_commit(void)
{
	rank_lock();
	if (rank_state.current_path[0])
	{
		rank_state.current.chosen = 1;
		rank_state.current.foreground = rank_clamp(rank_state.current.foreground, RANK_EMBLEM_LAST);
		rank_state.current.background = rank_clamp(rank_state.current.background, RANK_EMBLEM_LAST);
		rank_state.current.foreground_color = rank_clamp(rank_state.current.foreground_color, 17);
		rank_state.current.background_color = rank_clamp(rank_state.current.background_color, 17);
		rank_save_path(rank_state.current_path, &rank_state.current);
		if (rank_state.current.token[0])
			rank_queue_profile_locked(rank_state.current_path);
		else
			rank_state.profile_when_registered = 1;
	}
	rank_unlock();
}

void rank_account_restore(const struct rank_account *saved)
{
	rank_lock();
	if (saved)
	{
		rank_state.current.foreground = rank_clamp(saved->foreground, 7);
		rank_state.current.background = rank_clamp(saved->background, 7);
		rank_state.current.foreground_color = rank_clamp(saved->foreground_color, 17);
		rank_state.current.background_color = rank_clamp(saved->background_color, 17);
		rank_state.current.chosen = saved->chosen ? 1 : 0;
	}
	if (rank_state.current_path[0])
		rank_save_path(rank_state.current_path, &rank_state.current);
	rank_unlock();
}

void rank_party_begin(void)
{
	rank_lock();
	rank_state.party_count = 0;
	rank_unlock();
}

void rank_party_add(const char *directory, const char *name)
{
	char path[300];
	struct rank_account loaded;
	char safe[12];
	int slot;

	rank_copy_text(safe, (int)sizeof(safe), name);
	if (!safe[0])
		return;
	rank_file_path(path, (int)sizeof(path), directory);
	rank_load_path(path, &loaded);
	rank_copy_text(loaded.name, (int)sizeof(loaded.name), safe);
	rank_lock();
	if (rank_state.party_count >= RANK_PARTY)
	{
		rank_unlock();
		return;
	}
	slot = rank_state.party_count++;
	rank_copy_text(rank_state.party_path[slot], (int)sizeof(rank_state.party_path[slot]), path);
	rank_state.party[slot] = loaded;
	rank_unlock();
}

int rank_party_ready(void)
{
	int ready;

	rank_lock();
	ready = rank_party_ready_locked();
	rank_unlock();
	return ready;
}

void rank_search_set(int ranked)
{
	rank_lock();
	rank_state.search_ranked = ranked ? 1 : 0;
	rank_unlock();
}

int rank_search_blocks(void)
{
	int block;

	rank_lock();
	block = rank_state.search_ranked && !rank_party_ready_locked();
	rank_unlock();
	return block;
}

void rank_search_clear(void)
{
	rank_lock();
	rank_state.search_ranked = 0;
	rank_state.party_count = 0;
	rank_unlock();
}

static int rank_append(char *text, int size, int used, const char *piece)
{
	int length;

	if (!text || used < 0 || !piece)
		return -1;
	length = (int)strlen(piece);
	if (used + length >= size)
		return -1;
	memcpy(text + used, piece, (size_t)length + 1);
	return used + length;
}

int rank_queue_extra(char *text, int size)
{
	struct rank_account members[RANK_PARTY];
	char kicks[RANK_KICKS][48];
	int count;
	int kick_count;
	int index;
	int first;
	int accounted;
	int used;
	char escaped_account[96];
	char escaped_token[160];
	char escaped_name[32];
	char piece[320];

	if (!text || size < 2)
		return 0;
	text[0] = 0;
	rank_lock();
	count = rank_state.party_count;
	if (count < 1 && rank_state.current.token[0] && rank_state.current.account[0])
	{
		members[0] = rank_state.current;
		count = 1;
	}
	else
	{
		if (count > RANK_PARTY)
			count = RANK_PARTY;
		for (index = 0; index < count; index++)
			members[index] = rank_state.party[index];
	}
	kick_count = rank_state.kick_count;
	if (kick_count > RANK_KICKS)
		kick_count = RANK_KICKS;
	for (index = 0; index < kick_count; index++)
		rank_copy_text(kicks[index], (int)sizeof(kicks[index]), rank_state.kick_account[index]);
	rank_unlock();
	first = -1;
	accounted = 0;
	for (index = 0; index < count; index++)
	{
		if (!members[index].token[0] || !members[index].account[0])
			continue;
		if (first < 0)
			first = index;
		accounted++;
	}
	if (first < 0 && kick_count < 1)
		return 0;
	used = 0;
	if (first >= 0)
	{
		rank_json_escape(escaped_account, (int)sizeof(escaped_account), members[first].account);
		rank_json_escape(escaped_token, (int)sizeof(escaped_token), members[first].token);
		snprintf(piece, sizeof(piece), ",\"account\":\"%s\",\"token\":\"%s\"",
			escaped_account, escaped_token);
		used = rank_append(text, size, used, piece);
	}
	/* one local profile uses the top-level fields. more than one lists members */
	if (accounted > 1)
	{
		used = rank_append(text, size, used, ",\"members\":[");
		for (index = 0; index < count; index++)
		{
			if (!members[index].token[0] || !members[index].account[0])
				continue;
			rank_json_escape(escaped_account, (int)sizeof(escaped_account), members[index].account);
			rank_json_escape(escaped_token, (int)sizeof(escaped_token), members[index].token);
			rank_json_escape(escaped_name, (int)sizeof(escaped_name), members[index].name);
			snprintf(piece, sizeof(piece), "%s{\"account\":\"%s\",\"token\":\"%s\",\"name\":\"%s\"}",
				text[0] && text[strlen(text) - 1] != '[' ? "," : "",
				escaped_account, escaped_token, escaped_name);
			used = rank_append(text, size, used, piece);
		}
		used = rank_append(text, size, used, "]");
	}
	if (kick_count > 0)
	{
		used = rank_append(text, size, used, ",\"kick\":[");
		for (index = 0; index < kick_count; index++)
		{
			rank_json_escape(escaped_account, (int)sizeof(escaped_account), kicks[index]);
			snprintf(piece, sizeof(piece), "%s\"%s\"", index ? "," : "", escaped_account);
			used = rank_append(text, size, used, piece);
		}
		used = rank_append(text, size, used, "]");
	}
	if (used < 0)
	{
		text[0] = 0;
		return 0;
	}
	return text[0] ? 1 : 0;
}

void rank_register_kick(void)
{
	int index;

	rank_lock();
	if (rank_state.register_path[0])
	{
		rank_unlock();
		return;
	}
	for (index = 0; index < rank_state.party_count; index++)
	{
		if (rank_state.party[index].token[0] || !rank_state.party[index].name[0] || !rank_state.party_path[index][0])
			continue;
		if (rank_state.register_refused[0] && !strcmp(rank_state.register_refused, rank_state.party[index].name))
			continue;
		rank_copy_text(rank_state.register_path, (int)sizeof(rank_state.register_path), rank_state.party_path[index]);
		rank_copy_text(rank_state.register_name, (int)sizeof(rank_state.register_name), rank_state.party[index].name);
		rank_unlock();
		return;
	}
	if (!rank_state.current.token[0] && rank_state.current.name[0] && rank_state.current_path[0] &&
		!(rank_state.register_refused[0] && !strcmp(rank_state.register_refused, rank_state.current.name)))
	{
		rank_copy_text(rank_state.register_path, (int)sizeof(rank_state.register_path), rank_state.current_path);
		rank_copy_text(rank_state.register_name, (int)sizeof(rank_state.register_name), rank_state.current.name);
	}
	rank_unlock();
}

int rank_register_pending(char *body, int size)
{
	char escaped[32];
	char name[12];
	int pending;

	rank_lock();
	pending = rank_state.register_path[0] && rank_state.register_name[0];
	rank_copy_text(name, (int)sizeof(name), rank_state.register_name);
	rank_unlock();
	if (!pending || !body || size < 16)
		return 0;
	rank_json_escape(escaped, (int)sizeof(escaped), name);
	snprintf(body, (size_t)size, "{\"gamertag\":\"%s\"}", escaped);
	return body[0] ? 1 : 0;
}

int rank_profile_pending(char *topic, int topic_size, char *body, int body_size, unsigned *generation)
{
	struct rank_account record;
	char path[300];
	char account[48];
	char escaped_token[160];
	char escaped_name[32];
	unsigned stamp;
	int wrote;

	if (!topic || !body || !generation || topic_size < 16 || body_size < 32)
		return 0;
	rank_lock();
	if (!rank_state.profile_path[0] || rank_state.profile_generation == rank_state.profile_sent)
	{
		rank_unlock();
		return 0;
	}
	stamp = rank_state.profile_generation;
	rank_copy_text(path, (int)sizeof(path), rank_state.profile_path);
	if (!strcmp(path, rank_state.current_path))
		record = rank_state.current;
	else
		rank_load_path(path, &record);
	rank_unlock();
	if (!record.token[0] || !record.account[0])
		return 0;
	rank_topic_piece(account, (int)sizeof(account), record.account);
	if (!account[0])
		return 0;
	rank_json_escape(escaped_token, (int)sizeof(escaped_token), record.token);
	rank_json_escape(escaped_name, (int)sizeof(escaped_name), record.name);
	wrote = snprintf(topic, (size_t)topic_size, "hceu/3/mm/v1/profile/%s", account);
	if (wrote < 0 || wrote >= topic_size)
		return 0;
	wrote = snprintf(body, (size_t)body_size,
		"{\"token\":\"%s\",\"gamertag\":\"%s\",\"foreground\":%d,\"background\":%d,\"foregroundColor\":%d,\"backgroundColor\":%d}",
		escaped_token, escaped_name, rank_clamp(record.foreground, RANK_EMBLEM_LAST),
		rank_clamp(record.background, RANK_EMBLEM_LAST),
		rank_clamp(record.foreground_color, 17), rank_clamp(record.background_color, 17));
	if (wrote < 0 || wrote >= body_size)
		return 0;
	*generation = stamp;
	return 1;
}

void rank_profile_sent(unsigned generation)
{
	rank_lock();
	if (generation >= rank_state.profile_sent)
		rank_state.profile_sent = generation;
	rank_unlock();
}

static void rank_merge_server(struct rank_account *record, const struct rank_account *incoming, int got_account,
	int got_token, int got_core, int got_games, int got_level, int got_foreground, int got_background,
	int got_foreground_color, int got_background_color)
{
	if (got_account)
		rank_copy_text(record->account, (int)sizeof(record->account), incoming->account);
	if (got_token)
		rank_copy_text(record->token, (int)sizeof(record->token), incoming->token);
	if (got_core)
		rank_copy_text(record->core, (int)sizeof(record->core), incoming->core);
	if (got_games && incoming->games >= 0)
		record->games = incoming->games;
	if (got_level && incoming->level >= 0)
		record->level = incoming->level;
	if (!record->chosen)
	{
		if (got_foreground)
			record->foreground = rank_clamp(incoming->foreground, RANK_EMBLEM_LAST);
		if (got_background)
			record->background = rank_clamp(incoming->background, RANK_EMBLEM_LAST);
		if (got_foreground_color)
			record->foreground_color = rank_clamp(incoming->foreground_color, 17);
		if (got_background_color)
			record->background_color = rank_clamp(incoming->background_color, 17);
	}
	record->ready = record->token[0] ? 1 : 0;
}

void rank_apply_account_message(const char *payload)
{
	struct rank_account incoming;
	struct rank_account stored;
	char path[300];
	long number;
	int got_account;
	int got_token;
	int got_core;
	int got_games;
	int got_level;
	int got_foreground;
	int got_background;
	int got_foreground_color;
	int got_background_color;
	int index;
	int keep_emblem;
	int foreground;
	int background;
	int foreground_color;
	int background_color;
	int chosen;

	if (!payload)
		return;
	rank_blank(&incoming);
	got_account = rank_json_string(payload, "account", incoming.account, (int)sizeof(incoming.account));
	if (!got_account && rank_forced_account[0])
	{
		rank_copy_text(incoming.account, (int)sizeof(incoming.account), rank_forced_account);
		got_account = 1;
	}
	got_token = rank_json_string(payload, "token", incoming.token, (int)sizeof(incoming.token));
	got_core = rank_json_string(payload, "core", incoming.core, (int)sizeof(incoming.core));
	if (!got_core && rank_json_number(payload, "core", &number) && number >= 0)
	{
		snprintf(incoming.core, sizeof(incoming.core), "%ld", number);
		got_core = incoming.core[0] ? 1 : 0;
	}
	got_games = rank_json_number(payload, "games", &number);
	incoming.games = got_games && number >= 0 ? (int)number : -1;
	got_level = rank_json_number(payload, "level", &number);
	incoming.level = got_level && number >= 0 ? (int)number : -1;
	got_foreground = rank_json_number(payload, "foreground", &number);
	incoming.foreground = got_foreground ? (int)number : 0;
	got_background = rank_json_number(payload, "background", &number);
	incoming.background = got_background ? (int)number : 0;
	got_foreground_color = rank_json_number(payload, "foregroundColor", &number);
	incoming.foreground_color = got_foreground_color ? (int)number : 0;
	got_background_color = rank_json_number(payload, "backgroundColor", &number);
	incoming.background_color = got_background_color ? (int)number : 0;
	if (!got_account && !got_token && !got_core && !got_games && !got_level)
		return;
	rank_lock();
	path[0] = 0;
	if (got_token && rank_state.register_path[0])
		rank_copy_text(path, (int)sizeof(path), rank_state.register_path);
	else if (got_account && rank_state.current.account[0] && !strcmp(rank_state.current.account, incoming.account))
		rank_copy_text(path, (int)sizeof(path), rank_state.current_path);
	else if (got_account)
	{
		for (index = 0; index < rank_state.party_count; index++)
		{
			if (rank_state.party[index].account[0] && !strcmp(rank_state.party[index].account, incoming.account))
			{
				rank_copy_text(path, (int)sizeof(path), rank_state.party_path[index]);
				break;
			}
		}
	}
	else if (rank_state.current_path[0])
		rank_copy_text(path, (int)sizeof(path), rank_state.current_path);
	if (!path[0])
	{
		rank_unlock();
		return;
	}
	if (!strcmp(path, rank_state.current_path))
		stored = rank_state.current;
	else
		rank_load_path(path, &stored);
	if (!stored.name[0] && rank_state.register_name[0] && !strcmp(path, rank_state.register_path))
		rank_copy_text(stored.name, (int)sizeof(stored.name), rank_state.register_name);
	{
		char gamertag[12];

		if ((!stored.name[0]) && rank_json_string(payload, "gamertag", gamertag, (int)sizeof(gamertag)) &&
			gamertag[0])
			rank_copy_text(stored.name, (int)sizeof(stored.name), gamertag);
	}
	rank_merge_server(&stored, &incoming, got_account, got_token, got_core, got_games, got_level, got_foreground,
		got_background, got_foreground_color, got_background_color);
	rank_save_path(path, &stored);
	if (!strcmp(path, rank_state.current_path))
	{
		keep_emblem = rank_state.current.chosen;
		foreground = rank_state.current.foreground;
		background = rank_state.current.background;
		foreground_color = rank_state.current.foreground_color;
		background_color = rank_state.current.background_color;
		chosen = rank_state.current.chosen;
		rank_state.current = stored;
		if (keep_emblem)
		{
			rank_state.current.foreground = foreground;
			rank_state.current.background = background;
			rank_state.current.foreground_color = foreground_color;
			rank_state.current.background_color = background_color;
			rank_state.current.chosen = chosen;
		}
		rank_state.current.ready = rank_state.current.token[0] ? 1 : 0;
	}
	for (index = 0; index < rank_state.party_count; index++)
	{
		char kept[12];

		if (strcmp(rank_state.party_path[index], path))
			continue;
		rank_copy_text(kept, (int)sizeof(kept), rank_state.party[index].name);
		rank_state.party[index] = stored;
		if (kept[0])
			rank_copy_text(rank_state.party[index].name, (int)sizeof(rank_state.party[index].name), kept);
	}
	if (got_token && rank_state.register_path[0] && !strcmp(rank_state.register_path, path))
	{
		if (rank_state.register_refused[0] && !strcmp(rank_state.register_refused, rank_state.register_name))
			rank_state.register_refused[0] = 0;
		rank_state.register_path[0] = 0;
		rank_state.register_name[0] = 0;
		if (stored.chosen || rank_state.profile_when_registered)
		{
			rank_state.profile_when_registered = 0;
			rank_queue_profile_locked(path);
		}
	}
	rank_unlock();
}

static int rank_json_false(const char *json, const char *key)
{
	const char *value = rank_json_at(json, key);

	return value && !strncmp(value, "false", 5) ? 1 : 0;
}

static int rank_subscribed_has(const char *topic)
{
	int index;

	for (index = 0; index < rank_state.subscribed_count; index++)
	{
		if (!strcmp(rank_state.subscribed[index], topic))
			return 1;
	}
	return 0;
}

static void rank_collect_account(char accounts[][48], int *count, const char *account)
{
	char piece[48];
	int index;

	rank_topic_piece(piece, (int)sizeof(piece), account);
	if (!piece[0] || *count >= 5)
		return;
	for (index = 0; index < *count; index++)
	{
		if (!strcmp(accounts[index], piece))
			return;
	}
	rank_copy_text(accounts[*count], 48, piece);
	(*count)++;
}

void rank_apply_downlink(const char *topic, const char *payload, const char *client_id)
{
	char wanted[96];
	char error[16];
	char account[48];
	int wrote;

	if (!topic || !payload)
		return;
	if (client_id && client_id[0])
	{
		wrote = snprintf(wanted, sizeof(wanted), "hceu/3/mm/v1/register/%s/reply", client_id);
		if (wrote > 0 && wrote < (int)sizeof(wanted) && !strcmp(topic, wanted))
		{
			if (rank_json_false(payload, "ok"))
			{
				error[0] = 0;
				rank_json_string(payload, "error", error, (int)sizeof(error));
				rank_lock();
				if (!strcmp(error, "gamertag") && rank_state.register_name[0])
					rank_copy_text(rank_state.register_refused, (int)sizeof(rank_state.register_refused),
						rank_state.register_name);
				rank_state.register_path[0] = 0;
				rank_state.register_name[0] = 0;
				rank_unlock();
				return;
			}
			rank_apply_account_message(payload);
			return;
		}
		wrote = snprintf(wanted, sizeof(wanted), "hceu/3/mm/v1/result/%s/reply", client_id);
		if (wrote > 0 && wrote < (int)sizeof(wanted) && !strcmp(topic, wanted))
		{
			rank_result_sent();
			return;
		}
	}
	if (!strncmp(topic, "hceu/3/mm/v1/account/", sizeof("hceu/3/mm/v1/account/") - 1))
	{
		rank_topic_piece(account, (int)sizeof(account), topic + (sizeof("hceu/3/mm/v1/account/") - 1));
		if (!account[0])
			return;
		rank_copy_text(rank_forced_account, (int)sizeof(rank_forced_account), account);
		rank_apply_account_message(payload);
		rank_forced_account[0] = 0;
	}
}

int rank_subscribe_next(char *topic, int size)
{
	char accounts[5][48];
	char built[96];
	int count;
	int index;
	int kind;
	int wrote;

	if (!topic || size < 16)
		return 0;
	topic[0] = 0;
	count = 0;
	rank_lock();
	rank_collect_account(accounts, &count, rank_state.current.account);
	for (index = 0; index < rank_state.party_count; index++)
		rank_collect_account(accounts, &count, rank_state.party[index].account);
	for (index = 0; index < count; index++)
	{
		for (kind = 0; kind < 2; kind++)
		{
			if (kind == 0)
				wrote = snprintf(built, sizeof(built), "hceu/3/mm/v1/account/%s", accounts[index]);
			else
				wrote = snprintf(built, sizeof(built), "hceu/3/mm/v1/profile/%s/reply", accounts[index]);
			if (wrote < 1 || wrote >= (int)sizeof(built) || rank_subscribed_has(built))
				continue;
			rank_copy_text(topic, size, built);
			rank_unlock();
			return topic[0] ? 1 : 0;
		}
	}
	rank_unlock();
	return 0;
}

void rank_subscribe_done(const char *topic)
{
	if (!topic || !topic[0])
		return;
	rank_lock();
	if (!rank_subscribed_has(topic) && rank_state.subscribed_count < RANK_SUBSCRIBED)
	{
		rank_copy_text(rank_state.subscribed[rank_state.subscribed_count],
			(int)sizeof(rank_state.subscribed[0]), topic);
		rank_state.subscribed_count++;
	}
	rank_unlock();
}

void rank_kick_add(const char *account)
{
	char safe[48];
	int index;

	rank_topic_piece(safe, (int)sizeof(safe), account);
	if (!safe[0])
		return;
	rank_lock();
	for (index = 0; index < rank_state.kick_count; index++)
	{
		if (!strcmp(rank_state.kick_account[index], safe))
		{
			rank_unlock();
			return;
		}
	}
	if (rank_state.kick_count < RANK_KICKS)
	{
		rank_copy_text(rank_state.kick_account[rank_state.kick_count],
			(int)sizeof(rank_state.kick_account[0]), safe);
		rank_state.kick_count++;
	}
	rank_unlock();
}

void rank_kick_published(void)
{
	rank_lock();
	rank_state.kick_count = 0;
	rank_unlock();
}

int rank_match_begin(const char *match, const char *playlist, int teams)
{
	char safe[40];
	int same;

	rank_topic_piece(safe, (int)sizeof(safe), match);
	if (!safe[0])
	{
		static unsigned rank_match_serial;
		char piece[24];

		rank_match_serial++;
		rank_lock();
		rank_topic_piece(piece, (int)sizeof(piece), rank_state.current.account);
		rank_unlock();
		if (!piece[0])
			snprintf(safe, sizeof(safe), "m%u", rank_match_serial);
		else
			snprintf(safe, sizeof(safe), "%s-%u", piece, rank_match_serial);
	}
	rank_lock();
	same = rank_state.match_id[0] && safe[0] && !strcmp(rank_state.match_id, safe);
	if (same)
	{
		rank_unlock();
		return 0;
	}
	rank_copy_text(rank_state.match_id, (int)sizeof(rank_state.match_id), safe);
	rank_copy_text(rank_state.match_playlist, (int)sizeof(rank_state.match_playlist), playlist ? playlist : "");
	rank_state.match_teams = teams ? 1 : 0;
	rank_state.card_count = 0;
	rank_state.result_pending = 0;
	rank_state.result_sent = 0;
	rank_state.result_body[0] = 0;
	rank_unlock();
	return 1;
}

void rank_match_close(void)
{
	rank_lock();
	if (!rank_state.result_sent)
	{
		rank_state.match_id[0] = 0;
		rank_state.result_pending = 0;
		rank_state.result_body[0] = 0;
		rank_state.result_sent = 1;
	}
	rank_unlock();
}

void rank_match_note(const char *name, const char *account, int level)
{
	int slot;

	if (!name || !name[0])
		return;
	rank_lock();
	if (rank_state.card_count >= RANK_CARDS)
	{
		rank_unlock();
		return;
	}
	slot = rank_state.card_count++;
	rank_copy_text(rank_state.cards[slot].name, (int)sizeof(rank_state.cards[slot].name), name);
	rank_copy_text(rank_state.cards[slot].account, (int)sizeof(rank_state.cards[slot].account), account ? account : "");
	rank_state.cards[slot].level = level < 0 ? 0 : level;
	rank_unlock();
}

int rank_level_named(const char *name)
{
	int index;
	int level = -1;

	if (!name || !name[0])
		return -1;
	rank_lock();
	for (index = 0; index < rank_state.card_count; index++)
	{
		if (!strcmp(rank_state.cards[index].name, name))
		{
			level = rank_state.cards[index].level;
			break;
		}
	}
	if (level < 0)
	{
		for (index = 0; index < rank_state.party_count; index++)
		{
			if (!strcmp(rank_state.party[index].name, name))
			{
				level = rank_state.party[index].level;
				break;
			}
		}
	}
	if (level < 0 && rank_state.current.name[0] && !strcmp(rank_state.current.name, name))
		level = rank_state.current.level;
	rank_unlock();
	return level;
}

void rank_account_for_name(const char *name, char *account, int size)
{
	int index;

	if (!account || size < 2)
		return;
	account[0] = 0;
	if (!name || !name[0])
		return;
	rank_lock();
	for (index = 0; index < rank_state.card_count; index++)
	{
		if (!strcmp(rank_state.cards[index].name, name) && rank_state.cards[index].account[0])
		{
			rank_copy_text(account, size, rank_state.cards[index].account);
			rank_unlock();
			return;
		}
	}
	for (index = 0; index < rank_state.party_count; index++)
	{
		if (!strcmp(rank_state.party[index].name, name) && rank_state.party[index].account[0])
		{
			rank_copy_text(account, size, rank_state.party[index].account);
			rank_unlock();
			return;
		}
	}
	if (rank_state.current.name[0] && !strcmp(rank_state.current.name, name))
		rank_copy_text(account, size, rank_state.current.account);
	rank_unlock();
}

int rank_match_card_count(void)
{
	int count;

	rank_lock();
	count = rank_state.card_count;
	rank_unlock();
	return count;
}

int rank_match_card_at(int index, char *name, int name_size, char *account, int account_size)
{
	int ok = 0;

	if (name && name_size > 0)
		name[0] = 0;
	if (account && account_size > 0)
		account[0] = 0;
	rank_lock();
	if (index >= 0 && index < rank_state.card_count)
	{
		if (name && name_size > 0)
			rank_copy_text(name, name_size, rank_state.cards[index].name);
		if (account && account_size > 0)
			rank_copy_text(account, account_size, rank_state.cards[index].account);
		ok = 1;
	}
	rank_unlock();
	return ok;
}

int rank_result_open(void)
{
	int open;

	rank_lock();
	open = rank_state.match_id[0] && !rank_state.result_sent && !rank_state.result_pending;
	rank_unlock();
	return open;
}

int rank_result_context(char *match, int match_size, char *playlist, int playlist_size, char *token, int token_size,
	int *teams)
{
	int ok = 0;

	if (!match || !playlist || !token || !teams || match_size < 2 || playlist_size < 2 || token_size < 2)
		return 0;
	match[0] = 0;
	playlist[0] = 0;
	token[0] = 0;
	rank_lock();
	if (rank_state.match_id[0])
	{
		rank_copy_text(match, match_size, rank_state.match_id);
		rank_copy_text(playlist, playlist_size, rank_state.match_playlist);
		*teams = rank_state.match_teams;
		if (rank_state.current.token[0])
			rank_copy_text(token, token_size, rank_state.current.token);
		else if (rank_state.party_count > 0 && rank_state.party[0].token[0])
			rank_copy_text(token, token_size, rank_state.party[0].token);
		ok = 1;
	}
	rank_unlock();
	return ok;
}

void rank_result_submit(const char *body)
{
	int wrote;

	if (!body || !body[0])
		return;
	rank_lock();
	if (!rank_state.result_sent)
	{
		wrote = snprintf(rank_state.result_body, sizeof(rank_state.result_body), "%s", body);
		rank_state.result_pending = wrote > 0 && wrote < (int)sizeof(rank_state.result_body) ? 1 : 0;
		if (!rank_state.result_pending)
			rank_state.result_body[0] = 0;
	}
	rank_unlock();
}

int rank_result_pending(char *body, int body_size)
{
	int pending = 0;
	int wrote;

	if (!body || body_size < 16)
		return 0;
	rank_lock();
	if (rank_state.result_pending && rank_state.match_id[0] && rank_state.result_body[0])
	{
		wrote = snprintf(body, (size_t)body_size, "%s", rank_state.result_body);
		pending = wrote > 0 && wrote < body_size;
	}
	rank_unlock();
	return pending;
}

void rank_result_sent(void)
{
	rank_lock();
	rank_state.result_pending = 0;
	rank_state.result_sent = 1;
	rank_unlock();
}

void rank_mesh_progress(int games, int level, int wins, int losses)
{
	rank_lock();
	if (!rank_state.current_path[0] || !rank_state.current.token[0])
	{
		rank_unlock();
		return;
	}
	if (games >= 0)
		rank_state.current.games = games;
	if (level >= 0)
		rank_state.current.level = level;
	if (wins >= 0)
		rank_state.current.wins = wins;
	if (losses >= 0)
		rank_state.current.losses = losses;
	rank_save_path(rank_state.current_path, &rank_state.current);
	rank_unlock();
}
