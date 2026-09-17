#!/usr/bin/env python3
"""Assemble github_amiga68k_bobrhopper/ - the public repository of the Amiga port
(https://github.com/angree/Amiga-68K-BobrHopper).

    python tools/make_amiga_repo.py [--out github_amiga68k_bobrhopper]

A REGENERATOR, like tools/make_github_repo.py for the SF2000 and R36S ports: it keeps .git and release/,
rewrites the author's absolute paths, and refreshes everything else from the working tree.

README.md comes from docs/amiga/README_GITHUB.md - the text the user edited and approved. When that file does not
exist yet, a README.md already in the export is left exactly as it is (it may be open in an editor).

What stays out, on purpose:
  - src/amiga/cgx-include/   CyberGraphX developer headers: (c) phase5, not redistributable
  - docs/*_AMIGA.md          the working notes (hand-off, progress, tasks): they are a diary, not documentation
  - tools/sound_studio.py    serves takes from the author's own disk
  - the upstream archive     the Amiga data is shipped already baked in data_amiga/
"""
import argparse
import os
import shutil

FILES = [
    "src/amiga",
    "src/game",
    "src/engine",
    "src/ui",
    "src/sw",                               # the software renderer the sprite baker draws with
    "src/sf2000/platform_paths_sf2000.cpp", # the baker links it (sources.sh)
    "apps/sw_bake_amiga.cpp",
    "build/build_amiga.sh",
    "build/package_amiga.sh",
    "build/amiga",
    "build/build_pc.sh",
    "build/sources.sh",
    "build/setup_tools.sh",
    "tools/make_amiga_meta.py",
    "tools/make_amiga_audio.py",
    "tools/make_amiga_music.py",
    "tools/make_amiga_logo.py",
    "tools/make_amiga_font.py",
    "tools/make_amiga_icon.py",
    "tools/pack_amiga_sprites.py",
    "tools/raw2png_amiga.py",
    "winuae",
    "data_amiga",
    "docs/RANGI.txt",
    "docs/amiga",
]

EXCLUDE = {
    "src/amiga/cgx-include",
    "docs/amiga/README_GITHUB.md",  # becomes README.md at the top
    "winuae/harness/run_bh.ps1",    # the old harness restarted the emulator for every run
}

# The author's machine must not leak into a public tree. Order matters: the specific lines first.
PATH_FIXES = [
    ("REPO=/mnt/i/GITHUB/CrossyRoads", 'REPO=$(cd "$(dirname "$0")/.." && pwd)'),
    ("ls /mnt/i/GITHUB >/dev/null 2>&1 || sudo -n mount -t drvfs I: /mnt/i; ", ""),
    ("ls /mnt/i/GITHUB >/dev/null 2>&1 || sudo -n mount -t drvfs I: /mnt/i", ":"),
    ("/mnt/i/GITHUB/CrossyRoads", "$REPO"),
    ("I:\\GITHUB\\CrossyRoads", "."),
    ("I:/GITHUB/CrossyRoads", "."),
    ("Adapted from I:\\GITHUB\\Amiga_OpenXCOM\\winuae\\oxc-aga.uae.", "Adapted from the author's Amiga OpenXcom test machine."),
    ("the \\\\synologynas UNC path", "a network (UNC) path"),
    ("CrossyRoads/tools", "BobrHopper/tools"),
    ("CrossyRoads\\tools", "BobrHopper\\tools"),
]

TEXT_EXT = {".sh", ".py", ".md", ".c", ".h", ".cpp", ".uae", ".ps1", ".txt", ""}

LICENSE = """MIT License

Copyright (c) 2016-present Evan Bacon
Copyright (c) 2026 G. Korycki

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
"""

NOTICE = """# Notice

Both copyright holders in [LICENSE](LICENSE) release their work under the same MIT licence. This file says which
part is whose, and what is somebody else's.

**Evan Bacon** - [expo-crossy-road](https://github.com/EvanBacon/expo-crossy-road), the game this port is derived
from: its structure and its logic. The models the sprites were baked from come from it.

**G. Korycki** - this Amiga port: the C and C++ sources, the AmigaOS display, Paula audio and ADPCM streaming layers
(first written for the author's native AmigaOS port of OpenTTD, and the author's own code), the sprite baker, the
data tools, the beaver, the logo, the icon, the soundtrack and every sound effect.

## Third-party code

- `src/amiga/c2p_rect.s`, `src/amiga/c2p1x1_6_c5_bm_040.s` - chunky-to-planar routines by **Mikael Kalms**
  ([kalms-c2p](https://github.com/Kalmalyzer/kalms-c2p)), used as published.
- **Not included:** the CyberGraphX developer headers the RTG code is compiled against (`src/amiga/cgx-include/`).
  They are phase5's and not redistributable; see README.md.

## Audio

No audio from the original project is included. Every sound and music track was made for this port.

## Trademarks

This project is not affiliated with, endorsed by, or connected to Hipster Whale, Yodo1, or the "Crossy Road" game
or trademark.
"""

