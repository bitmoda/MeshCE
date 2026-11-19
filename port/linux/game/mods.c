/*
MODS.C

The console names that used to play tag files over a map. They stay so a
script that calls them still runs. The map's own tags are what is played.
*/

#include "cseries.h"
#include "main/console.h"
#include "mods.h"

void mods_reload(
	void)
{
	return;
}

void mods_enable(
	boolean enabled)
{
	(void)enabled;

	return;
}

void mods_list(
	void)
{
	console_printf(FALSE, "mods: this game plays the map's own tags");

	return;
}
