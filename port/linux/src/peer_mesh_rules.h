/*
PEER_MESH_RULES.H

Who may be matched, who hosts, and when a report becomes a ban. No sockets.
*/

#ifndef __HALO_LINUX_PEER_MESH_RULES_H
#define __HALO_LINUX_PEER_MESH_RULES_H

/* seconds a banned key waits before it can be matched again.
the first ban waits 1 hour. each further ban doubles that, up to 7 days. */
int peer_cooldown_seconds(int bans);

/* 1 while now is still before until. */
int peer_in_cooldown(unsigned until, unsigned now);

/* a cooled-down key is left out while two clean keys are already in the
search, and for the first 6 seconds otherwise. a key still in cooldown
is left out. a clean key is kept. */
int peer_include_in_search(int bans, unsigned until, unsigned now, int clean_count, int waited_ms);

/* 1 when a should host rather than b. a lower median round trip hosts.
a key that answered nobody is behind one that did. each ban adds ten
seconds, so a banned key hosts only when nobody else can. a tie is the
lower public key. */
int peer_hosts_over(unsigned short median_a, int answered_a, int bans_a, const unsigned char *key_a,
	unsigned short median_b, int answered_b, int bans_b, const unsigned char *key_b);

/* more than half the players, and at least two, the same count that removes
the player from the game. one player cannot ban another. */
int peer_report_accepted(int votes, int players);

/* short votes that ban a key once this many are stored. */
int peer_strike_limit(void);

/* 1 when that many short votes ban the key. */
int peer_strikes_ban(int strikes);

/* the ban count after those votes. below the middle step, the count becomes
that step (16 hours). at that step or further, it takes the next step. */
int peer_strike_ban_count(int bans);

#endif
