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
