/*
RANK_ACCOUNT.H

The profile's ranking account (port/linux/src/rank_account.c). The server
assigns the account, the token, the Core ID, the games played, and the
level. The client keeps the emblem the player picked, and it draws the
badge and the level color from those numbers.
*/

#ifndef __HALO_LINUX_RANK_ACCOUNT_H
#define __HALO_LINUX_RANK_ACCOUNT_H

struct rank_account
{
	char account[48];
	char token[80];
	char core[24];
	char name[12];
	int foreground;
	int background;
	int foreground_color;
	int background_color;
	int games;
	int level;
	/* ranked streaks. both clear when the level changes. */
	int wins;
	int losses;
	/* 1 when this profile has a token */
	int ready;
	/* 1 after the player has picked an emblem */
	int chosen;
};

/* directory is the profile folder. name is that profile's gamertag. */
void rank_account_bind(const char *directory, const char *name);
int rank_account_bound(void);
void rank_account_get(struct rank_account *out);
/* keeps this install's Mesh ID on the bound profile. The same ID is left as it is. */
void rank_account_remember_mesh(const char *mesh_id);
/* field 0 foreground, 1 background, 2 sprite color, 3 shape color */
void rank_emblem_step(int field, int delta);
void rank_emblem_set(int field, int value);
void rank_account_commit(void);
/* puts the emblem back. account, token, Core ID, games, and level stay. */
void rank_account_restore(const struct rank_account *saved);

int rank_foreground_count(void);
int rank_background_count(void);
int rank_color_count(void);
const char *rank_foreground_name(int index);
const char *rank_background_name(int index);
const char *rank_badge_name(int games);
/* 0 Recruit through 7 Ascendant. The pictures on the card, not a choice. */
int rank_badge_index(int games);
/* 0xRRGGBB. 0 to 9 green, then yellow, blue, pink, purple, and red from 50. */
unsigned rank_level_rgba(int level);

void rank_party_begin(void);
void rank_party_add(const char *directory, const char *name);
int rank_party_ready(void);
void rank_search_set(int ranked);
/* 1 when Find Game should wait for every local profile's account */
int rank_search_blocks(void);
void rank_search_clear(void);

/* writes a leading comma and the account fields, or leaves text empty.
1 when it wrote any. */
int rank_queue_extra(char *text, int size);

/* asks for an account for the next local profile that does not have one. */
void rank_register_kick(void);
int rank_register_pending(char *body, int size);
int rank_profile_pending(char *topic, int topic_size, char *body, int body_size, unsigned *generation);
void rank_profile_sent(unsigned generation);
/* register, profile, and result replies, and the retained public account */
void rank_apply_downlink(const char *topic, const char *payload, const char *client_id);
/* the next public account or profile reply this client should subscribe to */
int rank_subscribe_next(char *topic, int size);
void rank_subscribe_done(const char *topic);
/* the host's next queue publish lists this account once */
void rank_kick_add(const char *account);
void rank_kick_published(void);

/* 1 when this match id is new and the caller should note its cards. */
int rank_match_begin(const char *match, const char *playlist, int teams);
/* the game now starting is not the open mesh match. an unsent scoreboard is dropped. */
void rank_match_close(void);
void rank_match_note(const char *name, const char *account, int level);
int rank_level_named(const char *name);
void rank_account_for_name(const char *name, char *account, int size);
int rank_match_card_count(void);
int rank_match_card_at(int index, char *name, int name_size, char *account, int account_size);

int rank_result_open(void);
int rank_result_context(char *match, int match_size, char *playlist, int playlist_size,
	char *token, int token_size, int *teams);
void rank_result_submit(const char *body);
int rank_result_pending(char *body, int body_size);
void rank_result_sent(void);
/* the mesh applied a result to this profile. */
void rank_mesh_progress(int games, int level, int wins, int losses);

void rank_json_escape(char *out, int size, const char *text);

#endif
