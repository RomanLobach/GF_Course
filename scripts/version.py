# PlatformIO pre-build script: derives the firmware version from git and passes it to the
# compiler as -D macros (FW_VERSION, FW_GIT_HASH, FW_BUILD_DATE, FW_DIRTY), so no version
# string is ever typed by hand.
#
#   tag hw6-v0.1.0 on HEAD, clean tree     -> 0.1.0
#   3 commits after hw6-v0.1.0             -> 0.1.0-3-g1a2b3c4d
#   uncommitted changes                    -> ...-dirty
#   no hw6-v* tag yet                      -> 0.0.0-dev.<commits>-g<hash>
#   no git at all                          -> 0.0.0-nogit, hash = sha256 of the sources
#
# FW_BUILD_DATE is the commit date, not "now": the same commit always produces the same
# binary, and an unchanged version doesn't force a full rebuild on every `pio run`.
Import("env")  # noqa: F821 - provided by PlatformIO/SCons

import hashlib
import os
import subprocess

TAG_PREFIX = "hw6-v"
PROJECT_DIR = env.subst("$PROJECT_DIR")  # noqa: F821
HASHED_DIRS = ("src", "include", "scripts")
HASHED_FILES = ("platformio.ini",)


def git(*args):
    return subprocess.check_output(
        ["git", *args], cwd=PROJECT_DIR, stderr=subprocess.DEVNULL, text=True
    ).strip()


def sources_hash():
    h = hashlib.sha256()
    paths = [os.path.join(PROJECT_DIR, f) for f in HASHED_FILES]
    for d in HASHED_DIRS:
        for root, _, files in os.walk(os.path.join(PROJECT_DIR, d)):
            paths += [os.path.join(root, f) for f in files]
    for p in sorted(paths):
        if os.path.basename(p) == "secrets.h" or not os.path.isfile(p):
            continue
        h.update(os.path.relpath(p, PROJECT_DIR).encode())
        with open(p, "rb") as f:
            h.update(f.read())
    return h.hexdigest()[:8]


def version_info():
    try:
        sha = git("rev-parse", "--short=8", "HEAD")
    except (OSError, subprocess.CalledProcessError):
        return "0.0.0-nogit", sources_hash(), "unknown", True

    dirty = git("status", "--porcelain") != ""
    date = git("log", "-1", "--format=%cd", "--date=format:%Y-%m-%d %H:%M", "HEAD")
    try:
        desc = git("describe", "--tags", "--match", TAG_PREFIX + "*", "--abbrev=8", "HEAD")
        version = desc[len(TAG_PREFIX):]
    except subprocess.CalledProcessError:
        commits = git("rev-list", "--count", "HEAD")
        version = "0.0.0-dev.%s-g%s" % (commits, sha)
    if dirty:
        version += "-dirty"
    return version, sha, date, dirty


version, sha, date, dirty = version_info()
env.Append(  # noqa: F821
    CPPDEFINES=[
        ("FW_VERSION", env.StringifyMacro(version)),  # noqa: F821
        ("FW_GIT_HASH", env.StringifyMacro(sha)),  # noqa: F821
        ("FW_BUILD_DATE", env.StringifyMacro(date)),  # noqa: F821
        ("FW_DIRTY", 1 if dirty else 0),
    ]
)
print("Firmware version: %s (%s, %s)" % (version, sha, date))
