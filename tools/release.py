#!/usr/bin/env python3
"""
release.py — put one version everywhere it is shown, then say what is left.

    tools/.venv/bin/python3 tools/release.py v0.1.2
    tools/.venv/bin/python3 tools/release.py v0.1.2 --check   # assets only

Does, in order:
  1. rewrites the user guide's opening line ("This guide describes Core vX.Y.Z");
  2. regenerates docs/screens with CORE_STAMP_BUILD_ID / CORE_STAMP_VERSION set
     to the version, so every still and GIF that shows the boot stamp, the About
     chip or the Boot Details header names it;
  3. checks CHANGELOG.md has a "## <version>" section;
  4. greps the markdown for any other "Core v…" / stamp string that still names
     an older version (CHANGELOG.md and STATUS.md are history and are skipped);
  5. prints the remaining steps: commit, tag, `make ipod`, `gh release create`;
  6. asks the release what it is actually carrying — eleven assets are
     expected: core.ipod from the human flow, the five `core` CLI binaries
     and the five `core-app` desktop binaries that
     .github/workflows/release.yml attaches on the tag push — and says which
     are present and which are missing. `--check` runs only that step, which
     is what you want a few minutes after tagging, when CI has had time to
     finish. Nothing here uploads the binaries: CI does, from the tag, with
     the same script CI builds them with on every push.

The tag must land on the commit that carries the regenerated stills — the
images name the version, so a tag on a later commit would make them lie.
"""
import os
import re
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUIDE = os.path.join(REPO, "docs", "USER_GUIDE.md")
CHANGELOG = os.path.join(REPO, "CHANGELOG.md")
RENDER = os.path.join(REPO, "docs", "screens", "render.py")
PYTHON = os.path.join(REPO, "tools", ".venv", "bin", "python3")


def die(msg):
    sys.exit(f"release.py: {msg}")


