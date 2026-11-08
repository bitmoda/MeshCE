/* The matchmaking decisions in peer_mesh_rules.c and mesh_service.c. */
#include "../port/linux/src/peer_mesh_rules.c"
#include "../port/linux/src/mesh_service.c"

#include <stdio.h>
#include <string.h>

static int failed;

static void check(int condition, const char *text)
{
	if (condition)
		return;
	fprintf(stderr, "failed: %s\n", text);
	failed = 1;
}

int main(void)
{
	unsigned char low[32];
	unsigned char high[32];

	memset(low, 1, sizeof(low));
	memset(high, 2, sizeof(high));
	check(peer_cooldown_seconds(0) == 0, "no bans waits");
	check(peer_cooldown_seconds(1) == 60 * 60, "first ban is 1 hour");
	check(peer_cooldown_seconds(2) == 2 * 60 * 60, "second ban doubles");
	check(peer_cooldown_seconds(3) == 4 * 60 * 60, "third ban doubles again");
	check(peer_cooldown_seconds(8) == 5 * 24 * 60 * 60 + 8 * 60 * 60, "eighth ban is 5 days 8 hours");
	check(peer_cooldown_seconds(9) == 7 * 24 * 60 * 60, "ninth ban reaches 7 days");
	check(peer_cooldown_seconds(30) == 7 * 24 * 60 * 60, "cooldown stays within 7 days");
	check(peer_in_cooldown(100, 50) == 1, "still waiting");
	check(peer_in_cooldown(100, 100) == 0, "the wait has ended");
	check(peer_include_in_search(0, 0, 1000, 4, 0) == 1, "a clean key is matched");
	check(peer_include_in_search(2, 5000, 1000, 0, 10000) == 0, "a key in cooldown is not matched");
	check(peer_include_in_search(2, 1000, 1000, 2, 10000) == 0, "a banned key waits while the search is full");
	check(peer_include_in_search(2, 1000, 1000, 1, 1000) == 0, "a banned key waits out the first seconds");
	check(peer_include_in_search(2, 1000, 1000, 1, 6000) == 1, "a banned key can fill a short search");
	check(peer_hosts_over(20, 2, 0, low, 40, 2, 0, high) == 1, "the lower round trip hosts");
	check(peer_hosts_over(40, 2, 0, high, 20, 2, 0, low) == 0, "the higher round trip does not host");
	check(peer_hosts_over(20, 0, 0, low, 40, 2, 0, high) == 0, "a client that reached nobody does not host");
	check(peer_hosts_over(10, 2, 3, low, 50, 2, 0, high) == 0, "bans keep a key from hosting");
	check(peer_hosts_over(20, 2, 0, low, 20, 2, 0, high) == 1, "a tie uses the lower key");
	check(peer_report_accepted(2, 4) == 0, "two of four is not a ban");
	check(peer_report_accepted(3, 4) == 1, "three of four is a ban");
	check(peer_report_accepted(1, 4) == 0, "one of four is not a ban");
	check(peer_report_accepted(2, 3) == 1, "two of three is a ban");
	check(peer_report_accepted(1, 2) == 0, "one player cannot ban the other");
	check(peer_report_accepted(2, 2) == 1, "both of two is a ban");
	check(peer_report_accepted(4, 8) == 0, "four of eight is not a ban");
	check(peer_report_accepted(5, 8) == 1, "five of eight is a ban");
	check(peer_strike_limit() == 33, "thirty three short votes ban");
	check(peer_strikes_ban(32) == 0, "thirty two short votes wait");
	check(peer_strikes_ban(33) == 1, "thirty three short votes ban");
	check(peer_strike_ban_count(0) == 5, "an accumulated ban starts at the middle step");
	check(peer_strike_ban_count(4) == 5, "a shorter record is raised to the middle step");
	check(peer_strike_ban_count(5) == 6, "the middle step takes the next step");
	check(peer_strike_ban_count(8) == 9, "a later accumulated ban still advances");
	check(peer_cooldown_seconds(peer_strike_ban_count(0)) == 16 * 60 * 60, "the middle step waits 16 hours");
	check(mesh_gamertag_ok("mastrcheese") == 1, "a short name is a gamertag");
	check(mesh_gamertag_ok("Jos\xC3\xA9") == 1, "a latin letter is a gamertag");
	check(mesh_gamertag_ok("1") == 0, "a gamertag needs a letter");
	check(mesh_gamertag_ok(" hello") == 0, "a gamertag has no leading space");
	check(mesh_gamertag_ok("a|b") == 0, "a gamertag has no bar");
	check(mesh_gamertag_ok("\xE4\xB8\x80") == 0, "one cjk character is refused");
	{
		unsigned char key[32];
		char account[48];
		char token[80];
		char mesh_id[24];
		char again[24];
		int floor;
		int ceiling;
		int level;
		int wins;
		int losses;
		int games = 0;
		const char *result =
			"{\"playlist\":\"ranked\",\"players\":["
			"{\"account\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"quit\":false,\"score\":10,\"place\":1},"
			"{\"account\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"quit\":false,\"score\":4,\"place\":2}]}";

		memset(key, 0x11, sizeof(key));
		mesh_identity(key, account, (int)sizeof(account), token, (int)sizeof(token), mesh_id, (int)sizeof(mesh_id));
		mesh_identity(key, account, (int)sizeof(account), token, (int)sizeof(token), again, (int)sizeof(again));
		check(mesh_id[0] == 'M' && strlen(mesh_id) == 9, "a mesh id is M and eight hex digits");
		check(!strcmp(mesh_id, again), "the same key keeps its mesh id");
		mesh_rank_window(23, 0, &floor, &ceiling);
		check(floor == 20 && ceiling == 23, "a level under 50 opens on its band");
		mesh_rank_window(50, 0, &floor, &ceiling);
		check(floor == 50 && ceiling == 50, "level 50 opens at 50");
		mesh_rank_window(50, 1, &floor, &ceiling);
		check(floor == 0 && ceiling == 50, "level 50 expands to 0");
		mesh_rank_window(149, 0, &floor, &ceiling);
		check(floor == 50 && ceiling == 149, "level 149 opens at 50");
		mesh_rank_window(150, 0, &floor, &ceiling);
		check(floor == 50 && ceiling == 150, "level 150 opens at 50");
		mesh_rank_window(400, 0, &floor, &ceiling);
		check(floor == 250 && ceiling == 400, "level 400 opens at 250");
		mesh_rank_window(500, 0, &floor, &ceiling);
		check(floor == 350 && ceiling == 500, "level 500 opens at 350");
		mesh_rank_window(3000, 0, &floor, &ceiling);
		check(floor == 2850 && ceiling == 3000, "level 3000 opens at 2850");
		mesh_rank_window(3000, 1, &floor, &ceiling);
		check(floor == 350 && ceiling == 3000, "a high floor drops to 350");
		check(mesh_solo_yields(1, 40, 80, 0) == 1, "a lower ranked lobby moves to a higher one");
		check(mesh_solo_yields(1, 80, 40, 1) == 0, "a higher ranked lobby stays");
		check(mesh_solo_yields(1, 40, 40, 1) == 1, "the same level yields to the lower key");
		check(mesh_solo_yields(1, 40, 40, 0) == 0, "the same level keeps the lower key");
		check(mesh_solo_yields(0, 1, 9, 0) == 0, "a social lobby yields only to the lower key");
		check(mesh_ranked_together(80, 0, 40, 0) == 0, "a higher window does not cover a distant level yet");
		check(mesh_ranked_together(23, 0, 23, 0) == 1, "the same level covers itself");
		mesh_level_apply(0, 1, 0, 1, &level, &wins, &losses);
		check(level == 1 && wins == 0 && losses == 0, "two wins under 50 rank up");
		mesh_level_apply(0, 0, 2, 0, &level, &wins, &losses);
		check(level == 0, "three losses stay at the bottom");
		mesh_level_apply(50, 0, 0, 0, &level, &wins, &losses);
		check(level == 50 && wins == 0, "a loss at 50 stays and clears the win streak");
		mesh_level_apply(50, 2, 0, 1, &level, &wins, &losses);
		check(level == 51 && wins == 0, "three wins at 50 rank up");
		mesh_level_apply(51, 0, 0, 0, &level, &wins, &losses);
		check(level == 50 && wins == 0 && losses == 0, "one loss above 50 drops one level");
		check(mesh_games_apply("ffa", 4, 0) == 5, "a social finish adds a game");
		check(mesh_games_apply("ffa", 4, 1) == 4, "a quit adds no game");
		check(mesh_games_apply("ranked", 4, 0) == 4, "ranked does not add a social game");
		check(mesh_wait_seconds("ffa", 3, 8, 10) == -1, "an open search does not count down");
		check(mesh_wait_seconds("ffa", 8, 8, 1) == 0, "a full lobby starts");
		check(mesh_wait_seconds("ranked", 7, 8, 200) == -1, "ranked waits for eight");
		check(mesh_wait_seconds("ffa", 2, 8, 100) == 0, "a social search starts after 100 seconds");
		check(!strcmp(mesh_map_for((const unsigned char *)"\x00"), "hangemhigh"), "the first map is hangemhigh");
		check(mesh_score("hardcore") == 25, "hardcore is to 25");
		check(mesh_score("ranked") == 50, "ranked is to 50");
		games = 0;
		level = 0;
		wins = 0;
		losses = 0;
		check(mesh_result_apply(result, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", NULL, &games, &level, &wins, &losses) == 1,
			"a ranked result names this account");
		check(games == 0 && level == 0, "one ranked win does not rank up from 0");
		check(wins == 1, "one ranked win starts the streak");
		{
			const char *tie =
				"{\"playlist\":\"ranked\",\"players\":["
				"{\"account\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"quit\":false,\"score\":10,\"place\":1},"
				"{\"account\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"quit\":false,\"score\":10,\"place\":1},"
				"{\"account\":\"cccccccccccccccccccccccccccccccc\",\"quit\":false,\"score\":4,\"place\":3}]}";
			const char *team =
				"{\"playlist\":\"ranked\",\"players\":["
				"{\"account\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"quit\":true,\"score\":10,\"team\":0},"
				"{\"account\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"quit\":false,\"score\":1,\"team\":0},"
				"{\"account\":\"cccccccccccccccccccccccccccccccc\",\"quit\":false,\"score\":0,\"team\":1}]}";

			wins = 2;
			losses = 1;
			check(mesh_result_apply(tie, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", NULL, &games, &level, &wins, &losses) == 1,
				"a tied result names this account");
			check(wins == 2 && losses == 1 && level == 0, "sharing first changes no streak");
			wins = 2;
			losses = 0;
			mesh_result_apply(tie, "cccccccccccccccccccccccccccccccc", NULL, &games, &level, &wins, &losses);
			check(wins == 0 && losses == 1, "the players behind a tie lose");
			wins = 4;
			losses = 0;
			level = 10;
			mesh_result_apply(team, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", NULL, &games, &level, &wins, &losses);
			check(level == 10 && wins == 0 && losses == 1, "a quit on the winning team is a loss");
			wins = 0;
			losses = 0;
			mesh_result_apply(team, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", NULL, &games, &level, &wins, &losses);
			check(wins == 1 && losses == 0, "a teammate who stayed gets the win");
			{
				const char *named =
					"{\"playlist\":\"ffa\",\"players\":["
					"{\"name\":\"Alpha\",\"quit\":false,\"score\":10,\"place\":1},"
					"{\"name\":\"Beta\",\"quit\":false,\"score\":1,\"place\":2}]}";
				const char *both =
					"{\"playlist\":\"ffa\",\"players\":["
					"{\"name\":\"Beta\",\"quit\":false,\"score\":10,\"place\":1},"
					"{\"name\":\"Beta\",\"quit\":false,\"score\":1,\"place\":2}]}";

				games = 3;
				check(mesh_result_apply(named, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "Beta", &games, &level,
					&wins, &losses) == 1, "a row with no account still names the gamertag");
				check(games == 4, "that social finish adds a game");
				games = 4;
				check(mesh_result_apply(both, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "Beta", &games, &level,
					&wins, &losses) == 0, "two rows with the same gamertag are left alone");
				check(games == 4, "an ambiguous gamertag adds no game");
			}
		}
	}
	{
		struct mesh_tally_row first[2];
		struct mesh_tally_row second[2];
		struct mesh_standing book[4];
		struct mesh_claim claims[2];
		unsigned char keys[2][32];
		unsigned char flipped_keys[2][32];
		char left[512];
		char right[512];
		unsigned char alpha[32];
		unsigned char beta[32];
		unsigned char gamma[32];

		memset(first, 0, sizeof(first));
		memset(second, 0, sizeof(second));
		memset(alpha, 3, sizeof(alpha));
		memset(beta, 4, sizeof(beta));
		memset(gamma, 5, sizeof(gamma));
		memcpy(keys[0], beta, 32);
		memcpy(keys[1], alpha, 32);
		memcpy(flipped_keys[0], alpha, 32);
		memcpy(flipped_keys[1], beta, 32);
		snprintf(first[0].name, 12, "Alpha");
		first[0].team = -1;
		first[0].score = 10;
		first[0].place = 1;
		snprintf(first[1].name, 12, "Beta");
		first[1].team = -1;
		first[1].score = 1;
		first[1].place = 2;
		second[0] = first[1];
		second[1] = first[0];
		check(mesh_tally_text("abc", "ranked", keys, 2, first, 2, left, (int)sizeof(left)) == 1,
			"tally text fits");
		check(mesh_tally_text("abc", "ranked", flipped_keys, 2, second, 2, right, (int)sizeof(right)) == 1,
			"reversed tally fits");
		check(strcmp(left, right) == 0, "tally text does not depend on row order");
		check(mesh_tally_humans(left) == 2, "tally names the roster");
		check(mesh_tally_key_count(left) == 2, "tally lists both installs");
		check(mesh_tally_has_key(left, alpha) == 1, "the tally lists the first install");
		check(mesh_tally_has_key(left, gamma) == 0, "the tally leaves out an install that did not play");
		check(strstr(left, "v1|abc|ranked|2|2\n") == left, "tally starts with the match");
		check(mesh_tally_majority(2, 2) == 1, "both of two agree");
		check(mesh_tally_majority(2, 3) == 1, "two of three agree");
		check(mesh_tally_majority(2, 4) == 0, "two of four do not agree");
		check(mesh_tally_majority(3, 4) == 1, "three of four agree");
		check(mesh_tally_majority(1, 2) == 0, "one of two does not agree");
		check(mesh_tally_majority(4, 8) == 0, "four of eight do not agree");
		check(mesh_tally_majority(5, 8) == 1, "five of eight agree");
		memset(book, 0, sizeof(book));
		memset(claims, 0, sizeof(claims));
		memcpy(claims[0].key, alpha, 32);
		snprintf(claims[0].name, 12, "Alpha");
		memcpy(claims[1].key, beta, 32);
		snprintf(claims[1].name, 12, "Beta");
		check(mesh_standings_commit(book, 4, left, claims, 2) == 1, "an agreed tally updates the book");
		check(book[0].used && book[0].wins == 1 && book[0].level == 0, "the winner records one win");
		check(book[1].used && book[1].losses == 1 && book[1].level == 0, "the loser records one loss");
		book[0].wins = 7;
		book[1].losses = 7;
		memcpy(claims[1].key, gamma, 32);
		snprintf(claims[1].name, 12, "Alpha");
		check(mesh_standings_commit(book, 4, left, claims, 2) == 1, "two claims of one name are parsed");
		check(book[0].wins == 7 && book[1].losses == 7, "a shared name does not change the book");
		check(mesh_tally_text("abc", "ranked", keys, 1, first, 2, right, (int)sizeof(right)) == 1,
			"a short roster is kept");
		check(strstr(right, "|1|2\n") != NULL, "fewer machines than rows stays in the tally");
	}
	if (failed)
		return 1;
	puts("peer mesh rules ok");
	return 0;
}
