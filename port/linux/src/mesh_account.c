/*
MESH_ACCOUNT.C

Issues a Mesh ID from this install's key. Rank changes come from an agreed
tally on the mesh, not from a result this client applies on its own.
*/

#include "platform.h"
#include "mesh_service.h"
#include "peer_mesh.h"
#include "rank_account.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t serve_lock = PTHREAD_MUTEX_INITIALIZER;

void mesh_account_serve(void)
{
	static unsigned last;
	unsigned char key[32];
	char request[64];
	char reply[320];
	char body[2560];
	char topic[96];
	unsigned generation = 0;
	unsigned now = GetTickCount();

	if (pthread_mutex_trylock(&serve_lock) != 0)
		return;
	if (last && now - last < 1000)
	{
		pthread_mutex_unlock(&serve_lock);
		return;
	}
	last = now;
	if (!peer_mesh_public(key))
	{
		pthread_mutex_unlock(&serve_lock);
		return;
	}
	if (rank_register_pending(request, (int)sizeof(request)))
	{
		if (mesh_register_reply(request, key, reply, (int)sizeof(reply)))
			rank_apply_downlink("hceu/3/mm/v1/register/mesh/reply", reply, "mesh");
	}
	if (rank_profile_pending(topic, (int)sizeof(topic), body, (int)sizeof(body), &generation))
		rank_profile_sent(generation);
	/* A result left from an older path is acknowledged and not applied.
	The agreed tally is what changes the shared list. */
	if (rank_result_pending(body, (int)sizeof(body)))
		rank_result_sent();
	peer_mesh_take_result(body, (int)sizeof(body));
	pthread_mutex_unlock(&serve_lock);
}
