#!/usr/bin/env python3
"""The OpenCE updater ignores MeshCE tags and MeshCE file names."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "port/linux/src/opence_update_choice.c"

DRIVER = r"""
#include "opence_update_choice.h"
#include <stdio.h>
#include <string.h>

static int expect(const char *name, int found, struct opence_update_choice *choice, long number, const char *tag)
{
	int ok = found == (number != 0) && (!found || (choice->number == number && !strcmp(choice->tag, tag)));
	if (!ok)
		fprintf(stderr, "%s: found %d number %ld tag %s, wanted %ld %s\n",
			name, found, found ? choice->number : 0, found ? choice->tag : "", number, tag ? tag : "");
	return ok;
}

int main(void)
{
	const char *list =
		"[{"
		"\"tag_name\":\"build-75\",\"draft\":false,\"prerelease\":false,"
		"\"body\":\"notes mention \\\"opence-build-90\\\" and halo-windows-release.zip\","
		"\"assets\":[{\"name\":\"halo-windows-release.zip\"},{\"name\":\"MeshCE-macos-arm64.dmg\"}]"
		"},{"
		"\"tag_name\":\"opence-build-2\",\"draft\":false,\"prerelease\":false,"
		"\"body\":\"Network version: 44\","
		"\"assets\":[{\"name\":\"opence-windows-release.zip\"}]"
		"},{"
		"\"tag_name\":\"opence-testing-4\",\"draft\":false,\"prerelease\":true,"
		"\"assets\":[{\"name\":\"opence-windows-release.zip\"}]"
		"},{"
		"\"tag_name\":\"opence-testing-9\",\"draft\":false,\"prerelease\":false,"
		"\"assets\":[{\"name\":\"opence-windows-release.zip\"}]"
		"},{"
		"\"tag_name\":\"opence-build-2extra\",\"draft\":false,\"prerelease\":false,"
		"\"assets\":[{\"name\":\"opence-windows-release.zip\"}]"
		"}]";
	const char *latest =
		"{\"tag_name\":\"build-75\",\"draft\":false,\"prerelease\":false,"
		"\"assets\":[{\"name\":\"halo-windows-release.zip\"},{\"name\":\"MeshCE-macos-arm64.dmg\"}]}";
	struct opence_update_choice choice;
	int failed = 0;

	failed += !expect("main selects opence", opence_update_choice_select(list, latest, "opence-windows-release.zip", 0, 0, 1, &choice),
		&choice, 2, "opence-build-2");
	failed += !expect("main current stays", opence_update_choice_select(list, latest, "opence-windows-release.zip", 0, 0, 2, &choice),
		&choice, 0, NULL);
	failed += !expect("testing selects", opence_update_choice_select(list, latest, "opence-windows-release.zip", 1, 1, 1, &choice),
		&choice, 4, "opence-testing-4");
	failed += !expect("testing current stays", opence_update_choice_select(list, latest, "opence-windows-release.zip", 1, 1, 4, &choice),
		&choice, 0, NULL);
	failed += !expect("leave testing", opence_update_choice_select(list, latest, "opence-windows-release.zip", 0, 1, 9, &choice),
		&choice, 2, "opence-build-2");
	failed += !expect("linux file missing", opence_update_choice_select(list, latest, "opence-linux-release.zip", 0, 0, 1, &choice),
		&choice, 0, NULL);
	failed += !expect("meshce file is not ours", opence_update_choice_select(list, latest, "halo-windows-release.zip", 0, 0, 1, &choice),
		&choice, 0, NULL);
	return failed ? 1 : 0;
}
"""


class OpenCEUpdateChoiceTest(unittest.TestCase):
    def test_channel_rules(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            driver = directory / "driver.c"
            driver.write_text(DRIVER)
            binary = directory / "choice"
            subprocess.run(
                ["clang", "-Wall", "-Wextra", "-Werror", "-I", str(SOURCE.parent),
                 str(SOURCE), str(driver), "-o", str(binary)],
                check=True)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
