/*
PEER_MESH_RULES.C

The matchmaking decisions that do not need a socket, so a test can run them
on their own (tools/test_peer_mesh.c).
*/

#include "peer_mesh_rules.h"

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
