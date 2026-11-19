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
	check(peer_score_answers(0) == 0, "a client that was not timed answered nobody");
	check(peer_score_answers(8) == 1, "a timed client counts as one answer");
	check(peer_hosts_over(20, peer_score_answers(8), 0, low, 20, peer_score_answers(1), 0, high) == 1,
		"a claimed answer count does not win the tie");
	check(peer_ranked_steps(10) == 0, "a few seconds of hearing a client does not open the window");
	check(peer_ranked_steps(300) == 1, "five minutes of hearing a client is one step");
	check(mesh_ranked_together(80, peer_ranked_steps(10), 40, peer_ranked_steps(10)) == 0,
		"a claimed long search is not a wider window");
	check(peer_offer_accepted(1, 0, 1, 0, 1) == 0, "an offer while gathering does not host");
	check(peer_offer_accepted(0, 1, 1, 1, 0) == 1, "the chosen host's offer is the game");
	check(peer_offer_accepted(0, 1, 0, 0, 1) == 0, "another client's offer is not the chosen game");
	check(peer_offer_accepted(0, 0, 0, 1, 1) == 0, "a search that already saw someone waits");
	check(peer_offer_accepted(0, 0, 0, 0, 1) == 1, "a client still alone can yield");
	check(peer_offer_accepted(0, 0, 0, 0, 0) == 0, "a client still alone keeps its lobby");
	check(peer_host_joins(1, 1, 1) == 0, "a lobby that already has another player stays");
	check(peer_host_joins(0, 1, 0) == 1, "a lobby of its own players takes the lower key");
	check(peer_host_joins(0, 0, 0) == 0, "a lobby of its own players keeps a higher key at first");
	check(peer_host_joins(0, 0, 1) == 1, "a lobby still alone takes the offer it can see");
	check(peer_offer_taken(1, 0, 0, 1) == 0, "a player already seated keeps the host");
	check(peer_offer_taken(0, 1, 0, 1) == 0, "a seated lobby stays on an echoed offer");
	check(peer_offer_taken(1, 0, 0, 0) == 1, "an empty lobby can still yield");
	check(peer_offer_taken(0, 0, 0, 0) == 0, "an offer nobody accepted stays out");
	check(peer_lobby_occupied(2, 1, 1) == 1, "two players on one count still occupy the lobby");
	check(peer_lobby_occupied(1, 1, 2) == 1, "a second machine occupies the lobby");
	check(peer_lobby_occupied(1, 1, 1) == 0, "one player on one machine is still alone");
	check(peer_lobby_occupied(2, 2, 1) == 0, "split screen on one machine is still alone");
	check(peer_search_new(0, 0) == 1, "a new peer who just broadcast can be answered");
	check(peer_search_new(0, 60000) == 1, "a new peer at one minute can be answered");
	check(peer_search_new(0, 60001) == 0, "a new peer older than one minute waits");
	check(peer_search_new(1, 0) == 0, "a peer already played is not a new peer");
	check(peer_search_new(0, -1) == 0, "a missing looking broadcast is not answered");
	check(peer_search_known(0) == 1, "a known peer who just broadcast is fresh");
	check(peer_search_known(120000) == 1, "a known peer at two minutes is fresh");
	check(peer_search_known(120001) == 0, "a known peer older than two minutes waits");
	check(peer_list_expired(0, 100, 220, 0, 0, 0, 120, 1200) == 0, "an unheard address stays through its limit");
	check(peer_list_expired(0, 100, 221, 0, 0, 0, 120, 1200) == 1, "an unheard address leaves after its limit");
	check(peer_list_expired(1000, 1000, 2200, 0, 0, 0, 120, 1200) == 0, "a heard peer stays through its quiet limit");
	check(peer_list_expired(1000, 1000, 2201, 0, 0, 0, 120, 1200) == 1, "a heard peer leaves after its quiet limit");
	check(peer_list_expired(50, 1000, 2200, 0, 0, 0, 120, 1200) == 0, "a loaded peer stays for one quiet period");
	check(peer_list_expired(50, 1000, 2201, 0, 0, 0, 120, 1200) == 1, "a loaded peer leaves after that quiet period");
	check(peer_list_expired(5000, 1000, 5010, 0, 0, 0, 120, 1200) == 0, "a peer heard just now stays");
	check(peer_list_expired(50, 1000, 9000, 1, 0, 0, 120, 1200) == 0, "an invite stays while it is unanswered");
	check(peer_list_expired(50, 1000, 9000, 0, 1, 0, 120, 1200) == 0, "a ban that is still running stays");
	check(peer_list_expired(50, 1000, 9000, 0, 0, 1, 120, 1200) == 0, "a ban record stays until the list needs the slot");
	check(peer_list_replaceable(1, 0, 0, 0, 1, 0) == 0, "a named address does not replace a heard peer");
	check(peer_list_replaceable(0, 0, 0, 0, 1, 0) == 1, "a named address can replace one that was never heard");
	check(peer_list_replaceable(0, 0, 0, 1, 1, 0) == 0, "a named address does not replace a ban record");
	check(peer_list_replaceable(1, 0, 0, 0, 0, 0) == 1, "a heard peer can replace the quietest heard peer");
	check(peer_list_replaceable(1, 0, 0, 0, 0, 1) == 0, "a peer heard a moment ago stays");
	check(peer_list_replaceable(0, 0, 0, 1, 0, 0) == 1, "a heard peer can replace a finished ban");
	check(peer_list_replaceable(1, 1, 0, 0, 0, 0) == 0, "an open invite is not replaced");
	check(peer_list_replaceable(1, 0, 1, 0, 0, 0) == 0, "a ban that is still running is not replaced");
	{
		int order[4];
		unsigned char bytes[64];
		int index;
		int seen[4];

		for (index = 0; index < 64; index++)
			bytes[index] = (unsigned char)(index * 17 + 3);
		check(peer_search_shuffle(order, 4, bytes, 64) == 1, "a shuffle finishes on entropy");
		memset(seen, 0, sizeof(seen));
		for (index = 0; index < 4; index++)
		{
			check(order[index] >= 0 && order[index] < 4, "a shuffle stays inside the list");
			seen[order[index]] = 1;
		}
		check(seen[0] && seen[1] && seen[2] && seen[3], "a shuffle keeps every peer once");
		check(peer_search_shuffle(order, 4, bytes, 4) == 0, "a short entropy buffer does not finish a biased draw");
		check(peer_search_shuffle(order, 1, NULL, 0) == 1, "one peer needs no entropy");
	}
	check(peer_answer_admits(1, 1, 1, 0) == 1, "an echo of the nonce we sent fills a seat");
	check(peer_answer_admits(0, 1, 1, 0) == 0, "their looking reply does not fill a seat");
	check(peer_answer_admits(2, 1, 1, 0) == 0, "a different role does not fill a seat");
	check(peer_answer_admits(1, 0, 1, 0) == 0, "an echo we never sent does not fill a seat");
	check(peer_answer_admits(1, 1, 0, 0) == 0, "a different nonce does not fill a seat");
	check(peer_answer_admits(1, 1, 1, 1) == 0, "a blank nonce does not fill a seat");
	check(peer_offer_invites(1, 0, 2) == 0, "a solo host's unanswered shouts are not a crowd");
	check(peer_offer_invites(1, 1, 2) == 2, "a host who seated someone still counts the rest");
	check(peer_offer_invites(0, 0, 1) == 1, "during the measurement an unanswered reply still counts");
	check(peer_search_seen(0, 0, 0) == 0, "a search with nobody else is still alone");
	check(peer_search_seen(0, 1, 0) == 1, "an unanswered reply counts as someone else");
	check(peer_search_seen(1, 0, 0) == 1, "a peer already in the lobby counts as someone else");
	check(peer_search_seen(0, 0, 1) == 1, "a search that already grouped waits");
	check(peer_search_fill(1, 0, 1) == 1, "a new peer fills an open seat");
	check(peer_search_fill(1, 60001, 1) == 0, "a stale new peer does not fill a seat");
	check(peer_search_fill(0, 1000, 1) == 1, "a known peer fills an open seat");
	check(peer_search_fill(0, 120001, 1) == 0, "a stale known peer does not fill a seat");
	check(peer_search_fill(1, 0, 0) == 0, "a full search does not take another peer");
	check(peer_lobby_echo(1, 1, 1) == 0, "a host does not echo into another lobby");
	check(peer_lobby_echo(0, 1, 1) == 1, "an invited peer is echoed while others are here");
	check(peer_lobby_echo(0, 0, 0) == 1, "a search with nobody else echoes");
	check(peer_lobby_echo(0, 0, 1) == 0, "a stranger is not echoed while others are here");
	check(peer_offer_from_echo(1, 0, 0, 0, 0, 0, 0) == 1, "an answered invite joins that lobby");
	check(peer_offer_from_echo(0, 0, 0, 0, 0, 0, 0) == 0, "an offer we did not answer is left to the host rule");
	check(peer_offer_from_echo(1, 1, 0, 0, 0, 0, 0) == 0, "a peer already in this lobby uses the host rule");
	check(peer_offer_from_echo(1, 0, 1, 0, 0, 0, 0) == 0, "another player in this search keeps the lobby");
	check(peer_offer_from_echo(1, 0, 0, 1, 0, 0, 0) == 0, "a reply still out to someone else keeps the lobby");
	check(peer_offer_from_echo(1, 0, 0, 0, 1, 0, 0) == 0, "the measurement does not leave for that offer yet");
	check(peer_offer_from_echo(1, 0, 0, 0, 0, 1, 0) == 0, "gathering does not leave for that offer");
	check(peer_offer_from_echo(1, 0, 0, 0, 0, 0, 1) == 0, "a chosen host stays the chosen host");
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
	check(mesh_gamertag_ok("1") == 1, "a number is a gamertag");
	check(mesh_gamertag_ok("5555") == 1, "digits are a gamertag");
	check(mesh_gamertag_ok("...") == 0, "punctuation is not a gamertag");
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
		check(mesh_wait_seconds("ffa", 1, 8, 200) == -1, "one free for all player keeps searching");
		check(mesh_wait_seconds("ffa", 2, 8, 0) == 30, "free for all counts down with two");
		check(mesh_wait_seconds("ffa", 3, 8, 10) == 30, "free for all counts down before it is full");
		check(mesh_wait_seconds("ffa", 8, 8, 1) == 30, "a full free for all counts down");
		check(mesh_wait_seconds("ffa", 2, 16, 0) == 30, "free for all of sixteen counts down with two");
		check(mesh_wait_seconds("ffa", 16, 16, 1) == 30, "a full free for all of sixteen counts down");
		check(mesh_seats_unfilled(16, 16, 15) == 1, "fifteen players leave a seat in sixteen");
		check(mesh_seats_unfilled(16, 16, 16) == 0, "sixteen players fill sixteen seats");
		check(mesh_wait_seconds("hardcore", 2, 4, 200) == -1, "2v2 stays open with two");
		check(mesh_wait_seconds("hardcore", 3, 4, 200) == -1, "2v2 stays open with three");
		check(mesh_wait_seconds("hardcore", 4, 4, 0) == 30, "2v2 counts down at four");
		check(mesh_wait_seconds("ranked", 3, 4, 200) == -1, "ranked waits for four");
		check(mesh_wait_seconds("ranked", 4, 4, 1) == 30, "ranked counts down at four");
		check(mesh_seats_unfilled(8, 8, 2) == 1, "advertised seats do not fill the lobby");
		check(mesh_seats_unfilled(8, 8, 8) == 0, "eight players fill eight seats");
		check(mesh_seats_unfilled(2, 8, 2) == 0, "a short social search is not that wait");
		check(!strcmp(mesh_map_for((const unsigned char *)"\x00", "ffa"), "hangemhigh"), "the first map is hangemhigh");
		check(!strcmp(mesh_map_for((const unsigned char *)"\x0c", "ffa"), "longest"), "free for all uses every stock map");
		check(!strcmp(mesh_map_for((const unsigned char *)"\x05", "ranked"), "hangemhigh"), "ranked stays on the five smaller maps");
		check(mesh_score("hardcore") == 25, "hardcore is to 25");
		check(mesh_score("ranked") == 25, "ranked is to 25");
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
			games = 6;
			mesh_result_apply(team, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", NULL, &games, &level, &wins, &losses);
			check(level == 10 && wins == 4 && losses == 0 && games == 6,
				"a match with a player gone changes no rank");
			wins = 0;
			losses = 2;
			level = 12;
			games = 6;
			mesh_result_apply(team, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", NULL, &games, &level, &wins, &losses);
			check(level == 12 && wins == 0 && losses == 2 && games == 6,
				"a player who stayed is unchanged too");
			{
				const char *social =
					"{\"playlist\":\"ffa\",\"players\":["
					"{\"account\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"quit\":false,\"score\":10,\"place\":1},"
					"{\"account\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"quit\":true,\"score\":1,\"place\":2}]}";

				games = 3;
				mesh_result_apply(social, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", NULL, &games, &level, &wins, &losses);
				check(games == 3, "a social match with a player gone adds no game");
			}
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
				{
					const char *stamped =
						"{\"playlist\":\"ranked\",\"players\":["
						"{\"account\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"name\":\"Alpha\",\"quit\":false,\"score\":1,\"place\":2},"
						"{\"account\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"name\":\"Alpha\",\"quit\":false,\"score\":10,\"place\":1}]}";

					wins = 5;
					check(mesh_result_apply(stamped, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "Alpha", &games, &level,
						&wins, &losses) == 0, "two stamped rows with one gamertag are left alone");
					check(wins == 5, "the better of those rows is not kept");
				}
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
		char key_names[2][12];
		char flipped_names[2][12];
		char same_names[2][12];
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
		snprintf(key_names[0], 12, "Beta");
		snprintf(key_names[1], 12, "Alpha");
		snprintf(flipped_names[0], 12, "Alpha");
		snprintf(flipped_names[1], 12, "Beta");
		snprintf(same_names[0], 12, "Alpha");
		snprintf(same_names[1], 12, "Alpha");
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
		check(mesh_tally_text("abc", "ranked", keys, key_names, 2, first, 2, left, (int)sizeof(left)) == 1,
			"tally text fits");
		check(mesh_tally_text("abc", "ranked", flipped_keys, flipped_names, 2, second, 2, right, (int)sizeof(right)) == 1,
			"reversed tally fits");
		check(strcmp(left, right) == 0, "tally text does not depend on row order");
		check(mesh_tally_humans(left) == 2, "tally names the roster");
		check(mesh_tally_key_count(left) == 2, "tally lists both installs");
		check(mesh_tally_key_name(left, alpha, right) == 1 && !strcmp(right, "Alpha"),
			"the tally names the install");
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
		check(mesh_tally_majority(8, 16) == 0, "eight of sixteen do not agree");
		check(mesh_tally_majority(9, 16) == 1, "nine of sixteen agree");
		memset(book, 0, sizeof(book));
		memset(claims, 0, sizeof(claims));
		memcpy(claims[0].key, alpha, 32);
		snprintf(claims[0].name, 12, "Beta");
		memcpy(claims[1].key, beta, 32);
		snprintf(claims[1].name, 12, "Alpha");
		check(mesh_standings_commit(book, 4, left, claims, 2) == 1, "an agreed tally updates the book");
		check(book[0].used && !strcmp(book[0].name, "Alpha") && book[0].wins == 1 && book[0].level == 0,
			"the winner records one win");
		check(book[1].used && !strcmp(book[1].name, "Beta") && book[1].losses == 1 && book[1].level == 0,
			"the loser records one loss");
		check(mesh_tally_text("abc", "ranked", keys, same_names, 2, first, 2, right, (int)sizeof(right)) == 1,
			"a tally can repeat a gamertag");
		book[0].wins = 7;
		book[1].losses = 7;
		check(mesh_standings_commit(book, 4, right, claims, 2) == 1, "two install lines with one name are parsed");
		check(book[0].wins == 7 && book[1].losses == 7, "a shared name does not change the book");
		check(mesh_tally_text("abc", "ranked", keys, key_names, 1, first, 2, right, (int)sizeof(right)) == 1,
			"a short roster is kept");
		check(strstr(right, "|1|2\n") != NULL, "fewer machines than rows stays in the tally");
		{
			char long_name[1][12];
			struct mesh_tally_row long_row;
			unsigned char long_key[1][32];

			memset(&long_row, 0, sizeof(long_row));
			memcpy(long_key[0], alpha, 32);
			snprintf(long_name[0], 12, "MasterChief");
			snprintf(long_row.name, 12, "MasterChief");
			long_row.score = 10;
			long_row.place = 1;
			long_row.team = -1;
			check(mesh_tally_text("abc", "ranked", long_key, long_name, 1, &long_row, 1, right, (int)sizeof(right)) == 1,
				"an 11 character gamertag fits the tally");
			check(mesh_tally_key_count(right) == 1, "that tally still lists the install");
			check(mesh_tally_key_name(right, alpha, left) == 1 && !strcmp(left, "MasterChief"),
				"that tally keeps all 11 characters");
		}
		{
			struct mesh_tally_row twins[2];
			char twin_name[1][12];
			unsigned char twin_key[1][32];

			memset(twins, 0, sizeof(twins));
			memcpy(twin_key[0], alpha, 32);
			snprintf(twin_name[0], 12, "Alpha");
			snprintf(twins[0].name, 12, "Alpha");
			twins[0].score = 10;
			twins[0].place = 1;
			twins[0].team = -1;
			snprintf(twins[1].name, 12, "Alpha");
			twins[1].score = 1;
			twins[1].place = 2;
			twins[1].team = -1;
			check(mesh_tally_text("abc", "ranked", twin_key, twin_name, 1, twins, 2, right, (int)sizeof(right)) == 1,
				"one install can have two rows of its gamertag");
			book[0].wins = 3;
			memcpy(claims[0].key, alpha, 32);
			snprintf(claims[0].name, 12, "Lie");
			check(mesh_standings_commit(book, 4, right, claims, 1) == 1, "that tally still parses");
			check(book[0].wins == 3, "the better of the two rows is not the result");
		}
		{
			struct mesh_tally_row gone[2];
			char text[512];

			memset(gone, 0, sizeof(gone));
			snprintf(gone[0].name, 12, "Alpha");
			gone[0].score = 10;
			gone[0].place = 1;
			gone[0].team = -1;
			snprintf(gone[1].name, 12, "Beta");
			gone[1].score = 1;
			gone[1].place = 2;
			gone[1].team = -1;
			gone[1].quit = 1;
			check(mesh_tally_text("abc", "ranked", keys, key_names, 2, gone, 2, text, (int)sizeof(text)) == 1,
				"a tally with a player who left fits");
			check(mesh_tally_key_quit(text, beta) == 1, "the player who left is a vote against that install");
			check(mesh_tally_key_left(text, beta) == 1, "the player who left is marked as gone");
			check(mesh_tally_key_quit(text, alpha) == 0, "a player who stayed is not a leave vote");
			check(mesh_tally_key_left(text, alpha) == 0, "a player who stayed is not marked as gone");
			gone[0].quit = 400;
			gone[1].quit = 100;
			check(mesh_tally_text("abc", "ranked", keys, key_names, 2, gone, 2, text, (int)sizeof(text)) == 1,
				"two leave ticks fit");
			check(mesh_tally_key_quit(text, beta) == 1, "the first player to leave is the vote");
			check(mesh_tally_key_quit(text, alpha) == 0, "a later leave is not a vote");
			check(mesh_tally_key_left(text, alpha) == 1, "a later leave is still a player who left");
			gone[0].quit = 100;
			check(mesh_tally_text("abc", "ranked", keys, key_names, 2, gone, 2, text, (int)sizeof(text)) == 1,
				"one leave tick for both fits");
			check(mesh_tally_key_quit(text, alpha) == 1 && mesh_tally_key_quit(text, beta) == 1,
				"players who leave on the same tick are both first");
			snprintf(gone[0].name, 12, "Beta");
			gone[0].quit = 1;
			gone[1].quit = 0;
			check(mesh_tally_text("abc", "ranked", keys, key_names, 2, gone, 2, text, (int)sizeof(text)) == 1,
				"two rows of one name fit");
			check(mesh_tally_key_quit(text, beta) == 0, "two rows of the name are not a leave vote");
		}
	}
	{
		struct mesh_tally_row rows[16];
		unsigned char keys[16][32];
		char names[16][12];
		char text[2048];
		char match_id[40];
		int index;
		int length;

		memset(rows, 0, sizeof(rows));
		memset(match_id, 'a', 32);
		match_id[32] = 0;
		for (index = 0; index < 16; index++)
		{
			memset(keys[index], (unsigned char)(index + 1), 32);
			snprintf(names[index], 12, "%02d345678901", index);
			snprintf(rows[index].name, 12, "%s", names[index]);
			rows[index].team = -1;
			rows[index].score = 50;
			rows[index].place = index + 1;
			rows[index].quit = index == 0 ? 1 : 0;
		}
		check(mesh_tally_text(match_id, "ffa", keys, names, 16, rows, 16, text, (int)sizeof(text)) == 1,
			"sixteen players fit the scoreboard");
		length = (int)strlen(text);
		check(mesh_tally_humans(text) == 16, "the scoreboard names sixteen installs");
		check(mesh_tally_key_count(text) == 16, "the scoreboard lists sixteen installs");
		check(mesh_tally_key_quit(text, keys[0]) == 1, "the first of sixteen to leave is a vote");
		check(mesh_tally_key_quit(text, keys[1]) == 0, "a later player of sixteen who stayed is not a vote");
		check(length > 1042, "sixteen names need more than one scoreboard packet");
		check(length < 2048, "sixteen names fit the scoreboard buffer");
	}
	if (failed)
		return 1;
	puts("peer mesh rules ok");
	return 0;
}
