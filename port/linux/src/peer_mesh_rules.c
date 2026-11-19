/*
PEER_MESH_RULES.C

The matchmaking decisions that do not need a socket, so a test can run them
on their own (tools/test_peer_mesh.c).
*/

#include "peer_mesh_rules.h"
#include "mesh_service.h"

#include <string.h>

enum
{
	COOLDOWN_BASE_SECONDS = 60 * 60,
	COOLDOWN_CAP_SECONDS = 7 * 24 * 60 * 60,
	BANNED_WAIT_MS = 6000,
	BAN_HOST_PENALTY_MS = 10000,
	UNANSWERED_PENALTY_MS = 50000,
	/* thirty three votes that never reached a match majority */
	PEER_STRIKE_LIMIT = 33,
	/* the middle of the nine ban steps: 16 hours */
	PEER_STRIKE_STEP = 5
};

int peer_cooldown_seconds(int bans)
{
	int shift;
	int seconds;

	if (bans < 1)
		return 0;
	shift = bans - 1;
	if (shift > 10)
		shift = 10;
	seconds = COOLDOWN_BASE_SECONDS << shift;
	if (seconds < COOLDOWN_BASE_SECONDS || seconds > COOLDOWN_CAP_SECONDS)
		return COOLDOWN_CAP_SECONDS;
	return seconds;
}

int peer_in_cooldown(unsigned until, unsigned now)
{
	return until > now;
}

int peer_include_in_search(int bans, unsigned until, unsigned now, int clean_count, int waited_ms)
{
	if (peer_in_cooldown(until, now))
		return 0;
	if (bans <= 0)
		return 1;
	return waited_ms >= BANNED_WAIT_MS && clean_count < 2;
}

int peer_hosts_over(unsigned short median_a, int answered_a, int bans_a, const unsigned char *key_a,
	unsigned short median_b, int answered_b, int bans_b, const unsigned char *key_b)
{
	int score_a = (int)median_a + (bans_a > 0 ? bans_a : 0) * BAN_HOST_PENALTY_MS;
	int score_b = (int)median_b + (bans_b > 0 ? bans_b : 0) * BAN_HOST_PENALTY_MS;

	if (answered_a <= 0)
		score_a += UNANSWERED_PENALTY_MS;
	if (answered_b <= 0)
		score_b += UNANSWERED_PENALTY_MS;
	if (score_a != score_b)
		return score_a < score_b;
	if (answered_a != answered_b)
		return answered_a > answered_b;
	if (!key_a || !key_b)
		return key_a != NULL;
	return memcmp(key_a, key_b, 32) < 0;
}

int peer_score_answers(int samples)
{
	return samples > 0 ? 1 : 0;
}

int peer_ranked_steps(int observed_seconds)
{
	if (observed_seconds < 0)
		observed_seconds = 0;
	return mesh_rank_steps(observed_seconds);
}

int peer_offer_accepted(int gathering, int host_chosen, int from_chosen, int seen_others, int yields)
{
	if (gathering)
		return 0;
	if (host_chosen)
		return from_chosen ? 1 : 0;
	if (seen_others)
		return 0;
	return yields ? 1 : 0;
}

int peer_search_new(int played, int age_ms)
{
	if (played)
		return 0;
	if (age_ms < 0)
		return 0;
	return age_ms <= 60000;
}

int peer_search_known(int age_ms)
{
	if (age_ms < 0)
		return 0;
	return age_ms <= 120000;
}

int peer_search_shuffle(int *order, int count, const unsigned char *random, int random_len)
{
	int index;
	int used = 0;

	if (!order || count < 0 || count > 64)
		return 0;
	for (index = 0; index < count; index++)
		order[index] = index;
	if (count < 2)
		return 1;
	if (!random || random_len < 4)
		return 0;
	for (index = count - 1; index > 0; index--)
	{
		unsigned span = (unsigned)index + 1;
		unsigned limit = 0xffffffffu - (0xffffffffu % span);
		unsigned pick = 0;
		int found = 0;

		while (used + 4 <= random_len)
		{
			unsigned draw = (unsigned)random[used] |
				((unsigned)random[used + 1] << 8) |
				((unsigned)random[used + 2] << 16) |
				((unsigned)random[used + 3] << 24);

			used += 4;
			if (draw < limit)
			{
				pick = draw % span;
				found = 1;
				break;
			}
		}
		if (!found)
			return 0;
		if ((int)pick != index)
		{
			int swap = order[index];

			order[index] = order[pick];
			order[pick] = swap;
		}
	}
	return 1;
}

int peer_answer_admits(int role, int sent, int nonce_match, int nonce_blank)
{
	if (role != 1 || !sent || !nonce_match || nonce_blank)
		return 0;
	return 1;
}

int peer_search_seen(int other_candidates, int invites_waiting, int grouped)
{
	if (other_candidates > 0 || invites_waiting > 0 || grouped)
		return 1;
	return 0;
}

int peer_offer_invites(int offering, int seated, int invites_waiting)
{
	if (offering && seated <= 0)
		return 0;
	return invites_waiting;
}

int peer_host_joins(int remote_seated, int their_key_lower, int waited)
{
	if (remote_seated)
		return 0;
	if (their_key_lower)
		return 1;
	return waited ? 1 : 0;
}

int peer_offer_taken(int accepted, int echoed, int alone, int remote_seated)
{
	if (remote_seated)
		return 0;
	if (accepted || echoed || alone)
		return 1;
	return 0;
}

int peer_lobby_occupied(int humans, int locals, int machines)
{
	if (machines > 1)
		return 1;
	if (locals < 1)
		locals = 1;
	return humans > locals;
}

int peer_search_fill(int is_new, int age_ms, int room)
{
	if (room <= 0)
		return 0;
	if (is_new)
		return peer_search_new(0, age_ms);
	return peer_search_known(age_ms);
}

int peer_lobby_echo(int hosting, int invited, int others)
{
	if (hosting)
		return 0;
	if (invited)
		return 1;
	return others ? 0 : 1;
}

int peer_offer_from_echo(int echoed, int in_our_lobby, int other_candidates, int other_invites,
	int hearing, int gathering, int host_chosen)
{
	if (!echoed || in_our_lobby || other_candidates > 0 || other_invites > 0 || hearing || gathering || host_chosen)
		return 0;
	return 1;
}

int peer_report_accepted(int votes, int players)
{
	return players > 0 && votes >= 2 && votes * 2 > players;
}

int peer_strike_limit(void)
{
	return PEER_STRIKE_LIMIT;
}

int peer_strikes_ban(int strikes)
{
	return strikes >= PEER_STRIKE_LIMIT;
}

int peer_strike_ban_count(int bans)
{
	if (bans < PEER_STRIKE_STEP)
		return PEER_STRIKE_STEP;
	if (bans >= 1000)
		return 1000;
	return bans + 1;
}

int peer_list_expired(unsigned last_seen, unsigned noted, unsigned now, int invite,
	int ban_active, int ban_record, unsigned unheard_limit, unsigned stale_limit)
{
	if (invite || ban_active || ban_record)
		return 0;
	if (last_seen)
	{
		if (now < last_seen || now - last_seen <= stale_limit)
			return 0;
		if (noted && now >= noted && now - noted <= stale_limit)
			return 0;
		return 1;
	}
	if (!noted || now < noted || !unheard_limit)
		return 0;
	return now - noted > unheard_limit;
}

int peer_list_replaceable(int heard, int invite, int ban_active, int ban_record, int named_only,
	int recent)
{
	if (invite || ban_active || recent)
		return 0;
	if (named_only && (heard || ban_record))
		return 0;
	return 1;
}
