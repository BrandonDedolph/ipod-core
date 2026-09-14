#!/usr/bin/env python3
"""
release.py — put one version everywhere it is shown, then say what is left.

    tools/.venv/bin/python3 tools/release.py v0.1.2

Does, in order:
  1. rewrites the user guide's opening line ("This guide describes Core vX.Y.Z");
  2. regenerates docs/screens with CORE_STAMP_BUILD_ID / CORE_STAMP_VERSION set
     to the version, so every still and GIF that shows the boot stamp, the About
     chip or the Boot Details header names it;
  3. checks CHANGELOG.md has a "## <version>" section;
  4. greps the markdown for any other "Core v…" / stamp string that still names
     an older version (CHANGELOG.md and STATUS.md are history and are skipped);
  5. prints the remaining steps: commit, tag, `make ipod`, `gh release create`.

The tag must land on the commit that carries the regenerated stills — the
images name the version, so a tag on a later commit would make them lie.
"""
import os
import re
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
    if len(sys.argv) != 2 or not re.fullmatch(r"v\d+\.\d+\.\d+", sys.argv[1]):
        die("usage: release.py vMAJOR.MINOR.PATCH")
    ver = sys.argv[1]

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
""")


if __name__ == "__main__":
    main()
