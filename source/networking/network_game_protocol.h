/*
NETWORK_GAME_PROTOCOL.H

Shared game-protocol constants for the connection and message consumers.
*/

#ifndef __NETWORK_GAME_PROTOCOL_H
#define __NETWORK_GAME_PROTOCOL_H
#pragma once

/* ---------- constants */

/* This client does not bind Halo's 5150 and 5151, or the old matchmaking
ports 5160, 5161, and 5162. Discovery is 38952. */
enum
{
	NETWORK_GAME_SERVER_PORT = 38950,
	NETWORK_GAME_CLIENT_PORT = 38951,
	NETWORK_MESH_PORT = 38952,
	/* one port for each playlist, none of them the old 5160, 5161, or 5162 */
	NETWORK_PLAYLIST_PORT_RANKED = 38960,
	NETWORK_PLAYLIST_PORT_FFA = 38961,
	NETWORK_PLAYLIST_PORT_HARDCORE = 38962,
};

enum
{
	_rejection_code_version_too_old = 0,
	_rejection_code_version_too_new,
	_rejection_code_bad_join_token,
	_rejection_code_bad_password,
	_rejection_code_game_is_full,
	_rejection_code_game_is_closed,
	_rejection_code_blacklisted_machine,
	NUMBER_OF_SERVER_REJECTION_CODES,
};

#endif // __NETWORK_GAME_PROTOCOL_H