GITIGNORE = """# build output
/out/
*.o

# release archives are attached to GitHub releases, not committed
/release/*
!/release/README.md

# CyberGraphX developer headers: (c) phase5, not redistributable - supply your own
/src/amiga/cgx-include/

# emulator and test leftovers
winuaelog.txt
*.raw
*.pal
bh.log

__pycache__/
*.pyc
"""

GITATTRIBUTES = """# Shell scripts and AmigaDOS scripts must keep LF line endings: a CR breaks sh, and on the Amiga a CR becomes
# part of the last word of the line (a file name, a command).
*.sh text eol=lf
winuae/work/* text eol=lf
*.uae text eol=lf
"""

RELEASE_README = """# release/

Built packages land here; nothing in this folder is committed (see `.gitignore`).

    sh build/package_amiga.sh
    cp out/amiga/BobrHopper-Amiga-v011.zip release/

Attach the zip to a GitHub release.
"""


def rel_of(repo, path):
    return os.path.relpath(path, repo).replace("\\", "/")


def excluded(rel):
    return any(rel == e or rel.startswith(e + "/") for e in EXCLUDE) or "/__pycache__/" in "/" + rel + "/"


def copy_file(repo, out, rel):
    if excluded(rel):
        return 0
    src = os.path.join(repo, rel)
    dst = os.path.join(out, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.splitext(src)[1].lower() in TEXT_EXT and os.path.getsize(src) < 2000000:
        try:
            with open(src, encoding="utf-8", newline="") as f:
                text = f.read()
        except UnicodeDecodeError:
            shutil.copy2(src, dst)
            return 1
        fixed = text
        for a, b in PATH_FIXES:
            fixed = fixed.replace(a, b)
        with open(dst, "w", encoding="utf-8", newline="") as f:
            f.write(fixed)
    else:
        shutil.copy2(src, dst)
    return 1


def copy_entry(repo, out, rel):
    src = os.path.join(repo, rel)
    if not os.path.exists(src):
        print("  MISSING: %s" % rel)
        return 0
    if not os.path.isdir(src):
        return copy_file(repo, out, rel)
    count = 0
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d not in ("__pycache__", ".git") and not excluded(rel_of(repo, os.path.join(root, d)))]
        for name in files:
            count += copy_file(repo, out, rel_of(repo, os.path.join(root, name)))
    return count


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="github_amiga68k_bobrhopper")
    ap.add_argument("--repo", default=".")
    args = ap.parse_args()
    repo = os.path.abspath(args.repo)
    out = os.path.abspath(args.out)
    readme_src = os.path.join(repo, "docs", "amiga", "README_GITHUB.md")
    keep_readme = not os.path.exists(readme_src)

    if os.path.isdir(out):
        for name in os.listdir(out):
            if name in (".git", "release") or (keep_readme and name == "README.md"):
                continue
            path = os.path.join(out, name)
            shutil.rmtree(path) if os.path.isdir(path) else os.remove(path)
    os.makedirs(os.path.join(out, "release"), exist_ok=True)

    total = 0
    for rel in FILES:
        total += copy_entry(repo, out, rel)

    if not keep_readme:
        shutil.copy2(readme_src, os.path.join(out, "README.md"))
    for name, text in (("LICENSE", LICENSE), ("NOTICE.md", NOTICE), (".gitignore", GITIGNORE), (".gitattributes", GITATTRIBUTES),
                       ("release/README.md", RELEASE_README)):
        with open(os.path.join(out, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    # nothing of the author's machine or of the private headers may be left in the tree
    leaks = []
    for root, dirs, files in os.walk(out):
        dirs[:] = [d for d in dirs if d != ".git"]
        for name in files:
            p = os.path.join(root, name)
            if os.path.getsize(p) > 2000000:
                continue
            try:
                t = open(p, encoding="utf-8").read()
            except (UnicodeDecodeError, OSError):
                continue
            for bad in ("/mnt/i/", "I:\\GITHUB", "I:/GITHUB", "synologynas", "GNU General Public"):
                if bad in t:
                    leaks.append("%s: %s" % (rel_of(out, p), bad))
    if os.path.isdir(os.path.join(out, "src", "amiga", "cgx-include")):
        leaks.append("src/amiga/cgx-include is present")
    print("%d files -> %s%s" % (total, out, "  (README.md left as it is)" if keep_readme else ""))
    if leaks:
        print("LEAKS:")
        for l in leaks:
            print("  " + l)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
