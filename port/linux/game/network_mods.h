#ifndef NETWORK_MODS_H
#define NETWORK_MODS_H
#include "cseries.h"
/* The host tells every machine to play the map's own tags. A machine that
does not answer is dropped. */
void network_mods_reset(void);
void network_mods_update(void);
void network_mods_send_policy(long machine_index);
boolean network_mods_handles_message(word const *message, word size);
void network_mods_handle_message(long machine_index, word const *message, word size);
#endif
