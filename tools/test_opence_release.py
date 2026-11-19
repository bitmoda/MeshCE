#!/usr/bin/env python3
"""The OpenCE publish plan, without talking to GitHub."""

import unittest

from opence_release import plan


def release(tag, assets, prerelease=False):
    return {"tag": tag, "assets": assets, "prerelease": prerelease}


class OpenCEReleasePlanTest(unittest.TestCase):
    def test_first_tag_is_opened_by_mac(self):
        result = plan([], "mac")
        self.assertEqual(result["action"], "open")
        self.assertEqual(result["tag"], "opence-testing-1")
        self.assertFalse(result["latest"])

    def test_windows_and_linux_wait_for_mac_to_open(self):
        self.assertEqual(plan([], "windows")["action"], "wait")
        self.assertEqual(plan([], "linux")["action"], "wait")
        self.assertEqual(plan([], "linux")["tag"], "opence-testing-1")

    def test_missing_file_is_added_to_the_same_tag(self):
        releases = [release("opence-testing-3", ["opence-macos-arm64.dmg"], True)]
        linux = plan(releases, "linux")
        self.assertEqual(linux["action"], "add")
        self.assertEqual(linux["tag"], "opence-testing-3")
        self.assertEqual(linux["file"], "opence-linux-release.zip")

    def test_complete_tag_opens_the_next_one_on_mac_only(self):
        releases = [release("opence-testing-3", [
            "opence-macos-arm64.dmg",
            "opence-windows-release.zip",
            "opence-linux-release.zip",
        ], True)]
        self.assertEqual(plan(releases, "mac")["tag"], "opence-testing-4")
        self.assertEqual(plan(releases, "mac")["action"], "open")
        self.assertEqual(plan(releases, "windows")["action"], "wait")
        self.assertEqual(plan(releases, "windows")["tag"], "opence-testing-4")

    def test_a_machine_waits_when_its_file_is_already_there(self):
        releases = [release("opence-testing-3", [
            "opence-macos-arm64.dmg",
            "opence-windows-release.zip",
        ], True)]
        windows = plan(releases, "windows")
        self.assertEqual(windows["action"], "wait")
        self.assertEqual(windows["tag"], "opence-testing-3")

    def test_meshce_tags_are_not_this_line(self):
        releases = [
            release("build-75", ["halo-windows-release.zip", "MeshCE-macos-arm64.dmg"]),
            release("testing-75", ["halo-linux-release.zip"], True),
            release("opence-testing-2", ["opence-macos-arm64.dmg"], True),
            release("opence-build-2", [
                "opence-macos-arm64.dmg",
                "opence-windows-release.zip",
                "opence-linux-release.zip",
            ]),
        ]
        linux = plan(releases, "linux")
        self.assertEqual(linux["action"], "add")
        self.assertEqual(linux["tag"], "opence-testing-2")
        self.assertEqual(linux["highest_build"], 2)

    def test_a_meshce_file_on_an_opence_tag_is_reported(self):
        releases = [release("opence-testing-1", ["halo-windows-release.zip"], True)]
        result = plan(releases, "windows")
        self.assertTrue(result["warnings"])
        self.assertIn("MeshCE", result["warnings"][0])

    def test_an_extra_suffix_is_not_a_number(self):
        releases = [release("opence-testing-4extra", ["opence-macos-arm64.dmg"], True)]
        self.assertEqual(plan(releases, "mac")["tag"], "opence-testing-1")


if __name__ == "__main__":
    unittest.main()
