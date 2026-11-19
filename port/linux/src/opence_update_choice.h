/*
OPENCE_UPDATE_CHOICE.H

Which GitHub release the self-updater should install. Main releases are
tags opence-build-<number> and are not prereleases. Testing releases are tags
opence-testing-<number> and are prereleases. The same channel moves only to a
higher number. Turning testing off installs the latest main release even
when its number is lower.
*/

#ifndef OPENCE_UPDATE_CHOICE_H
#define OPENCE_UPDATE_CHOICE_H

struct opence_update_choice
{
	long number;
	char tag[64];
};

/* list_json is GitHub's releases array. latest_json is the one release
GitHub calls latest, or null. Either may be null. asset is this platform's
file name. Returns 1 when choice should be installed. */
int opence_update_choice_select(const char *list_json, const char *latest_json, const char *asset,
	int want_testing, int on_testing, long current, struct opence_update_choice *choice);

#endif
