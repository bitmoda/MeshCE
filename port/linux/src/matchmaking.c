/*
MATCHMAKING.C

The matchmaking service. Find Game uses the peer mesh. This client does not
open a broker and does not publish a queue. The catalog below is the local
list.

The catalog is the menu. Choosing a playlist opens this machine's private
lobby on that playlist's port. Find Game sets ready. role host updates the
lobby already open. role wait keeps it from starting. role server or join,
with an invite and phase search, leaves this lobby and joins that invite.
*/

#include "platform.h"
#include "posix.h"
#include "p2p.h"
#include "halo_port_limits.h"
#include "matchmaking.h"
#include "peer_mesh.h"
#include "rank_account.h"
#include "mesh_service.h"
#include "networking/network_game_protocol.h"

#include <stdio.h>
#include <string.h>

void game_engine_override_map_name(char const *map_name);
void network_votekick_clear_local_blocks(void);

enum
{
	QUEUE_INTERVAL = 5000,
	KEEP_ALIVE = 30,
	LINK_BUFFER = 16384,
	/* one id for this install */
	ID_GENERATED = 16,
	ID_LIMIT = 64
};

static pthread_mutex_t matchmaking_lock = PTHREAD_MUTEX_INITIALIZER;

static struct
{
	int started;
	int listening;
	int queueing;
	int published_queue;
	int want_clear;
	int committed;
	int want_hosted;
	int hosted_sent;
	/* the retained invite is removed once this machine is not that host */
	int want_unhost;
	int host_mark;
	/* offers are ignored until this search has published */
	int search;
	int accept_offers;
	int epoch;
	/* in the private lobby: closing the playlist screen must not stop the queue */
	int in_party;
	/* this machine followed someone else's join link. it stays through the match. */
	int joined_link;
	/* this find-game join returns to the search if that lobby is full */
	int rejoin_search;
	int resume_ui;
	char resume_playlist[16];
	int ready;
	int locals;
	/* humans in this lobby, not the seats the service fills */
	int players;
	/* after a match, the countdown waits until this many humans are seated again.
	0 means this search has not returned from a match. */
	int rematch_players;
	/* wait_seconds 0 is a backstop, not an immediate start */
	unsigned long backstop_at;
	int backstop_seated;
	int backstop_hold;
	int backstop_absent;
	int party_mark;
	int link;
	int id_ready;
	unsigned short game_port;
	unsigned short host_port;
	char id[ID_LIMIT + 1];
	char playlist[16];
	char name[12];
	char host_playlist[16];
	int offer_ready;
	struct matchmaking_offer offer;
	int catalog_ready;
	struct matchmaking_catalog catalog;
} matchmaking;

/* sockaddr ports are network order, the same swap the invite broker uses */
static unsigned short host_order_short(unsigned short value)
{
	return (unsigned short)((value << 8) | (value >> 8));
}

static void backstop_clear_locked(void)
{
	matchmaking.backstop_at = 0;
	matchmaking.backstop_seated = 0;
	matchmaking.backstop_hold = 0;
	matchmaking.backstop_absent = 0;
}

static int same_word(const char *text, const char *end, const char *word)
{
	int length = (int)strlen(word);

	return end - text == length && !memcmp(text, word, (size_t)length);
}

static const char *skip_space(const char *text)
{
	while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r')
		text++;
	return text;
}

static const char *skip_json(const char *text)
{
	text = skip_space(text);
	if (*text == '"')
	{
		text++;
		while (*text && *text != '"')
			text += *text == '\\' && text[1] ? 2 : 1;
		return *text ? text + 1 : text;
	}
	if (*text == '{' || *text == '[')
	{
		char open = *text;
		char close = open == '{' ? '}' : ']';
		int depth = 0;

		do
		{
			if (*text == '"')
			{
				text = skip_json(text);
				continue;
			}
			if (*text == open)
				depth++;
			else if (*text == close)
				depth--;
			if (*text)
				text++;
		} while (*text && depth);
		return text;
	}
	while (*text && *text != ',' && *text != '}' && *text != ']' && *text != ' ' && *text != '\t' &&
		*text != '\n' && *text != '\r')
		text++;
	return text;
}

static int object_field(const char *object, const char *key, const char **start, const char **end)
{
	const char *text = skip_space(object);

	if (*text != '{')
		return 0;
	text++;
	while (*text)
	{
		const char *name;
		const char *name_end;
		const char *value;

		text = skip_space(text);
		if (*text == '}')
			return 0;
		if (*text != '"')
			return 0;
		name = ++text;
		while (*text && *text != '"')
			text++;
		name_end = text;
		if (*text == '"')
			text++;
		text = skip_space(text);
		if (*text != ':')
			return 0;
		text = skip_space(text + 1);
		value = text;
		text = skip_json(text);
		if (same_word(name, name_end, key))
		{
			*start = value;
			*end = text;
			return 1;
		}
		text = skip_space(text);
		if (*text == ',')
			text++;
	}
	return 0;
}

static int copy_json_string(const char *start, const char *end, char *text, int size)
{
	int length = 0;

	start = skip_space(start);
	if (size < 1 || *start != '"')
		return 0;
	start++;
	while (start < end && *start != '"')
	{
		char character = *start++;

		if (character == '\\' && start < end)
		{
			character = *start++;
			if (character == 'n')
				character = '\n';
			else if (character == 't')
				character = '\t';
		}
		if (length < size - 1)
			text[length++] = character;
	}
	text[length] = 0;
	return 1;
}

static int json_string(const char *object, const char *key, char *text, int size)
{
	const char *start;
	const char *end;

	text[0] = 0;
	if (!object_field(object, key, &start, &end))
		return 0;
	return copy_json_string(start, end, text, size);
}

static int json_number(const char *object, const char *key, long *value)
{
	const char *start;
	const char *end;
	char *stopped;
	long number;

	if (!object_field(object, key, &start, &end))
		return 0;
	start = skip_space(start);
	number = strtol(start, &stopped, 10);
	if (stopped == start)
		return 0;
	*value = number;
	return 1;
}

static int json_bool(const char *object, const char *key, int *value)
{
	const char *start;
	const char *end;

	if (!object_field(object, key, &start, &end))
		return 0;
	start = skip_space(start);
	if (!strncmp(start, "true", 4))
		*value = 1;
	else if (!strncmp(start, "false", 5))
		*value = 0;
	else
		return 0;
	return 1;
}

static int weapon_named(const char *name)
{
	if (!strcmp(name, "assault_rifle"))
		return 2;
	if (!strcmp(name, "pistol"))
		return 3;
	if (!strcmp(name, "shotgun"))
		return 4;
	if (!strcmp(name, "sniper_rifle"))
		return 5;
	if (!strcmp(name, "rocket_launcher"))
		return 6;
	if (!strcmp(name, "plasma_pistol"))
		return 7;
	if (!strcmp(name, "plasma_rifle"))
		return 8;
	if (!strcmp(name, "needler"))
		return 9;
	if (!strcmp(name, "random"))
		return 1;
	return 0;
}

static int mode_named(const char *name)
{
	if (!strcmp(name, "oddball"))
		return MATCHMAKING_MODE_ODDBALL;
	if (!strcmp(name, "king"))
		return MATCHMAKING_MODE_KING;
	if (!strcmp(name, "ctf"))
		return MATCHMAKING_MODE_CTF;
	return MATCHMAKING_MODE_SLAYER;
}

static int map_index(const char *map)
{
	static const char *const maps[] = { "chillout", "prisoner", "carousel", "hangemhigh", "beavercreek" };
	int index;

	for (index = 0; index < (int)(sizeof(maps) / sizeof(maps[0])); index++)
	{
		if (!strcmp(map, maps[index]))
			return index;
	}
	return 0;
}

static void rules_default(struct matchmaking_rules *rules, const char *playlist, const char *map)
{
	static const int objective_modes[5] = { MATCHMAKING_MODE_ODDBALL, MATCHMAKING_MODE_KING, MATCHMAKING_MODE_CTF,
		MATCHMAKING_MODE_ODDBALL, MATCHMAKING_MODE_KING };
	int objective = !strcmp(playlist, "objective");
	int hardcore = !strcmp(playlist, "hardcore");
	int ranked = !strcmp(playlist, "ranked");

	memset(rules, 0, sizeof(*rules));
	rules->mode = objective ? objective_modes[map_index(map)] : MATCHMAKING_MODE_SLAYER;
	rules->respawn_ticks = 150;
	rules->goal_radar = 1;
	rules->radar_players = 2;
	rules->primary = 2;
	rules->secondary = 3;
	rules->score = rules->mode == MATCHMAKING_MODE_SLAYER ? mesh_score(playlist) :
		rules->mode == MATCHMAKING_MODE_CTF ? 3 : 5;
	rules->no_death_bonus = 1;
	rules->no_kill_penalty = 1;
	rules->teams = objective || hardcore || ranked;
	rules->ball_spawn_count = 1;
}

