/*
PEER_MESH.H

Every client keeps a list of other clients and trades that list with them
(port/linux/src/peer_mesh.c). Find Game uses the list. The client with the
best round trip hosts. A claimed round trip cannot be faster than the ping
this client measured. The reputation id is sealed to the machine's secure
element when it has one. A report bans a key when the voters signed the tally
the mesh kept and those votes are more than half of the installs that tally
names. That is the same count that removes the player from the game. A vote
that falls short of that count is kept. Thirty three of those votes ban the
key, starting at the middle of the cooldown.
*/

#ifndef __HALO_LINUX_PEER_MESH_H
#define __HALO_LINUX_PEER_MESH_H

/* One match's scoreboard text. Sixteen full names fit. */
#define PEER_MESH_TALLY_TEXT 2048

void peer_mesh_start(void);
void peer_mesh_search(const char *playlist, int seats);
void peer_mesh_stop(void);
/* a new game drops the previous game's reports and machine ids */
void peer_mesh_clear_game(void);
int peer_mesh_ready(void);
int peer_mesh_searching(void);
/* this search's players, including this machine. 1 when no search is open. */
int peer_mesh_party_size(void);
/* names of the other parties in this search. returns how many were copied. */
int peer_mesh_party_names(char names[][12], int max);
/* 1 when this player is known. mesh_id is that install. level is that score.
machine -1 uses the name. a machine uses its peer id when that has arrived,
else one install that announced this name. a numbered name matches the name
before the number when only one install announced it. this install's name
is used only for a row that is not a machine. address is that game
connection in network order, or 0. one install heard from that address is
used when the name is not the one it announced. */
int peer_mesh_player(const char *name, int machine, char *mesh_id, int mesh_id_size, int *level,
	unsigned long address);
/* 0 is this machine. 1 and up follow party_names. -1 when the key is not in this search. */
int peer_mesh_party_for_key(const unsigned char key[32]);
/* seconds since Find Game. */
int peer_mesh_search_seconds(void);
/* Send one match result to the other players in the mesh. */
void peer_mesh_publish_result(const char *body);
/* Copy a result another player published. Returns 1 when one is waiting. */
int peer_mesh_take_result(char *body, int size);
/* One client's scoreboard for a match. The mesh keeps it with the others. */
int peer_mesh_submit_tally(const char *match_id, const char *name, const char *tally);
/* Installs that have sent a peer id for the current match, including this one. */
int peer_mesh_roster(unsigned char keys[][32], int max);

int peer_mesh_public(unsigned char key[32]);
int peer_mesh_sign(const void *message, int size, unsigned char signature[64]);
int peer_mesh_verify(const unsigned char key[32], const void *message, int size,
	const unsigned char signature[64]);
int peer_mesh_match(unsigned char match[16]);
/* 1 when text is the hex id of the mesh match this client is in. */
int peer_mesh_match_is(const char *text);
/* this client is no longer in a mesh match, so a scoreboard cannot change rank. */
void peer_mesh_drop_match(void);
/* the mesh match has started. keep sending its invite briefly, so an address
learned after the search stops still reaches the other players. */
void peer_mesh_arm_late_offer(void);
/* 1 while this host's lobby has no seat left. offers stop until a seat opens. */
void peer_mesh_set_lobby_full(int full);
/* humans sitting in this host's game, including this machine. */
void peer_mesh_set_game_humans(int humans);
/* machines connected to this host's game, including this one. */
void peer_mesh_set_game_machines(int machines);
/* the match is over and this host is back in the lobby. id is the new match.
the players already seated stay. offers go out while a seat is open. this
does not start the search over. */
void peer_mesh_set_rematch(const unsigned char id[16]);
/* A join names the other client. Remember that client and try this mesh
   port, or 38952 when the port is 0. A search is not started. The saved
   list keeps the client after a hello arrives. */
void peer_mesh_introduce(const unsigned char key[32], unsigned long address, unsigned short port);
/* Say hello on this mesh port, or 38952 when the port is 0. A search is
   not started. */
void peer_mesh_greet(unsigned long address, unsigned short port);
/* 1 when a STUN answer has named this machine's public mesh address.
   Both values are network byte order. */
int peer_mesh_mapped(unsigned long *address, unsigned short *port);

/* machine -1 is the host of this game. */
void peer_mesh_note_machine(int machine, const unsigned char key[32]);
int peer_mesh_machine_key(int machine, unsigned char key[32]);

void peer_mesh_add_report(const unsigned char voter[32], const unsigned char target[32],
	const unsigned char signature[64]);
/* The voter sends this vote on the mesh as well as to the host. */
void peer_mesh_cast_vote(const unsigned char target[32], const unsigned char host[32],
	const unsigned char signature[64]);
int peer_mesh_report_count(const unsigned char target[32]);
void peer_mesh_publish_ban(const unsigned char target[32], int votes, int players);
/* votes from a kick that stayed short of the count that removes the player */
void peer_mesh_publish_strikes(const unsigned char target[32], int players);

#endif
