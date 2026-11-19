/*
MODS.H

The console names that used to play tag files over a map. They stay so a
script that calls them still runs. The map's own tags are what is played.
*/

#ifndef __MODS_H
#define __MODS_H
#pragma once

#include "cseries.h"

void mods_reload(void);
void mods_enable(boolean enabled);
void mods_list(void);

#endif