def main():
    args = sys.argv[1:]
    check_only = "--check" in args
    args = [a for a in args if a != "--check"]
    if len(args) != 1 or not re.fullmatch(r"v\d+\.\d+\.\d+", args[0]):
        die("usage: release.py vMAJOR.MINOR.PATCH [--check]\n"
            "       --check: only list the release's assets, change nothing")
    ver = args[0]

    # --check touches no file: it is the "did CI attach the binaries?" run.
    if check_only:
        check_assets(ver, required=True)
        return

    # 1. the guide's version line
    src = open(GUIDE, encoding="utf-8").read()
    new, n = re.subn(r"This guide describes Core v\d+\.\d+\.\d+", f"This guide describes Core {ver}", src)
    if n != 1:
        die("docs/USER_GUIDE.md: expected exactly one 'This guide describes Core vX.Y.Z' line")
    if new != src:
        open(GUIDE, "w", encoding="utf-8").write(new)
        print(f"guide: version line -> {ver}")
    else:
        print(f"guide: already {ver}")

    # 2. the gallery, stamped
    env = dict(os.environ, CORE_STAMP_BUILD_ID=ver, CORE_STAMP_VERSION=ver)
    py = PYTHON if os.path.exists(PYTHON) else sys.executable
    print("gallery: regenerating with the stamp set …")
    r = subprocess.run([py, RENDER], cwd=REPO, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        die(f"render.py failed:\n{r.stderr[-2000:]}")
    changed = subprocess.run(["git", "status", "--short", "docs/screens"], cwd=REPO,
                             capture_output=True, text=True).stdout.strip()
    print("gallery: " + (changed.replace("\n", "\n         ") if changed else "no still changed"))

    # 3. the changelog section
    if f"## {ver} " not in open(CHANGELOG, encoding="utf-8").read():
        print(f"CHANGELOG.md: NO '## {ver} — <date>' section yet — write it before tagging")

    # 4. anything else still naming an older version
    stale = []
    for root, _dirs, files in os.walk(REPO):
        if any(p in root for p in ("/.git", "/build", "/tools/.venv", "/node_modules")):
            continue
        for f in files:
            if not f.endswith(".md") or f in ("CHANGELOG.md", "STATUS.md"):
                continue
            p = os.path.join(root, f)
            for i, line in enumerate(open(p, encoding="utf-8"), 1):
                for m in re.finditer(r"Core v(\d+\.\d+\.\d+)", line):
                    if "v" + m.group(1) != ver:
                        stale.append(f"{os.path.relpath(p, REPO)}:{i}: {line.strip()[:100]}")
    if stale:
        print("still naming another version (fix or confirm these are examples):")
        for s in stale:
            print("  " + s)
    else:
        print("markdown: no other 'Core vX.Y.Z' reference disagrees")

    print(f"""
next:
  git add -A && git commit
  git tag -a {ver} -m "Core {ver}"
  (cd core && make ipod)            # from the tagged tree: the stamp reads {ver}
  git push origin main {ver}
  gh release create {ver} core/build-hw/core.ipod --title "Core {ver}" --notes-file <the CHANGELOG section>

  # the ten core-* / core-app-* binaries are NOT uploaded by hand — the tag
  # push starts .github/workflows/release.yml, which builds them on three
  # runners (ubuntu for the CLI and the Windows GUI, ubuntu-24.04-arm and
  # macos-latest for the cgo GUI builds) and uploads them onto this release
  # itself. Give it a few minutes, then:
  tools/release.py {ver} --check    # lists which of the eleven assets landed
""")

    check_assets(ver)


# The ten binaries .github/workflows/release.yml attaches on the tag push:
# five `core` CLI binaries, all cross-compiled on one Linux runner because
# the CLI is pure Go, and five `core-app` desktop binaries, which are not —
# Gio's Windows backend is pure Go and cross-builds with the CLI, but its
# X11 and Cocoa backends are cgo and are built on their own runners.
CLI_BINARIES = [
    "core-linux-amd64",
    "core-linux-arm64",
    "core-darwin-amd64",
    "core-darwin-arm64",
    "core-windows-amd64.exe",
]

APP_BINARIES = [
    "core-app-linux-amd64",
    "core-app-linux-arm64",
    "core-app-darwin-amd64",
    "core-app-darwin-arm64",
    "core-app-windows-amd64.exe",
]

BINARIES = CLI_BINARIES + APP_BINARIES


def expected(ver):
    """The eleven assets a finished release carries, as (slot, accepted names).

    The firmware image is the locally built, locally flashed one — CI's pinned
    gcc 14 would not reproduce the gcc 16 binary that was verified on the
    device, so nothing uploads it but a human. v0.1.0-v0.1.2 attached it as
    `core-vX.Y.Z.ipod`; the updater in the companion-app plan looks for a
    stable `core.ipod`. Until that is settled, either name fills the slot.
    """
    return [("firmware image", [f"core-{ver}.ipod", "core.ipod"])] + \
           [(name, [name]) for name in BINARIES]


def check_assets(ver, required=False):
    """Print which of the eleven expected assets are on the release.

    Advisory during a normal run — at that point the tag usually does not
    exist yet, and a missing release is the expected state, not an error.
    With --check (required=True) it is the whole point of the invocation, so
    a missing gh or a missing release exits non-zero.
    """
    def missing(msg):
        print(f"assets: {msg}")
        if required:
            sys.exit(1)
        return None

    if not shutil.which("gh"):
        return missing("gh not on PATH — cannot list the release's assets")
    r = subprocess.run(["gh", "release", "view", ver, "--json", "assets",
                        "-q", ".assets[].name"],
                       cwd=REPO, capture_output=True, text=True)
    if r.returncode != 0:
        return missing(f"no release {ver} yet"
                       + (f" ({r.stderr.strip().splitlines()[-1]})" if r.stderr.strip() else ""))

    have = set(r.stdout.split())
    slots = expected(ver)
    matched, gone = set(), []
    print(f"\nassets on {ver}:")
    for slot, names in slots:
        hit = next((n for n in names if n in have), None)
        if hit:
            matched.add(hit)
            print(f"  present  {hit}")
        else:
            gone.append(slot)
            print(f"  MISSING  {' or '.join(names)}")
    for extra in sorted(have - matched):
        print(f"  extra    {extra}")

    if gone:
        print(f"{len(gone)} of {len(slots)} missing: {', '.join(gone)}")
        if "firmware image" in gone:
            print(f"  firmware image  -> gh release upload {ver} "
                  f"core/build-hw/core.ipod --clobber   (built locally, never by CI)")
        if any(g in BINARIES for g in gone):
            print("  core-* binaries -> .github/workflows/release.yml attaches these; "
                  "check `gh run list --workflow=Release`")
        if "core-app-linux-arm64" in gone:
            print("  core-app-linux-arm64 -> its job runs on ubuntu-24.04-arm and is "
                  "continue-on-error; that runner label is not on every plan")
        if any(g.startswith("core-app-darwin") for g in gone):
            print("  core-app-darwin-*    -> built on macos-latest (Cocoa is cgo); "
                  "check that job in the same run")
        if required:
            sys.exit(1)
    else:
        print(f"all {len(slots)} expected assets are attached")


if __name__ == "__main__":
    main()