static void rules_read(struct matchmaking_rules *rules, const char *object)
{
	const char *rules_text = NULL;
	const char *rules_end = NULL;
	char word[32];
	long number;
	int flag;
	char stored[1024];

	if (!object_field(object, "rules", &rules_text, &rules_end))
		return;
	if ((int)(rules_end - rules_text) >= (int)sizeof(stored))
		return;
	memcpy(stored, rules_text, (size_t)(rules_end - rules_text));
	stored[rules_end - rules_text] = 0;
	if (json_string(stored, "mode", word, sizeof(word)) || json_string(object, "mode", word, sizeof(word)))
		rules->mode = mode_named(word);
	if (json_number(stored, "respawn_ticks", &number))
		rules->respawn_ticks = number;
	if (json_number(stored, "respawn_growth_ticks", &number))
		rules->respawn_growth_ticks = number;
	if (json_number(stored, "suicide_penalty_ticks", &number))
		rules->suicide_penalty_ticks = number;
	if (json_bool(stored, "motion_sensor", &flag))
		rules->motion_sensor = flag;
	if (json_number(stored, "goal_radar", &number))
		rules->goal_radar = number;
	if (json_string(stored, "radar_players", word, sizeof(word)))
		rules->radar_players = !strcmp(word, "all") ? 0 : !strcmp(word, "friends") ? 1 : 2;
	else if (json_number(stored, "radar_players", &number))
		rules->radar_players = (int)number;
	if (json_string(stored, "primary", word, sizeof(word)))
		rules->primary = weapon_named(word);
	if (json_string(stored, "secondary", word, sizeof(word)))
		rules->secondary = weapon_named(word);
	if (json_number(stored, "time_limit_ticks", &number))
		rules->time_limit_ticks = number;
	if (json_number(stored, "score", &number))
		rules->score = number;
	if (json_bool(stored, "no_death_bonus", &flag))
		rules->no_death_bonus = flag;
	if (json_bool(stored, "no_kill_penalty", &flag))
		rules->no_kill_penalty = flag;
	if (json_bool(stored, "teams", &flag))
		rules->teams = flag;
	if (json_bool(stored, "moving_hill", &flag))
		rules->moving_hill = flag;
	if (json_number(stored, "ball_spawn_count", &number) || json_number(stored, "balls", &number) ||
		json_number(stored, "ball_count", &number))
		rules->ball_spawn_count = number;
}

static int role_named(const char *name)
{
	if (!strcmp(name, "wait"))
		return MATCHMAKING_ROLE_WAIT;
	if (!strcmp(name, "server"))
		return MATCHMAKING_ROLE_SERVER;
	if (!strcmp(name, "host"))
		return MATCHMAKING_ROLE_HOST;
	if (!strcmp(name, "join"))
		return MATCHMAKING_ROLE_JOIN;
	if (!strcmp(name, "error"))
		return MATCHMAKING_ROLE_ERROR;
	return MATCHMAKING_ROLE_NONE;
}

static int port_ok(long port)
{
	return port > 0 && port < 65536 && port != 5150 && port != 5151 &&
		port != 5160 && port != 5161 && port != 5162;
}

/* the catalog's port for this playlist, else the ports the service uses */
static unsigned short playlist_port(const char *playlist)
{
	int index;

	for (index = 0; index < matchmaking.catalog.count; index++)
	{
		if (!strcmp(matchmaking.catalog.playlist[index].id, playlist) &&
			port_ok(matchmaking.catalog.playlist[index].port))
			return matchmaking.catalog.playlist[index].port;
	}
	if (!strcmp(playlist, "ffa"))
		return (unsigned short)NETWORK_PLAYLIST_PORT_FFA;
	if (!strcmp(playlist, "hardcore"))
		return (unsigned short)NETWORK_PLAYLIST_PORT_HARDCORE;
	if (!strcmp(playlist, "ranked"))
		return (unsigned short)NETWORK_PLAYLIST_PORT_RANKED;
	return 0;
}

/* free for all holds sixteen. 2v2 and ranked hold four. */
static int players_for(const char *playlist)
{
	if (playlist && (!strcmp(playlist, "hardcore") || !strcmp(playlist, "ranked")))
		return 4;
	return 16;
}

/* one screen can fill a social lobby. ranked keeps a second machine in the match. */
static int local_cap_for(const char *playlist)
{
	return playlist && !strcmp(playlist, "ranked") ? 2 : 4;
}

static int map_ok(const char *map)
{
	int index;

	if (!map[0] || strlen(map) > 31)
		return 0;
	for (index = 0; map[index]; index++)
	{
		char character = map[index];

		if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9') || character == '_'))
			return 0;
	}
	return 1;
}

static int array_item(const char *array, int index, const char **start, const char **end)
{
	const char *text = skip_space(array);
	int at = 0;

	if (*text != '[')
		return 0;
	text++;
	while (*text)
	{
		const char *value;

		text = skip_space(text);
		if (*text == ']')
			return 0;
		value = text;
		text = skip_json(text);
		if (at == index)
		{
			*start = value;
			*end = text;
			return 1;
		}
		at++;
		text = skip_space(text);
		if (*text == ',')
			text++;
	}
	return 0;
}

static void catalog_store(const char *payload)
{
	struct matchmaking_catalog catalog;
	const char *list;
	const char *list_end;
	char stored[4096];
	long number;
	int index;

	memset(&catalog, 0, sizeof(catalog));
	if (json_number(payload, "network", &number))
		catalog.network = (int)number;
	if (!object_field(payload, "playlists", &list, &list_end))
		return;
	if ((int)(list_end - list) >= (int)sizeof(stored))
		return;
	memcpy(stored, list, (size_t)(list_end - list));
	stored[list_end - list] = 0;
	for (index = 0; index < (int)(sizeof(catalog.playlist) / sizeof(catalog.playlist[0])); index++)
	{
		struct matchmaking_playlist *row = &catalog.playlist[catalog.count];
		const char *item;
		const char *item_end;
		char object[1024];
		const char *modes;
		const char *modes_end;
		char mode_list[256];
		char kind[16];
		int mode;

		if (!array_item(stored, index, &item, &item_end))
			break;
		if ((int)(item_end - item) >= (int)sizeof(object))
			break;
		memcpy(object, item, (size_t)(item_end - item));
		object[item_end - item] = 0;
		if (!json_string(object, "id", row->id, sizeof(row->id)) || !row->id[0])
			continue;
		/* the chooser keeps ffa and hardcore */
		if (!strcmp(row->id, "objective"))
			continue;
		json_string(object, "name", row->name, sizeof(row->name));
		if (!row->name[0])
			snprintf(row->name, sizeof(row->name), "%s", row->id);
		if (json_number(object, "players", &number) && number >= 2 && number <= 128)
			row->players = (int)number;
		if (!strcmp(row->id, "ffa"))
			row->players = players_for(row->id);
		if (json_bool(object, "teams", &mode))
			row->teams = mode ? 1 : 0;
		if (json_number(object, "team_size", &number) && number >= 1 && number <= 16)
			row->team_size = (int)number;
		row->local_max = 4;
		if (json_number(object, "local_max", &number) && number >= 1 && number <= 4)
			row->local_max = (int)number;
		if (json_number(object, "port", &number) && port_ok(number))
			row->port = (unsigned short)number;
		if (object_field(object, "modes", &modes, &modes_end) &&
			(int)(modes_end - modes) < (int)sizeof(mode_list))
		{
			memcpy(mode_list, modes, (size_t)(modes_end - modes));
			mode_list[modes_end - modes] = 0;
			for (mode = 0; mode < 4; mode++)
			{
				const char *mode_text;
				const char *mode_end;

				if (!array_item(mode_list, mode, &mode_text, &mode_end))
					break;
				if (copy_json_string(mode_text, mode_end, row->modes[row->mode_count], sizeof(row->modes[0])))
					row->mode_count++;
			}
		}
		row->ranked = 0;
		if (json_string(object, "kind", kind, (int)sizeof(kind)) && !strcmp(kind, "ranked"))
			row->ranked = 1;
		catalog.count++;
	}
	if (!catalog.count)
		return;
	pthread_mutex_lock(&matchmaking_lock);
	matchmaking.catalog = catalog;
	matchmaking.catalog_ready = 1;
	pthread_mutex_unlock(&matchmaking_lock);
}

static void offer_name_keep(char *text)
{
	int read = 0;
	int write = 0;

	while (text[read] && write < MATCHMAKING_LOBBY_NAME_LENGTH - 1)
	{
		unsigned char character = (unsigned char)text[read++];

		if (character >= 32)
			text[write++] = (char)character;
	}
	text[write] = 0;
}

static void offer_names(struct matchmaking_offer *offer, const char *payload)
{
	const char *list;
	const char *list_end;
	char stored[1024];
	int length;
	int index;

	if (!object_field(payload, "names", &list, &list_end))
		return;
	length = (int)(list_end - list);
	if (length < 0 || length >= (int)sizeof(stored))
		return;
	memcpy(stored, list, (size_t)length);
	stored[length] = 0;
	for (index = 0; index < MATCHMAKING_LOBBY_NAMES; index++)
	{
		const char *item;
		const char *item_end;

		if (!array_item(stored, index, &item, &item_end))
			break;
		if (!copy_json_string(item, item_end, offer->names[offer->name_count],
			MATCHMAKING_LOBBY_NAME_LENGTH))
			continue;
		offer_name_keep(offer->names[offer->name_count]);
		if (offer->names[offer->name_count][0])
			offer->name_count++;
	}
}

