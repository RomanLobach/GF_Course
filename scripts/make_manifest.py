#!/usr/bin/env python3
"""Package a release: copy both firmware images under release names and write manifest.json.

The manifest is what devices read for OTA: one entry per role with the download URL, size and
SHA-256 of that role's image. Run from the project root after `pio run -e base -e rover`, on a
commit tagged hw6-vX.Y.Z (refuses anything else, so a release always matches its tag):

    python scripts/make_manifest.py --repo RomanLobach/GF_Course --out dist
"""
import argparse
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys

PROJECT = "ttgo-lora-bench"
TAG_PREFIX = "hw6-v"
ROLES = ("base", "rover")


def git(*args):
    return subprocess.check_output(["git", *args], text=True).strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo", required=True, help="owner/name on GitHub")
    ap.add_argument("--out", default="dist", help="output directory")
    args = ap.parse_args()

    try:
        tag = git("describe", "--tags", "--exact-match", "--match", TAG_PREFIX + "*", "HEAD")
    except subprocess.CalledProcessError:
        sys.exit("HEAD is not tagged %s* - releases are built from a tag only" % TAG_PREFIX)
    if git("status", "--porcelain"):
        sys.exit("working tree is dirty - a release must be built from the clean tag")
    version = tag[len(TAG_PREFIX):]

    os.makedirs(args.out, exist_ok=True)
    manifest = {
        "project": PROJECT,
        "version": version,
        "tag": tag,
        "git": git("rev-parse", "--short=8", "HEAD"),
        "released": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "images": {},
    }
    for role in ROLES:
        src = os.path.join(".pio", "build", role, "firmware.bin")
        name = "%s-%s-%s.bin" % (PROJECT, role, version)
        shutil.copyfile(src, os.path.join(args.out, name))
        with open(src, "rb") as f:
            data = f.read()
        manifest["images"][role] = {
            "url": "https://github.com/%s/releases/download/%s/%s" % (args.repo, tag, name),
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        }

    with open(os.path.join(args.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
