#!/usr/bin/env python3
"""Plan the next OpenCE file on bitmoda/MeshCE.

This prints what to publish. It does not upload, create a tag, or mark a
release Latest.
"""

import argparse
import json
import subprocess
import sys

REPOSITORY = "bitmoda/MeshCE"
FILES = {
    "mac": "opence-macos-arm64.dmg",
    "windows": "opence-windows-release.zip",
    "linux": "opence-linux-release.zip",
}
MESHCE_FILES = {
    "MeshCE-macos-arm64.dmg",
    "halo-windows-release.zip",
    "halo-linux-release.zip",
}


def _number(tag, prefix):
    if not isinstance(tag, str) or not tag.startswith(prefix):
        return None
    rest = tag[len(prefix):]
    if not rest.isdigit() or rest.startswith("0") and rest != "0":
        return None
    number = int(rest)
    return number if number > 0 else None


def plan(releases, platform):
    """releases is a list of {tag, assets, prerelease}. platform is mac,
    windows, or linux. The result says open, add, or wait."""
    if platform not in FILES:
        raise ValueError(platform)
    filename = FILES[platform]
    best = None
    best_assets = set()
    highest_build = 0
    warnings = []
    for release in releases:
        tag = release.get("tag", "")
        assets = set(release.get("assets") or [])
        testing = _number(tag, "opence-testing-")
        built = _number(tag, "opence-build-")
        if testing is not None and (best is None or testing > best):
            best = testing
            best_assets = assets
        if built and built > highest_build:
            highest_build = built
        if (testing or built) and assets & MESHCE_FILES:
            warnings.append(f"{tag} carries a MeshCE file and must not")
    if best is None or all(name in best_assets for name in FILES.values()):
        number = 1 if best is None else best + 1
        tag = f"opence-testing-{number}"
        action = "open" if platform == "mac" else "wait"
    elif filename not in best_assets:
        number = best
        tag = f"opence-testing-{number}"
        action = "add"
    else:
        number = best
        tag = f"opence-testing-{number}"
        action = "wait"
    return {
        "action": action,
        "tag": tag,
        "number": number,
        "file": filename,
        "highest_build": highest_build,
        "latest": False,
        "warnings": warnings,
    }


def fetch_releases():
    raw = subprocess.check_output(
        ["gh", "api", f"repos/{REPOSITORY}/releases?per_page=100"], text=True)
    data = json.loads(raw)
    return [
        {
            "tag": item.get("tag_name", ""),
            "assets": [asset.get("name", "") for asset in item.get("assets") or []],
            "prerelease": bool(item.get("prerelease")),
        }
        for item in data
    ]


def print_plan(result):
    print(f"{result['action']} {result['tag']} {result['file']}")
    print(f"highest opence-build is {result['highest_build']}")
    print("this release is not marked Latest")
    print("do not attach MeshCE-macos-arm64.dmg, halo-windows-release.zip, or halo-linux-release.zip")
    for warning in result["warnings"]:
        print(f"warning: {warning}")


def main(argv=None):
    parser = argparse.ArgumentParser(description="Plan the next OpenCE testing file.")
    parser.add_argument("platform", choices=sorted(FILES))
    parser.add_argument("command", nargs="?", default="plan", choices=("plan",))
    args = parser.parse_args(argv)
    print_plan(plan(fetch_releases(), args.platform))
    return 0


if __name__ == "__main__":
    sys.exit(main())