static int offer_name_slot(struct matchmaking_offer *offer, const char *text)
{
	int index;

	for (index = 0; index < offer->name_count; index++)
	{
		if (!strcmp(offer->names[index], text))
			return index;
	}
	if (offer->name_count >= MATCHMAKING_LOBBY_NAMES)
		return -1;
	index = offer->name_count++;
	snprintf(offer->names[index], MATCHMAKING_LOBBY_NAME_LENGTH, "%s", text);
	return index;
}

/* "parties": [["Alice", "Bob"], ["Carol", "Dave"]]. each inner array shares a team. */
static void offer_parties(struct matchmaking_offer *offer, const char *payload)
{
	const char *list;
	const char *list_end;
	char stored[2048];
	int length;
	int party;

	if (!object_field(payload, "parties", &list, &list_end))
		return;
	length = (int)(list_end - list);
	if (length < 0 || length >= (int)sizeof(stored))
		return;
	memcpy(stored, list, (size_t)length);
	stored[length] = 0;
	for (party = 0; party < 4; party++)
	{
		const char *item;
		const char *item_end;
		char inner[512];
		int inner_length;
		int member;
		int counted = 0;

		if (!array_item(stored, party, &item, &item_end))
			break;
		inner_length = (int)(item_end - item);
		if (inner_length < 0 || inner_length >= (int)sizeof(inner))
			break;
		memcpy(inner, item, (size_t)inner_length);
		inner[inner_length] = 0;
		for (member = 0; member < MATCHMAKING_LOBBY_NAMES; member++)
		{
			const char *name;
			const char *name_end;
			char text[MATCHMAKING_LOBBY_NAME_LENGTH];
			int slot;

			if (!array_item(inner, member, &name, &name_end))
				break;
			if (!copy_json_string(name, name_end, text, (int)sizeof(text)))
				continue;
			offer_name_keep(text);
			if (!text[0])
				continue;
			slot = offer_name_slot(offer, text);
			if (slot < 0)
				break;
			offer->name_party[slot] = party;
			counted = 1;
		}
		if (counted)
			offer->party_count = party + 1;
	}
}

static void offer_cards(struct matchmaking_offer *offer, const char *payload)
{
	const char *list;
	const char *list_end;
	char stored[4096];
	int length;
	int index;

	if (!object_field(payload, "cards", &list, &list_end))
		return;
	length = (int)(list_end - list);
	if (length < 0 || length >= (int)sizeof(stored))
		return;
	memcpy(stored, list, (size_t)length);
	stored[length] = 0;
	for (index = 0; index < MATCHMAKING_CARDS; index++)
	{
		struct matchmaking_card *card;
		const char *item;
		const char *item_end;
		char object[512];
		long number;

		if (!array_item(stored, index, &item, &item_end))
			break;
		if ((int)(item_end - item) >= (int)sizeof(object))
			break;
		memcpy(object, item, (size_t)(item_end - item));
		object[item_end - item] = 0;
		card = &offer->cards[offer->card_count];
		if (!json_string(object, "name", card->name, (int)sizeof(card->name)) || !card->name[0])
			continue;
		offer_name_keep(card->name);
		json_string(object, "account", card->account, (int)sizeof(card->account));
		json_string(object, "core", card->core, (int)sizeof(card->core));
		if (json_number(object, "foreground", &number))
			card->foreground = (int)number;
		if (json_number(object, "background", &number))
			card->background = (int)number;
		if (json_number(object, "foregroundColor", &number))
			card->foreground_color = (int)number;
		if (json_number(object, "backgroundColor", &number))
			card->background_color = (int)number;
		if (json_number(object, "games", &number))
			card->games = (int)number;
		if (json_number(object, "level", &number))
			card->level = (int)number;
		offer->card_count++;
	}
}

static void offer_clear(struct matchmaking_offer *offer)
{
	int index;

	memset(offer, 0, sizeof(*offer));
	offer->joined = -1;
	offer->wait_seconds = -1;
	offer->bots = -1;
	for (index = 0; index < MATCHMAKING_LOBBY_NAMES; index++)
		offer->name_party[index] = -1;
}

static void offer_read(struct matchmaking_offer *offer, const char *payload, const char *playlist)
{
	char word[32];
	long number;
	int teams;

	offer_clear(offer);
	offer->local_max = 4;
	snprintf(offer->playlist, sizeof(offer->playlist), "%s", playlist);
	if (!json_string(payload, "playlist", offer->playlist, sizeof(offer->playlist)))
		snprintf(offer->playlist, sizeof(offer->playlist), "%s", playlist);
	json_string(payload, "map", offer->map, sizeof(offer->map));
	json_string(payload, "name", offer->name, sizeof(offer->name));
	json_string(payload, "invite", offer->invite, sizeof(offer->invite));
	json_string(payload, "match", offer->match, sizeof(offer->match));
	if (!map_ok(offer->map))
		offer->map[0] = 0;
	rules_default(&offer->rules, offer->playlist, offer->map[0] ? offer->map : "chillout");
	rules_read(&offer->rules, payload);
	if (json_string(payload, "mode", word, sizeof(word)))
		offer->rules.mode = mode_named(word);
	offer->players = players_for(offer->playlist);
	if (json_number(payload, "players", &number) && number >= 2 && number <= 128)
		offer->players = (int)number;
	if (!strcmp(offer->playlist, "ffa"))
		offer->players = players_for(offer->playlist);
	if (json_number(payload, "joined", &number) && number >= 0 && number <= 128)
		offer->joined = (int)number;
	if (json_number(payload, "wait_seconds", &number) && number >= 0 && number <= 600)
		offer->wait_seconds = (int)number;
	if (json_string(payload, "phase", word, sizeof(word)))
	{
		if (!strcmp(word, "party"))
			offer->phase = MATCHMAKING_PHASE_PARTY;
		else if (!strcmp(word, "search"))
			offer->phase = MATCHMAKING_PHASE_SEARCH;
	}
	if (json_number(payload, "local_max", &number) && number >= 1 && number <= 4)
		offer->local_max = (int)number;
	offer_names(offer, payload);
	offer_parties(offer, payload);
	offer_cards(offer, payload);
	if (json_number(payload, "bots", &number) && number >= 0 && number <= 16)
		offer->bots = (int)number;
	if (json_bool(payload, "teams", &teams))
		offer->rules.teams = teams ? 1 : 0;
	offer->teams = offer->rules.teams;
	if (json_number(payload, "team_size", &number) && number >= 1 && number <= 16)
		offer->team_size = (int)number;
	offer->port = 0;
	if (json_number(payload, "port", &number) && port_ok(number))
		offer->port = (unsigned short)number;
	else if (port_ok(playlist_port(offer->playlist)))
		offer->port = playlist_port(offer->playlist);
	if (json_number(payload, "network", &number))
		offer->network = (int)number;
	if (json_string(payload, "error", word, sizeof(word)) && !strcmp(word, "network"))
		offer->role = MATCHMAKING_ROLE_ERROR;
	else
	{
		if (!json_string(payload, "role", word, sizeof(word)))
			word[0] = 0;
		offer->role = role_named(word);
	}
}

static void *matchmaking_thread(void *unused)
{
	(void)unused;
	for (;;)
	{
		int active;

		pthread_mutex_lock(&matchmaking_lock);
		active = matchmaking.listening || matchmaking.queueing;
		pthread_mutex_unlock(&matchmaking_lock);
		if (active)
			mesh_account_serve();
		posix_socket_select(NULL, NULL, NULL, NULL, NULL, NULL, active ? 1 : 0, active ? 0 : 200000, 0);
	}
	return NULL;
}

static void matchmaking_start(void)
{
	pthread_t thread;

	if (pthread_create(&thread, NULL, matchmaking_thread, NULL) != 0)
	{
		pthread_mutex_lock(&matchmaking_lock);
		matchmaking.started = 0;
		pthread_mutex_unlock(&matchmaking_lock);
		return;
	}
	pthread_detach(thread);
}

static void make_id(char *id)
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";
	unsigned char bytes[ID_GENERATED];
	int index;

	posix_random_bytes(bytes, sizeof(bytes));
	for (index = 0; index < ID_GENERATED; index++)
		id[index] = alphabet[bytes[index] % (sizeof(alphabet) - 1)];
	id[ID_GENERATED] = 0;
}

