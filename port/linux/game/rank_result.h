/*
RANK_RESULT.H

The host's match report, and the scoreboard name with its competitive level.
The Mesh ID is drawn after a gamertag in smaller characters.
*/

#ifndef __HALO_RANK_RESULT_H
#define __HALO_RANK_RESULT_H

#include "cseries.h"
#include "math/real_math.h"

struct player_datum;

void rank_match_report(void);
wchar_t const *rank_scoreboard_name(wchar_t const *name);

/* This install's Mesh ID, M and eight hex digits, with its end. 1 when the
key is known. local_install uses this client's own key. id holds 10 wchars. */
int rank_mesh_id_text(int machine, int local_install, wchar_t id[10]);

/* The install a player belongs to. local_install is 1 for a player on this
client, including one whose machine index has not been assigned yet. */
void rank_player_install(struct player_datum const *player, int *machine, int *local_install);

/* The Mesh ID just after the gamertag the current draw mode lays out in
line_bounds, at about two thirds of that text's size, on the same capital
line. The competitive level follows at the gamertag's size when with_level
is set. Returns the layout x where the next text of the gamertag's size
starts. The text scale already set is restored. No key yet: the gamertag
is left as it was drawn. */
short rank_draw_name_tail(
	rectangle2d const *line_bounds,
	wchar_t const *name,
	int machine,
	int local_install,
	long font_index,
	real_argb_color const *color,
	int with_level);

#endif
