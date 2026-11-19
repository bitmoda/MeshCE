/*
MESH_SERVICE.H

The matchmaking rules the broker used to apply. Each client applies the
same rules. A Mesh ID is derived from the install's key, so it does not
change and no server assigns it.
*/

#ifndef __HALO_LINUX_MESH_SERVICE_H
#define __HALO_LINUX_MESH_SERVICE_H

/* 1 when the name can be a gamertag. At most 11 characters, no leading or
trailing space, no "|", at least one letter or digit. Printable ASCII and
Latin letters U+00C0 through U+017F. */
int mesh_gamertag_ok(const char *name);

/* account and token are hex halves of the key. mesh_id is M and eight hex
digits. The same key always produces the same three. */
void mesh_identity(const unsigned char key[32], char *account, int account_size,
	char *token, int token_size, char *mesh_id, int mesh_size);

/* writes the register reply for this gamertag JSON and key. 0 when the key
is missing. An unacceptable name gets {"ok":false,"error":"gamertag"}. */
int mesh_register_reply(const char *request, const unsigned char key[32], char *reply, int size);

/* steps of a ranked window. 0 is the opening window. 1 is the expansion at
5 minutes. Later steps are the drops after that. */
void mesh_rank_window(int level, int steps, int *floor_out, int *ceiling_out);

/* how many expansions the clock has reached. elapsed_seconds is how long
this party has been searching. */
int mesh_rank_steps(int elapsed_seconds);

/* 1 when the higher party's window covers the lower party's level. */
int mesh_ranked_together(int level_a, int steps_a, int level_b, int steps_b);

/* 1 when a lobby that is still alone should leave for this offer.
their_key_lower is 1 when the other client's key sorts first.
A higher ranked lobby stays. The same level yields to the lower key. */
int mesh_solo_yields(int ranked, int our_level, int their_level, int their_key_lower);

/* outcome 1 win, 0 loss, -1 a tie or a result that changes no streak.
the new level and streaks are written to the out values. */
void mesh_level_apply(int level, int wins, int losses, int outcome,
	int *level_out, int *wins_out, int *losses_out);

/* social playlists add one game when the player did not quit. ranked does not. */
int mesh_games_apply(const char *playlist, int games, int quit);

/* -1 while the search stays open. 30 when the lobby is ready and the
host should count down that many seconds. free for all is ready once two
people are seated, and that lobby holds sixteen. 2v2 and ranked are ready
when all four seats are filled. */
int mesh_wait_seconds(const char *playlist, int humans, int cap, int elapsed_seconds);

/* 1 when advertised seats already fill the match and that many players are
not in the lobby yet. a shorter search is not this wait. */
int mesh_seats_unfilled(int advertised, int cap, int present);

/* the map this match id uses. the same id is the same map on every client.
free for all draws from every stock multiplayer map. the other playlists
draw from the five smaller ones. */
const char *mesh_map_for(const unsigned char match[16], const char *playlist);

/* slayer score: 25 on 2v2 and ranked, 50 on free for all. */
int mesh_score(const char *playlist);

/* 1 when this search rolls pistol and sniper. otherwise assault rifle and pistol.
primary and secondary are the game's weapon numbers. */
void mesh_loadout(const unsigned char match[16], const char *playlist, int *primary, int *secondary);

/* 1 when account appears in the result, or when name is the only row with
that gamertag and the row has no different account. games, level, wins, and
losses are updated from their current values. a result with anyone gone
leaves those values as they are. */
int mesh_result_apply(const char *body, const char *account, const char *name,
	int *games, int *level, int *wins, int *losses);

/* once a second, gives this install its Mesh ID and applies a pending result. */
void mesh_account_serve(void);

/* one scoreboard row. team is -1 when the match is not a team game. */
struct mesh_tally_row
{
	char name[12];
	int team;
	int score;
	int place;
	int quit;
};

/* the scoreboard every client can calculate. keys are the installs that
played, and key_names is the gamertag of each install in that same order.
the same rows and the same keys in any order produce the same text.
0 when the text does not fit. */
int mesh_tally_text(const char *match_id, const char *playlist,
	const unsigned char keys[][32], const char key_names[][12], int key_count,
	const struct mesh_tally_row *rows, int count, char *out, int size);

/* humans named by a tally, or 0 when the text is not a tally. */
int mesh_tally_humans(const char *text);

/* installs listed on a tally. 0 when the list does not match the header. */
int mesh_tally_key_count(const char *text);

/* 1 when this install is one of the tally's keys. */
int mesh_tally_has_key(const char *text, const unsigned char key[32]);

/* copies one listed install. 0 when index is past the list. */
int mesh_tally_key_at(const char *text, int index, unsigned char key[32]);

/* the gamertag written on that install's line. 0 when the key is not listed. */
int mesh_tally_key_name(const char *text, const unsigned char key[32], char name[12]);

/* 1 when that install's one scoreboard row left at any time. Two rows
with the name, or two installs with the name, are left alone. */
int mesh_tally_key_left(const char *text, const unsigned char key[32]);

/* 1 when that install's one scoreboard row is a first leave. The quit
field is the order they left: the smallest number above 0 is first, and
0 means they stayed. A later leave is not a vote. Two rows with the name,
or two installs with the name, are left alone. */
int mesh_tally_key_quit(const char *text, const unsigned char key[32]);

/* 1 when this many matching tallies is more than half of the match. */
int mesh_tally_majority(int votes, int humans);

/* one install's record on the mesh. the key is the Ed25519 id. */
struct mesh_standing
{
	unsigned char key[32];
	char name[12];
	int games;
	int level;
	int wins;
	int losses;
	int used;
	/* The numbers before this match was applied, so a larger tally can take that apply back. */
	int undo_have;
	int undo_was_new;
	char undo_match[40];
	int undo_games;
	int undo_level;
	int undo_wins;
	int undo_losses;
};

/* a client in the match, and the scoreboard name it claims. */
struct mesh_claim
{
	unsigned char key[32];
	char name[12];
};

/* applies an agreed tally to the book. the gamertag is the one on that
install's line. a name outside the tally is not used. the same gamertag on
two install lines is left alone. 1 when the tally parsed. */
int mesh_standings_commit(struct mesh_standing *book, int capacity, const char *tally,
	const struct mesh_claim *claims, int claim_count);

#endif