static int id_ok(const char *id)
{
	int length = 0;

	if (!id)
		return 0;
	while (id[length])
	{
		char character = id[length];

		if (!((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
			(character >= '0' && character <= '9') || character == '_' || character == '-'))
			return 0;
		length++;
		if (length > ID_LIMIT)
			return 0;
	}
	return length >= 8;
}

static void id_path(char *path, int size)
{
	/* the same folder as profiles, which is writable on Mac, Windows, and Linux */
	const char *root = platform_save_root();

	if (!root || !root[0])
		root = ".";
	snprintf(path, (size_t)size, "%s/matchmaking.id", root);
}

/* the same id for this install, so a later search replaces the last queue */
static void id_load(void)
{
	char path[1024];
	char stored[ID_LIMIT + 8];
	FILE *file;
	int ready;

	pthread_mutex_lock(&matchmaking_lock);
	ready = matchmaking.id_ready;
	pthread_mutex_unlock(&matchmaking_lock);
	if (ready)
		return;
	memset(stored, 0, sizeof(stored));
	id_path(path, sizeof(path));
	file = fopen(path, "rb");
	if (file)
	{
		if (fgets(stored, sizeof(stored), file))
		{
			int length = (int)strlen(stored);

			while (length > 0 && (stored[length - 1] == '\n' || stored[length - 1] == '\r'))
				stored[--length] = 0;
		}
		fclose(file);
	}
	if (!id_ok(stored))
	{
		make_id(stored);
		file = fopen(path, "wb");
		if (file)
		{
			fputs(stored, file);
			fputc('\n', file);
			fclose(file);
		}
	}
	pthread_mutex_lock(&matchmaking_lock);
	if (!matchmaking.id_ready)
	{
		memcpy(matchmaking.id, stored, sizeof(matchmaking.id));
		matchmaking.id[sizeof(matchmaking.id) - 1] = 0;
		matchmaking.id_ready = 1;
	}
	pthread_mutex_unlock(&matchmaking_lock);
}

static int playlist_listed(const char *playlist)
{
	int index;

	if (!playlist || !playlist[0])
		return 0;
	for (index = 0; index < matchmaking.catalog.count; index++)
	{
		if (!strcmp(matchmaking.catalog.playlist[index].id, playlist))
			return 1;
	}
	return 0;
}

static void matchmaking_ensure(void)
{
	int start;

	pthread_mutex_lock(&matchmaking_lock);
	start = !matchmaking.started;
	if (start)
		matchmaking.started = 1;
	pthread_mutex_unlock(&matchmaking_lock);
	if (start)
		matchmaking_start();
}

static void copy_short(char *destination, int size, const char *text, int limit)
{
	int length = 0;

	if (!text)
		text = "";
	while (*text && length < limit && length < size - 1)
	{
		unsigned char character = (unsigned char)*text++;

		destination[length++] = character >= 32 && character < 127 ? (char)character : '?';
	}
	destination[length] = 0;
}

static void catalog_fallback_locked(void);

void matchmaking_watch(void)
{
	id_load();
	peer_mesh_start();
	pthread_mutex_lock(&matchmaking_lock);
	matchmaking.listening = 1;
	catalog_fallback_locked();
	pthread_mutex_unlock(&matchmaking_lock);
	matchmaking_ensure();
}

static void catalog_fallback_locked(void)
{
	struct matchmaking_playlist *row;

	if (matchmaking.catalog_ready)
		return;
	memset(&matchmaking.catalog, 0, sizeof(matchmaking.catalog));
	matchmaking.catalog.network = HALO_PORT_NETWORK_VERSION;
	row = &matchmaking.catalog.playlist[0];
	snprintf(row->id, sizeof(row->id), "ffa");
	snprintf(row->name, sizeof(row->name), "FFA");
	row->players = players_for(row->id);
	row->team_size = 1;
	row->local_max = 4;
	row->port = (unsigned short)NETWORK_PLAYLIST_PORT_FFA;
	snprintf(row->modes[0], sizeof(row->modes[0]), "slayer");
	row->mode_count = 1;
	row = &matchmaking.catalog.playlist[1];
	snprintf(row->id, sizeof(row->id), "hardcore");
	snprintf(row->name, sizeof(row->name), "2v2");
	row->players = 4;
	row->teams = 1;
	row->team_size = 2;
	row->local_max = 4;
	row->port = (unsigned short)NETWORK_PLAYLIST_PORT_HARDCORE;
	snprintf(row->modes[0], sizeof(row->modes[0]), "slayer");
	row->mode_count = 1;
	row = &matchmaking.catalog.playlist[2];
	snprintf(row->id, sizeof(row->id), "ranked");
	snprintf(row->name, sizeof(row->name), "Ranked");
	row->players = 4;
	row->teams = 1;
	row->team_size = 2;
	row->local_max = 2;
	row->port = (unsigned short)NETWORK_PLAYLIST_PORT_RANKED;
	row->ranked = 1;
	snprintf(row->modes[0], sizeof(row->modes[0]), "slayer");
	row->mode_count = 1;
	matchmaking.catalog.count = 3;
	matchmaking.catalog_ready = 1;
}

int matchmaking_catalog(struct matchmaking_catalog *catalog)
{
	int result = 0;

	pthread_mutex_lock(&matchmaking_lock);
	catalog_fallback_locked();
	if (matchmaking.catalog_ready)
	{
		*catalog = matchmaking.catalog;
		result = !catalog->network || catalog->network == HALO_PORT_NETWORK_VERSION ? 1 : -1;
	}
	pthread_mutex_unlock(&matchmaking_lock);
	return result;
}

/* the retained invite comes down. the queue is a separate publish. */
static void hosted_drop_locked(void)
{
	matchmaking.want_hosted = 0;
	if (matchmaking.hosted_sent)
		matchmaking.want_unhost = 1;
}

/* machines that sat in this lobby before Find Game, this host's machine,
and a later human the offer does not list. a filled seat is not here.
two searches both publish Player, which is also this host's name, so the
name cannot say who sat here. */
static unsigned char screen_party_machine[16];

static int offer_name_matches(const wchar_t *player_name, const char *offer_name);

static void screen_party_clear(void)
{
	memset(screen_party_machine, 0, sizeof(screen_party_machine));
}

static void screen_party_mark(int machine)
{
	if (machine < 0 || machine >= 128)
		return;
	screen_party_machine[machine >> 3] |= (unsigned char)(1u << (machine & 7));
}

static void screen_party_unmark(int machine)
{
	if (machine < 0 || machine >= 128)
		return;
	screen_party_machine[machine >> 3] &= (unsigned char)~(1u << (machine & 7));
}

/* 1 when the offer lists this name. have_parties is 1 when the offer has
named any party, so a missing list is not treated as an unlisted friend. */
static int screen_offer_lists(const wchar_t *name, int *have_parties)
{
	struct matchmaking_offer offer;
	int index;

	if (have_parties)
		*have_parties = 0;
	if (!name || !name[0])
		return 0;
	pthread_mutex_lock(&matchmaking_lock);
	offer = matchmaking.offer;
	pthread_mutex_unlock(&matchmaking_lock);
	if (have_parties)
		*have_parties = offer.party_count > 0;
	if (offer.party_count <= 0)
		return 0;
	for (index = 0; index < offer.name_count && index < MATCHMAKING_LOBBY_NAMES; index++)
	{
		if (offer.name_party[index] < 0)
			continue;
		if (offer_name_matches(name, offer.names[index]))
			return 1;
	}
	return 0;
}

void matchmaking_screen_party_note(int machine, int host_local, const wchar_t *name)
{
	int have_parties = 0;

	if (!matchmaking_game_port())
		return;
	/* still the private lobby: Find Game has not been pressed. once the
	match starts, queueing is cleared and a later arrival is not the lobby. */
	if (host_local || (matchmaking_queue_active() && !matchmaking_is_ready()))
	{
		screen_party_mark(machine);
		return;
	}
	if (screen_offer_lists(name, &have_parties) || !have_parties)
		return;
	screen_party_mark(machine);
}

void matchmaking_screen_party_clear_machine(int machine)
{
	screen_party_unmark(machine);
}

void matchmaking_screen_party_bits(unsigned char bits[16])
{
	if (!bits)
		return;
	memcpy(bits, screen_party_machine, 16);
}

void matchmaking_queue(const char *playlist, const char *name)
{
	id_load();
	pthread_mutex_lock(&matchmaking_lock);
	if (!playlist_listed(playlist) ||
		(matchmaking.catalog.network && matchmaking.catalog.network != HALO_PORT_NETWORK_VERSION))
	{
		pthread_mutex_unlock(&matchmaking_lock);
		return;
	}
	copy_short(matchmaking.playlist, sizeof(matchmaking.playlist), playlist, 15);
	copy_short(matchmaking.name, sizeof(matchmaking.name), name && name[0] ? name : "Player", 11);
	matchmaking.listening = 1;
	matchmaking.queueing = 1;
	matchmaking.committed = 0;
	matchmaking.in_party = 0;
	matchmaking.joined_link = 0;
	matchmaking.ready = 0;
	matchmaking.locals = 1;
	matchmaking.players = 1;
	backstop_clear_locked();
	matchmaking.party_mark++;
	hosted_drop_locked();
	matchmaking.want_clear = 0;
	matchmaking.offer_ready = 0;
	offer_clear(&matchmaking.offer);
	matchmaking.accept_offers = 0;
	matchmaking.search++;
	matchmaking.rejoin_search = 0;
	matchmaking.resume_ui = 0;
	matchmaking.resume_playlist[0] = 0;
	matchmaking.rematch_players = 0;
	pthread_mutex_unlock(&matchmaking_lock);
	screen_party_clear();
	network_votekick_clear_local_blocks();
	matchmaking_ensure();
}

void matchmaking_set_locals(int locals)
{
	if (locals < 1)
		locals = 1;
	if (locals > 4)
		locals = 4;
	pthread_mutex_lock(&matchmaking_lock);
	if (matchmaking.queueing && !matchmaking.ready && locals != matchmaking.locals)
	{
		matchmaking.locals = locals;
		matchmaking.party_mark++;
	}
	pthread_mutex_unlock(&matchmaking_lock);
}

void matchmaking_set_players(int players)
{
	if (players < 1)
		players = 1;
	if (players > 16)
		players = 16;
	pthread_mutex_lock(&matchmaking_lock);
	if (matchmaking.queueing && players != matchmaking.players)
	{
		matchmaking.players = players;
		matchmaking.party_mark++;
	}
	pthread_mutex_unlock(&matchmaking_lock);
}

/* the lock is held. a rematch keeps the countdown off until the players who
finished are seated again. any other ready lobby counts down for 30 seconds. */
static int seated_wait_locked(const char *playlist, int humans, int cap, int seconds)
{
	int wait = mesh_wait_seconds(playlist, humans, cap, seconds);

	if (matchmaking.rematch_players > 1 && humans < matchmaking.rematch_players)
		return -1;
	return wait;
}

void matchmaking_note_seated(int humans)
{
	struct matchmaking_offer offer;
	int seconds;
	int cap;

	if (humans < 1)
		humans = 1;
	if (humans > 16)
		humans = 16;
	/* recorded before the offer update. a party-phase return must still tell
	the search that someone is already sitting here. */
	peer_mesh_set_game_humans(humans);
	seconds = peer_mesh_search_seconds();
	pthread_mutex_lock(&matchmaking_lock);
	if (!matchmaking.queueing || !matchmaking.ready ||
		matchmaking.offer.role != MATCHMAKING_ROLE_HOST ||
		matchmaking.offer.phase == MATCHMAKING_PHASE_PARTY)
	{
		pthread_mutex_unlock(&matchmaking_lock);
		return;
	}
	offer = matchmaking.offer;
	cap = players_for(offer.playlist);
	if (cap > 0 && humans > cap)
		humans = cap;
	offer.joined = humans;
	offer.wait_seconds = seated_wait_locked(offer.playlist, humans, cap, seconds);
	matchmaking.offer = offer;
	if (matchmaking.players != humans)
	{
		matchmaking.players = humans;
		matchmaking.party_mark++;
	}
	pthread_mutex_unlock(&matchmaking_lock);
	peer_mesh_set_game_humans(humans);
	peer_mesh_set_lobby_full(cap > 0 && humans >= cap);
}

int matchmaking_begin_rematch(int humans)
{
	unsigned char id[16];
	char hex[40];
	char map_name[64];
	char playlist[16];
	const char *map;
	int index;
	int cap;
	int seconds;
	int ok;

	pthread_mutex_lock(&matchmaking_lock);
	ok = matchmaking.game_port && matchmaking.playlist[0];
	playlist[0] = 0;
	if (ok)
		copy_short(playlist, (int)sizeof(playlist), matchmaking.playlist, 15);
	pthread_mutex_unlock(&matchmaking_lock);
	if (!ok)
		return 0;
	posix_random_bytes(id, 16);
	for (index = 0; index < 16; index++)
		snprintf(hex + index * 2, 3, "%02x", id[index]);
	hex[32] = 0;
	peer_mesh_set_rematch(id);
	seconds = peer_mesh_search_seconds();
	map = mesh_map_for(id, playlist);
	snprintf(map_name, sizeof(map_name), "%s", map ? map : "hangemhigh");
	if (humans < 2)
		humans = 2;
	cap = players_for(playlist);
	if (cap > 0 && humans > cap)
		humans = cap;
	pthread_mutex_lock(&matchmaking_lock);
	if (!matchmaking.game_port || !matchmaking.playlist[0])
	{
		pthread_mutex_unlock(&matchmaking_lock);
		return 0;
	}
	matchmaking.rematch_players = humans;
	matchmaking.players = humans;
	matchmaking.queueing = 1;
	matchmaking.ready = 1;
	matchmaking.in_party = 1;
	matchmaking.offer.role = MATCHMAKING_ROLE_HOST;
	matchmaking.offer.phase = MATCHMAKING_PHASE_SEARCH;
	copy_short(matchmaking.offer.playlist, (int)sizeof(matchmaking.offer.playlist), playlist, 15);
	copy_short(matchmaking.offer.match, (int)sizeof(matchmaking.offer.match), hex, 39);
	copy_short(matchmaking.offer.map, (int)sizeof(matchmaking.offer.map), map_name, 31);
	rules_default(&matchmaking.offer.rules, playlist, matchmaking.offer.map);
	mesh_loadout(id, playlist, &matchmaking.offer.rules.primary, &matchmaking.offer.rules.secondary);
	matchmaking.offer.players = cap > 0 ? cap : humans;
	matchmaking.offer.teams = matchmaking.offer.rules.teams;
	matchmaking.offer.team_size = matchmaking.offer.teams ? 2 : 1;
	matchmaking.offer.local_max = local_cap_for(playlist);
	matchmaking.offer.bots = 0;
	matchmaking.offer.joined = humans;
	matchmaking.offer.wait_seconds = seated_wait_locked(playlist, humans, cap > 0 ? cap : humans, seconds);
	matchmaking.offer_ready = 1;
	pthread_mutex_unlock(&matchmaking_lock);
	game_engine_override_map_name(map_name);
	return 1;
}

int matchmaking_rematch_target(void)
{
	int target;

	pthread_mutex_lock(&matchmaking_lock);
	target = matchmaking.rematch_players;
	pthread_mutex_unlock(&matchmaking_lock);
	return target;
}

void matchmaking_arm_rejoin(void)
{
	pthread_mutex_lock(&matchmaking_lock);
	matchmaking.rejoin_search = matchmaking.playlist[0] ? 1 : 0;
	pthread_mutex_unlock(&matchmaking_lock);
}

void matchmaking_clear_rejoin(void)
{
	pthread_mutex_lock(&matchmaking_lock);
	matchmaking.rejoin_search = 0;
	pthread_mutex_unlock(&matchmaking_lock);
}

int matchmaking_rejoin_pending(void)
{
	int pending;

	pthread_mutex_lock(&matchmaking_lock);
	pending = matchmaking.rejoin_search && matchmaking.playlist[0];
	pthread_mutex_unlock(&matchmaking_lock);
	return pending;
}

void matchmaking_mark_resume(void)
{
	pthread_mutex_lock(&matchmaking_lock);
	if (matchmaking.rejoin_search && matchmaking.playlist[0])
	{
		copy_short(matchmaking.resume_playlist, (int)sizeof(matchmaking.resume_playlist),
			matchmaking.playlist, 15);
		matchmaking.resume_ui = 1;
	}
	matchmaking.rejoin_search = 0;
	pthread_mutex_unlock(&matchmaking_lock);
}

int matchmaking_take_resume(char *playlist, int size)
{
	int resume;

	pthread_mutex_lock(&matchmaking_lock);
	resume = matchmaking.resume_ui && matchmaking.resume_playlist[0];
	if (resume && playlist && size > 0)
		copy_short(playlist, size, matchmaking.resume_playlist, 15);
	matchmaking.resume_ui = 0;
	pthread_mutex_unlock(&matchmaking_lock);
	return resume;
}

int matchmaking_name_is_filler(const wchar_t *name)
{
	struct matchmaking_offer offer;
	char host[12];
	int index;
	int filler = 0;

	if (!name || !name[0])
		return 0;
	pthread_mutex_lock(&matchmaking_lock);
	memcpy(host, matchmaking.name, sizeof(host));
	offer = matchmaking.offer;
	pthread_mutex_unlock(&matchmaking_lock);
	/* this client's own name is in parties[0] and is not a filled seat */
	if (offer_name_matches(name, host))
		return 0;
	if (offer.party_count <= 0)
		return 0;
	for (index = 0; index < offer.name_count && index < MATCHMAKING_LOBBY_NAMES; index++)
	{
		if (offer.name_party[index] < 0)
			continue;
		if (offer_name_matches(name, offer.names[index]))
			filler = 1;
	}
	return filler;
}

void matchmaking_find_game(void)
{
	char playlist[16];
	int locals = 1;

	if (rank_search_blocks())
		return;
	playlist[0] = 0;
	pthread_mutex_lock(&matchmaking_lock);
	if (matchmaking.queueing && !matchmaking.ready)
	{
		matchmaking.ready = 1;
		matchmaking.party_mark++;
	}
	copy_short(playlist, (int)sizeof(playlist), matchmaking.playlist, 15);
	locals = matchmaking.locals;
	pthread_mutex_unlock(&matchmaking_lock);
	if (playlist[0])
		peer_mesh_search(playlist, locals);
}

void matchmaking_note_mesh(int host, const char *playlist, unsigned short port, const char *invite,
	const char *match)
{
	struct matchmaking_offer offer;
	int party_humans;
	int seconds;
	char remote[MATCHMAKING_LOBBY_NAMES][12];
	int remote_count;

	if (!playlist || !playlist[0])
		return;
	/* mesh calls take mesh_lock. that lock is not taken while matchmaking_lock
	is already held. */
	party_humans = peer_mesh_party_size();
	seconds = peer_mesh_search_seconds();
	remote_count = peer_mesh_party_names(remote, MATCHMAKING_LOBBY_NAMES);
	pthread_mutex_lock(&matchmaking_lock);
	if (!matchmaking.queueing || !matchmaking.ready)
	{
		pthread_mutex_unlock(&matchmaking_lock);
		return;
	}
	if (!host && (matchmaking.offer.role == MATCHMAKING_ROLE_JOIN ||
		matchmaking.offer.role == MATCHMAKING_ROLE_SERVER) && matchmaking.offer.invite[0])
	{
		/* a later offer can replace the address. the match itself stays. */
		if (invite && invite[0])
			copy_short(matchmaking.offer.invite, (int)sizeof(matchmaking.offer.invite), invite, 159);
		pthread_mutex_unlock(&matchmaking_lock);
		return;
	}
	offer_clear(&offer);
	offer.role = host ? MATCHMAKING_ROLE_HOST : MATCHMAKING_ROLE_SERVER;
	offer.phase = MATCHMAKING_PHASE_SEARCH;
	offer.port = port ? port : playlist_port(playlist);
	copy_short(offer.playlist, (int)sizeof(offer.playlist), playlist, 15);
	copy_short(offer.name, (int)sizeof(offer.name), !strcmp(playlist, "hardcore") ? "2v2" :
		!strcmp(playlist, "ranked") ? "Ranked" : "FFA", 31);
	if (invite && invite[0])
		copy_short(offer.invite, (int)sizeof(offer.invite), invite, 159);
	if (match && match[0])
		copy_short(offer.match, (int)sizeof(offer.match), match, 39);
	{
		unsigned char match_bytes[16];
		int index;
		int humans;

		memset(match_bytes, 0, sizeof(match_bytes));
		for (index = 0; match && match[index] && match[index + 1] && index / 2 < 16; index += 2)
		{
			int high = match[index];
			int low = match[index + 1];

			high = high >= 'a' ? high - 'a' + 10 : high >= 'A' ? high - 'A' + 10 : high - '0';
			low = low >= 'a' ? low - 'a' + 10 : low >= 'A' ? low - 'A' + 10 : low - '0';
			if (high < 0 || high > 15 || low < 0 || low > 15)
				break;
			match_bytes[index / 2] = (unsigned char)((high << 4) | low);
		}
		copy_short(offer.map, (int)sizeof(offer.map), mesh_map_for(match_bytes, playlist), 31);
		rules_default(&offer.rules, offer.playlist, offer.map);
		mesh_loadout(match_bytes, offer.playlist, &offer.rules.primary, &offer.rules.secondary);
		humans = party_humans;
		if (matchmaking.rematch_players > 1)
		{
			/* the people sitting now. their names stay, so a team game still
			knows who is on which side. the countdown follows this count, not
			those names. */
			humans = matchmaking.players;
		}
		else if (humans < matchmaking.players)
			humans = matchmaking.players;
		if (humans < 1)
			humans = 1;
		offer.joined = humans;
		offer.wait_seconds = seated_wait_locked(offer.playlist, humans, players_for(offer.playlist),
			seconds);
	}
	offer.players = players_for(offer.playlist);
	offer.teams = offer.rules.teams;
	offer.team_size = offer.teams ? 2 : 1;
	offer.local_max = local_cap_for(offer.playlist);
	offer.bots = 0;
	{
		struct rank_account account;

		rank_account_get(&account);
		if (matchmaking.name[0])
		{
			int remote_index;

			copy_short(offer.names[0], (int)sizeof(offer.names[0]), matchmaking.name, 11);
			offer.name_count = 1;
			/* Party 0 is the host. A free-for-all still lists that name, and
			each other machine, so the start waits until those players are
			actually in the lobby. */
			offer.name_party[0] = 0;
			offer.party_count = 1;
			for (remote_index = 0; remote_index < remote_count && offer.name_count < MATCHMAKING_LOBBY_NAMES;
				remote_index++)
			{
				copy_short(offer.names[offer.name_count], (int)sizeof(offer.names[0]), remote[remote_index], 11);
				offer.name_party[offer.name_count] = offer.teams ? offer.name_count : 0;
				offer.name_count++;
				offer.party_count = offer.name_count;
			}
		}
		if (account.account[0] && account.core[0])
		{
			struct matchmaking_card *card = &offer.cards[0];

			copy_short(card->name, (int)sizeof(card->name), matchmaking.name, 11);
			copy_short(card->account, (int)sizeof(card->account), account.account, 47);
			copy_short(card->core, (int)sizeof(card->core), account.core, 23);
			card->foreground = account.foreground;
			card->background = account.background;
			card->foreground_color = account.foreground_color;
			card->background_color = account.background_color;
			card->games = account.games;
			card->level = account.level;
			offer.card_count = 1;
		}
	}
	offer.host_seats = matchmaking.locals;
	matchmaking.offer = offer;
	matchmaking.offer_ready = 1;
	pthread_mutex_unlock(&matchmaking_lock);
}

int matchmaking_queue_active(void)
{
	int active;

	pthread_mutex_lock(&matchmaking_lock);
	active = matchmaking.queueing;
	pthread_mutex_unlock(&matchmaking_lock);
	return active;
}

int matchmaking_is_ready(void)
{
	int ready;

	pthread_mutex_lock(&matchmaking_lock);
	ready = matchmaking.queueing && matchmaking.ready;
	pthread_mutex_unlock(&matchmaking_lock);
	return ready;
}

int matchmaking_offer_copy(struct matchmaking_offer *offer)
{
	int ready;

	pthread_mutex_lock(&matchmaking_lock);
	ready = matchmaking.offer.role != MATCHMAKING_ROLE_NONE;
	if (ready && offer)
		*offer = matchmaking.offer;
	pthread_mutex_unlock(&matchmaking_lock);
	return ready;
}

int matchmaking_card_level(const char *name)
{
	int level = -1;
	int index;

	if (!name || !name[0])
		return -1;
	pthread_mutex_lock(&matchmaking_lock);
	for (index = 0; index < matchmaking.offer.card_count && index < MATCHMAKING_CARDS; index++)
	{
		if (!strcmp(matchmaking.offer.cards[index].name, name))
		{
			level = matchmaking.offer.cards[index].level;
			break;
		}
	}
	pthread_mutex_unlock(&matchmaking_lock);
	return level;
}

void matchmaking_leave(void)
{
	pthread_mutex_lock(&matchmaking_lock);
	/* the playlist screen closes because the lobby opened. the queue stays. */
	if (matchmaking.in_party && matchmaking.queueing)
	{
		pthread_mutex_unlock(&matchmaking_lock);
		return;
	}
	matchmaking.listening = 0;
	matchmaking.queueing = 0;
	matchmaking.in_party = 0;
	matchmaking.ready = 0;
	matchmaking.offer_ready = 0;
	matchmaking.accept_offers = 0;
	matchmaking.rejoin_search = 0;
	matchmaking.rematch_players = 0;
	if (!matchmaking.committed)
	{
		hosted_drop_locked();
		matchmaking.game_port = 0;
		if (matchmaking.published_queue)
			matchmaking.want_clear = 1;
	}
	pthread_mutex_unlock(&matchmaking_lock);
	network_votekick_clear_local_blocks();
	peer_mesh_stop();
}

void matchmaking_reset(void)
{
	pthread_mutex_lock(&matchmaking_lock);
	matchmaking.listening = 0;
	matchmaking.queueing = 0;
	matchmaking.committed = 0;
	matchmaking.in_party = 0;
	matchmaking.joined_link = 0;
	matchmaking.ready = 0;
	matchmaking.locals = 1;
	backstop_clear_locked();
	hosted_drop_locked();
	matchmaking.offer_ready = 0;
	matchmaking.accept_offers = 0;
	matchmaking.game_port = 0;
	matchmaking.rejoin_search = 0;
	matchmaking.rematch_players = 0;
	matchmaking.search++;
	if (matchmaking.published_queue)
		matchmaking.want_clear = 1;
	pthread_mutex_unlock(&matchmaking_lock);
	network_votekick_clear_local_blocks();
	screen_party_clear();
	rank_search_clear();
	peer_mesh_stop();
}

void matchmaking_stop(void)
{
	struct matchmaking_offer offer;
	int index;
	int note;

	pthread_mutex_lock(&matchmaking_lock);
	if (!matchmaking.queueing && !matchmaking.published_queue)
	{
		pthread_mutex_unlock(&matchmaking_lock);
		/* a map that is not the queued mesh match. its scoreboard stays local. */
		peer_mesh_drop_match();
		rank_match_close();
		return;
	}
	offer = matchmaking.offer;
	matchmaking.queueing = 0;
	matchmaking.in_party = 0;
	matchmaking.ready = 0;
	matchmaking.locals = 1;
	matchmaking.accept_offers = 0;
	matchmaking.offer_ready = 0;
	/* the match started and this machine is still the host, so the invite stays.
	the game is loading. a later leave of a menu must not drop the port. */
	matchmaking.committed = 1;
	matchmaking.rematch_players = 0;
	matchmaking.search++;
	pthread_mutex_unlock(&matchmaking_lock);
	peer_mesh_stop();
	if (offer.match[0] && peer_mesh_match_is(offer.match))
	{
		note = rank_match_begin(offer.match, offer.playlist, offer.teams);
		if (note)
		{
			for (index = 0; index < offer.card_count && index < MATCHMAKING_CARDS; index++)
				rank_match_note(offer.cards[index].name, offer.cards[index].account, offer.cards[index].level);
		}
		peer_mesh_arm_late_offer();
	}
	else
	{
		peer_mesh_drop_match();
		rank_match_close();
	}
}

void matchmaking_commit_join(void)
{
	pthread_mutex_lock(&matchmaking_lock);
	matchmaking.committed = 1;
	matchmaking.in_party = 1;
	matchmaking.rejoin_search = 0;
	hosted_drop_locked();
	pthread_mutex_unlock(&matchmaking_lock);
}

void matchmaking_joined_link(void)
{
	pthread_mutex_lock(&matchmaking_lock);
	matchmaking.queueing = 0;
	matchmaking.ready = 0;
	matchmaking.in_party = 1;
	matchmaking.committed = 1;
	matchmaking.accept_offers = 0;
	matchmaking.offer_ready = 0;
	matchmaking.joined_link = 1;
	matchmaking.rejoin_search = 0;
	hosted_drop_locked();
	pthread_mutex_unlock(&matchmaking_lock);
}

int matchmaking_joined_by_link(void)
{
	int joined;

	pthread_mutex_lock(&matchmaking_lock);
	joined = matchmaking.joined_link;
	pthread_mutex_unlock(&matchmaking_lock);
	return joined;
}

void matchmaking_commit_party(void)
{
	pthread_mutex_lock(&matchmaking_lock);
	matchmaking.committed = 1;
	matchmaking.in_party = 1;
	copy_short(matchmaking.host_playlist, sizeof(matchmaking.host_playlist), matchmaking.playlist, 15);
	if (port_ok(matchmaking.game_port))
		matchmaking.host_port = matchmaking.game_port;
	matchmaking.want_hosted = 1;
	matchmaking.want_unhost = 0;
	matchmaking.hosted_sent = 0;
	matchmaking.host_mark++;
	pthread_mutex_unlock(&matchmaking_lock);
}

void matchmaking_release_host(void)
{
	pthread_mutex_lock(&matchmaking_lock);
	hosted_drop_locked();
	pthread_mutex_unlock(&matchmaking_lock);
}

void matchmaking_party_offer(struct matchmaking_offer *offer, const struct matchmaking_playlist *row)
{
	unsigned short port;

	if (!offer || !row || !row->id[0])
		return;
	offer_clear(offer);
	offer->role = MATCHMAKING_ROLE_HOST;
	offer->phase = MATCHMAKING_PHASE_PARTY;
	copy_short(offer->playlist, sizeof(offer->playlist), row->id, 15);
	copy_short(offer->name, sizeof(offer->name), row->name[0] ? row->name : row->id, 31);
	copy_short(offer->map, sizeof(offer->map), "chillout", 31);
	port = port_ok(row->port) ? row->port : 0;
	if (!port)
		port = playlist_port(row->id);
	offer->port = port;
	offer->players = players_for(row->id);
	if (row->players > 1 && strcmp(row->id, "ffa") != 0)
		offer->players = row->players;
	offer->teams = row->teams;
	offer->team_size = row->team_size > 0 ? row->team_size : offer->teams ? 2 : 1;
	offer->local_max = row->local_max > 0 ? row->local_max : 4;
	offer->joined = -1;
	offer->wait_seconds = -1;
	rules_default(&offer->rules, row->id, offer->map);
}

void matchmaking_yield_to_invite(const char *text)
{
	int yield = 0;

	if (!text || !text[0])
		return;
	pthread_mutex_lock(&matchmaking_lock);
	if (matchmaking.queueing)
	{
		yield = !matchmaking.offer.invite[0] || !strstr(text, matchmaking.offer.invite);
	}
	if (yield)
	{
		matchmaking.queueing = 0;
		matchmaking.in_party = 0;
		matchmaking.joined_link = 0;
		matchmaking.ready = 0;
		matchmaking.locals = 1;
		matchmaking.accept_offers = 0;
		matchmaking.offer_ready = 0;
		matchmaking.committed = 0;
		matchmaking.game_port = 0;
		backstop_clear_locked();
		hosted_drop_locked();
		matchmaking.search++;
		if (matchmaking.published_queue)
			matchmaking.want_clear = 1;
		screen_party_clear();
	}
	pthread_mutex_unlock(&matchmaking_lock);
	if (yield)
		peer_mesh_stop();
}

void matchmaking_commit_host(const char *playlist, unsigned short port)
{
	pthread_mutex_lock(&matchmaking_lock);
	copy_short(matchmaking.host_playlist, sizeof(matchmaking.host_playlist), playlist, 15);
	matchmaking.host_port = port;
	matchmaking.committed = 1;
	matchmaking.in_party = 1;
	matchmaking.want_hosted = 1;
	matchmaking.want_unhost = 0;
	matchmaking.hosted_sent = 0;
	matchmaking.host_mark++;
	pthread_mutex_unlock(&matchmaking_lock);
}

int matchmaking_take_offer(struct matchmaking_offer *offer)
{
	int ready;

	pthread_mutex_lock(&matchmaking_lock);
	ready = matchmaking.offer_ready;
	if (ready)
	{
		*offer = matchmaking.offer;
		matchmaking.offer_ready = 0;
	}
	pthread_mutex_unlock(&matchmaking_lock);
	return ready;
}

int matchmaking_link(void)
{
	int state;

	pthread_mutex_lock(&matchmaking_lock);
	state = matchmaking.link;
	pthread_mutex_unlock(&matchmaking_lock);
	if (peer_mesh_ready())
		return 1;
	return state;
}

unsigned short matchmaking_game_port(void)
{
	return matchmaking.game_port;
}

int matchmaking_voice_open(void)
{
	struct matchmaking_offer offer;

	if (!matchmaking_game_port() || !matchmaking_offer_copy(&offer))
		return 0;
	return !strcmp(offer.playlist, "ffa");
}

int matchmaking_comms_team(void)
{
	struct matchmaking_offer offer;

	if (!matchmaking_game_port() || !matchmaking_offer_copy(&offer))
		return 0;
	return !strcmp(offer.playlist, "hardcore") || !strcmp(offer.playlist, "ranked");
}

void matchmaking_set_game_port(unsigned short port)
{
	/* 5150 is an ordinary lobby. 5151 is this machine's client port. */
	if (!port_ok(port))
		port = 0;
	matchmaking.game_port = port;
}

static int offer_name_matches(const wchar_t *player_name, const char *offer_name)
{
	int index;

	if (!player_name || !offer_name)
		return 0;
	for (index = 0; index < MATCHMAKING_LOBBY_NAME_LENGTH - 1; index++)
	{
		wchar_t left = player_name[index];
		wchar_t right = (wchar_t)(unsigned char)offer_name[index];

		if (left >= L'A' && left <= L'Z')
			left = (wchar_t)(left - L'A' + L'a');
		if (right >= L'A' && right <= L'Z')
			right = (wchar_t)(right - L'A' + L'a');
		if (left != right)
			return 0;
		if (!left)
			return 1;
	}
	return 1;
}

/* parties[0] is the host's team and parties[1] is the other. a later party
joins the team that already has fewer players. a tie stays with the first. */
static int party_seat(const struct matchmaking_offer *offer, int party)
{
	int counts[2];
	int index;
	int which;

	int host_names;
	int host_seats;

	if (party <= 0)
		return 0;
	if (party == 1)
		return 1;
	host_seats = offer->host_seats;
	if (host_seats < 1)
		host_seats = 1;
	if (host_seats > 4)
		host_seats = 4;
	host_names = 0;
	counts[0] = 0;
	counts[1] = 0;
	for (index = 0; index < offer->name_count && index < MATCHMAKING_LOBBY_NAMES; index++)
	{
		if (offer->name_party[index] == 0)
			host_names++;
		else if (offer->name_party[index] == 1)
			counts[1]++;
	}
	counts[0] = host_names > host_seats ? host_names : host_seats;
	for (which = 2; which <= party && which < 4; which++)
	{
		int members = 0;
		int team = counts[0] <= counts[1] ? 0 : 1;

		for (index = 0; index < offer->name_count && index < MATCHMAKING_LOBBY_NAMES; index++)
		{
			if (offer->name_party[index] == which)
				members++;
		}
		if (which == party)
			return team;
		counts[team] += members;
	}
	return counts[0] <= counts[1] ? 0 : 1;
}

static int party_team(const struct matchmaking_offer *offer, int party)
{
	if (party <= 0)
		return 0;
	if (party == 1)
		return 1;
	return party_seat(offer, party);
}

static int name_listed_in_party(const struct matchmaking_offer *offer, const wchar_t *name, int party)
{
	int index;

	for (index = 0; index < offer->name_count && index < MATCHMAKING_LOBBY_NAMES; index++)
	{
		if (offer->name_party[index] != party)
			continue;
		if (offer_name_matches(name, offer->names[index]))
			return 1;
	}
	return 0;
}

int matchmaking_listed_team(const wchar_t *name, int host_local, int *listed)
{
	struct matchmaking_offer offer;
	char host_name[12];
	int index;

	if (listed)
		*listed = 0;
	if (!matchmaking_game_port())
		return -1;
	pthread_mutex_lock(&matchmaking_lock);
	offer = matchmaking.offer;
	memcpy(host_name, matchmaking.name, sizeof(host_name));
	pthread_mutex_unlock(&matchmaking_lock);
	host_name[sizeof(host_name) - 1] = 0;
	if (!offer.teams)
		return -1;
	/* no parties listed: the people in this hosted game share a team */
	if (offer.party_count <= 0)
	{
		if (listed)
			*listed = 1;
		return 0;
	}
	/* the host's own player is team 0, even when a remote human has the same name */
	if (host_local)
	{
		if (listed)
			*listed = 1;
		return 0;
	}
	/* a filler name is unique and belongs to the party that lists it */
	if (matchmaking_name_is_filler(name))
	{
		for (index = 0; index < offer.name_count && index < MATCHMAKING_LOBBY_NAMES; index++)
		{
			if (!offer_name_matches(name, offer.names[index]))
				continue;
			if (listed)
				*listed = 1;
			return party_team(&offer, offer.name_party[index]);
		}
	}
	/* both humans can publish the same name. the remote one is the other
	team only when parties[1] lists that name too. the first string match
	would put every copy, and the filler beside the first, on team 0. */
	if (host_name[0] && offer_name_matches(name, host_name) &&
		name_listed_in_party(&offer, name, 1))
	{
		if (listed)
			*listed = 1;
		return 1;
	}
	for (index = 0; index < offer.name_count && index < MATCHMAKING_LOBBY_NAMES; index++)
	{
		if (!offer_name_matches(name, offer.names[index]))
			continue;
		if (listed)
			*listed = 1;
		return party_team(&offer, offer.name_party[index]);
	}
	/* not listed: this human joined the link and stays on the host's team. */
	return 0;
}

int matchmaking_join_team(const wchar_t *name, int host_local)
{
	return matchmaking_listed_team(name, host_local, NULL);
}

int matchmaking_team_for_machine(int machine)
{
	struct matchmaking_offer offer;
	unsigned char key[32];
	int party;

	if (machine < 0 || !peer_mesh_machine_key(machine, key))
		return -1;
	party = peer_mesh_party_for_key(key);
	if (party < 0 || !matchmaking_game_port())
		return -1;
	pthread_mutex_lock(&matchmaking_lock);
	offer = matchmaking.offer;
	pthread_mutex_unlock(&matchmaking_lock);
	if (!offer.teams)
		return -1;
	return party_team(&offer, party);
}

int matchmaking_may_count(int player_count, int maximum_players)
{
	struct matchmaking_offer offer;

	if (!matchmaking_game_port() || !matchmaking_offer_copy(&offer))
		return 0;
	/* role wait keeps this lobby. server and join leave it for the other invite. */
	if (offer.role != MATCHMAKING_ROLE_HOST)
		return 0;
	/* the private lobby omits wait_seconds. it does not start. */
	if (offer.phase == MATCHMAKING_PHASE_PARTY || !matchmaking_is_ready())
		return 0;
	/* a rematch stays stopped while fewer people are seated than finished it,
	even if the offer still says the lobby is ready. */
	{
		int rematch;

		pthread_mutex_lock(&matchmaking_lock);
		rematch = matchmaking.rematch_players;
		pthread_mutex_unlock(&matchmaking_lock);
		if (rematch > 1 && player_count < rematch)
			return 0;
	}
	if (offer.wait_seconds >= 0)
	{
		/* 2v2 and Ranked start when four are actually seated. Free for all
		starts once two are seated. */
		if (!strcmp(offer.playlist, "ranked") || !strcmp(offer.playlist, "hardcore"))
			return offer.players > 0 && player_count >= offer.players;
		return player_count >= 2;
	}
	/* Find Game, and the seats already fill the cap. an omitted wait starts then. */
	return maximum_players > 0 && player_count >= maximum_players &&
		offer.phase == MATCHMAKING_PHASE_SEARCH;
}

int matchmaking_listed_absent(const wchar_t *const *names, int count)
{
	struct matchmaking_offer offer;
	unsigned char used[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	int index;

	if (!names || count < 0)
		count = 0;
	if (count > HALO_PORT_MAXIMUM_NETWORK_PLAYERS)
		count = HALO_PORT_MAXIMUM_NETWORK_PLAYERS;
	if (!matchmaking_offer_copy(&offer) || offer.party_count <= 0)
		return 0;
	memset(used, 0, (size_t)count);
	for (index = 0; index < offer.name_count && index < MATCHMAKING_LOBBY_NAMES; index++)
	{
		int player;
		int found = 0;

		if (offer.name_party[index] < 0 || !offer.names[index][0])
			continue;
		for (player = 0; player < count; player++)
		{
			if (used[player] || !names[player])
				continue;
			if (!offer_name_matches(names[player], offer.names[index]))
				continue;
			used[player] = 1;
			found = 1;
			break;
		}
		if (!found)
			return 1;
	}
	return 0;
}

void matchmaking_backstop_observe(int listed_absent)
{
	unsigned long now = GetTickCount();

	pthread_mutex_lock(&matchmaking_lock);
	if (matchmaking.offer.wait_seconds != 0 || matchmaking.offer.phase == MATCHMAKING_PHASE_PARTY)
	{
		backstop_clear_locked();
		pthread_mutex_unlock(&matchmaking_lock);
		return;
	}
	if (!matchmaking.backstop_at)
		matchmaking.backstop_at = now ? now : 1;
	matchmaking.backstop_absent = listed_absent ? 1 : 0;
	if (listed_absent)
		matchmaking.backstop_seated = 0;
	else if (!matchmaking.backstop_seated)
	{
		matchmaking.backstop_seated = 1;
		matchmaking.backstop_hold = 1;
	}
	else
		matchmaking.backstop_hold = 0;
	pthread_mutex_unlock(&matchmaking_lock);
}

int matchmaking_wait_omitted(void)
{
	struct matchmaking_offer offer;

	if (!matchmaking_offer_copy(&offer))
		return 0;
	return offer.wait_seconds < 0 && offer.phase == MATCHMAKING_PHASE_SEARCH;
}

int matchmaking_seats_unfilled(int present)
{
	struct matchmaking_offer offer;

	if (!matchmaking_offer_copy(&offer))
		return 0;
	if (!mesh_seats_unfilled(offer.joined, offer.players, present))
		return 0;
	/* Free for all starts with the people who reached the lobby once two are here.
	2v2 and Ranked keep waiting until the advertised seats are actually filled. */
	if (!strcmp(offer.playlist, "ffa") && present >= 2)
		return 0;
	return 1;
}

unsigned long matchmaking_countdown_ms(void)
{
	struct matchmaking_offer offer;
	unsigned long now;
	unsigned long at;
	int absent;
	int hold;
	int wait;

	pthread_mutex_lock(&matchmaking_lock);
	offer = matchmaking.offer;
	at = matchmaking.backstop_at;
	absent = matchmaking.backstop_absent;
	hold = matchmaking.backstop_hold;
	pthread_mutex_unlock(&matchmaking_lock);
	if (offer.wait_seconds < 0)
		return 0;
	wait = offer.wait_seconds;
	if (wait > 100)
		wait = 100;
	if (wait > 0)
		return (unsigned long)wait * 1000UL;
	/* wait_seconds 0 waits for every listed name. the tick that first sees
	them seated holds. the next tick starts. a name still missing 15 seconds
	after this 0 arrived starts then. */
	now = GetTickCount();
	if (!at || absent)
	{
		if (at && now - at >= 15000UL)
			return 0;
		return 15000UL;
	}
	if (hold)
		return 15000UL;
	return 0;
}
