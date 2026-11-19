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

/* answers this client will believe. a timed peer reached this client.
the count in its score is not a connection to anyone else. */
int peer_score_answers(int samples);

/* ranked steps taken from how long this search has been hearing that
client. a search age the client wrote is not a step. */
int peer_ranked_steps(int observed_seconds);

/* 1 when this offer is the game to join. an offer while the search is
still gathering is early. once a host has been chosen, only that host's
offer joins. a search that already saw another client waits for the
result. a search that saw nobody joins when it would leave its own lobby. */
int peer_offer_accepted(int gathering, int host_chosen, int from_chosen, int seen_others, int yields);

/* 1 when a peer this client has not played, and who is not already on the
saved peer list, can be answered. age_ms is how long ago they broadcast that
they are looking. A broadcast older than one minute is stale. */
int peer_search_new(int played, int age_ms);

/* 1 when a known peer's looking broadcast is still fresh. Two minutes. */
int peer_search_known(int age_ms);

/* Shuffles order[0..count) into a random permutation. random is entropy from
the operating system, 4 bytes at a time, with extra bytes for a rejected
draw. 0 when there is not enough entropy to finish. count 0 or 1 is already
done. */
int peer_search_shuffle(int *order, int count, const unsigned char *random, int random_len);

/* 1 when this signed reply fills a seat. The role has to be the echo, this
client has to have sent that peer a nonce, and the echo has to be that nonce.
A blank nonce is not an echo. Any other role is their looking reply, or a
packet this client did not send. */
int peer_answer_admits(int role, int sent, int nonce_match, int nonce_blank);

/* 1 when this search has someone besides the client making an offer. A reply
that has not come back yet counts. So does a peer already in the lobby, and a
search that already chose among a group. */
int peer_search_seen(int other_candidates, int invites_waiting, int grouped);

/* invites that still count as other players. a host who has seated nobody is
still alone: the answers it sent are never echoed, so they do not keep it
from joining the lower key. during the measurement those answers still count. */
int peer_offer_invites(int offering, int seated, int invites_waiting);

/* 1 when a host whose game still has only its own players takes this offer.
A lower key is taken at once. After the lobby has waited, a higher key is
taken too, so a player listed on the card joins the lobby the others are in.
A game that already holds another player stays. */
int peer_host_joins(int remote_seated, int their_key_lower, int waited);

/* 1 when this offer becomes the game. A player already sitting in the host's
game keeps that host, including when the search would otherwise yield. */
int peer_offer_taken(int accepted, int echoed, int alone, int remote_seated);

/* 1 when another machine is already in this host's game. The player count
covers someone the client list has, and the machine count covers a join
that has connected and is not in that list yet. Split screen on one
machine stays empty. */
int peer_lobby_occupied(int humans, int locals, int machines);

/* 1 when a looking broadcast can fill an open seat. A new peer is inside one
minute. A known peer is inside two minutes. No room means no answer. */
int peer_search_fill(int is_new, int age_ms, int room);

/* 1 when this client echoes their nonce, so their lobby can include it. A
client that is already hosting does not. An invite already sent to them is
echoed. A search with nobody else echoes too. A search that already has
other players does not echo a stranger. */
int peer_lobby_echo(int hosting, int invited, int others);

/* 1 when their offer is the lobby this client agreed to enter. It answered
their invite, they are not already in this lobby, and this search has nobody
else. Gathering, a chosen host, and a reply still on the way stay as they are. */
int peer_offer_from_echo(int echoed, int in_our_lobby, int other_candidates, int other_invites,
	int hearing, int gathering, int host_chosen);

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

/* 1 when this directory entry can be forgotten.
   An unanswered invite stays. A ban that is still running stays. A ban
   record stays until a new key needs the slot. An address that was never
   heard leaves after unheard_limit seconds. A heard address leaves once
   the slot has been held for stale_limit seconds and no packet has arrived
   for stale_limit seconds. The hold gives a peer loaded from disk one
   quiet period to answer before the slot is freed. */
int peer_list_expired(unsigned last_seen, unsigned noted, unsigned now, int invite,
	int ban_active, int ban_record, unsigned unheard_limit, unsigned stale_limit);

/* 1 when a full directory can give this entry's slot to a new key.
   named_only is a key carried in someone else's list. That key can take an
   address this client has not heard. A key this client heard can take any
   slot except an open invite, a ban that is still running, a ban record
   when the new key was only named by someone else, or a peer heard a
   moment ago. recent is 1 for that peer. */
int peer_list_replaceable(int heard, int invite, int ban_active, int ban_record, int named_only,
	int recent);

#endif
