#!/usr/bin/env python3
"""Install or remove BUG Mod 4.4 (BTS Unaltered Gameplay) for the native build.

  bugmod.py install   --docs DIR [--mode customassets|mod]
  bugmod.py uninstall --docs DIR
  bugmod.py status    --docs DIR

DIR is the game's user folder, normally
~/Library/Application Support/Civ4 BtS Native/Civilization IV Beyond the Sword. Everything goes there; the game
install itself is never modified:
  customassets  Assets -> CustomAssets (always active, also in other people's
                saves); Info, UserSettings, GameSetUpCheck -> "BUG Mod"
  mod           the whole mod -> MODS/BUG Mod 4.4 (Advanced > Load a Mod)
PrivateMaps (map scripts) go to PublicMaps in both modes.

A manifest records every file written and backs up any file overwritten, so
uninstall restores the folder exactly.
"""
import argparse
import hashlib
import json
import os
import shutil
import sys
import tempfile
import urllib.request
import zipfile

URL = ("https://sourceforge.net/projects/civ4bug/files/The%20BUG%20Mod%20%28BTS%20v3.13%2B%29/"
       "The%20BUG%20Mod%204.4/BUGMod_4.4.zip/download")
SHA256 = "c1e115bb1e957872b8b8a7d0812823435ec1f030cea02bf485281118fa3692a2"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "build", "bugmod", "BUGMod_4.4.zip")
MANIFEST = "civ4bts4mac-bugmod.json"  # in DIR
BACKUP = "civ4bts4mac-bugmod-backup"  # in DIR


def progress(msg):
    print(msg, flush=True)


def fetch() -> str:
    if os.path.exists(CACHE) and sha256(CACHE) == SHA256:
        return CACHE
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    progress("Downloading BUG Mod 4.4 from SourceForge...")
    tmp = CACHE + ".part"
    with urllib.request.urlopen(URL, timeout=60) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f)
    got = sha256(tmp)
    if got != SHA256:
        os.remove(tmp)
        sys.exit(f"BUGMod_4.4.zip checksum mismatch: {got}")
    os.replace(tmp, CACHE)
    return CACHE


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def plan(src: str, mode: str):
    """(source path, destination relative to DIR) for every file to install."""
    moves = []
    def add_tree(sub, dest):
        base = os.path.join(src, sub)
        for d, _, files in os.walk(base):
            for f in files:
                p = os.path.join(d, f)
                moves.append((p, os.path.join(dest, os.path.relpath(p, base))))
    if mode == "customassets":
        add_tree("Assets", "CustomAssets")
        for sub in ("Info", "UserSettings", "GameSetUpCheck"):
            add_tree(sub, os.path.join("BUG Mod", sub))
    else:
        for sub in ("Assets", "Info", "UserSettings", "GameSetUpCheck"):
            add_tree(sub, os.path.join("MODS", "BUG Mod 4.4", sub))
        moves.append((os.path.join(src, "Readme.txt"), os.path.join("MODS", "BUG Mod 4.4", "Readme.txt")))
    add_tree("PrivateMaps", "PublicMaps")
    return moves


def find_existing(docs: str, rel: str):
    """The existing path for `rel` under DIR, matching case-insensitively
    (the game creates lowercase folders such as CustomAssets/python)."""
    cur = docs
    for part in rel.split(os.sep):
        if not os.path.isdir(cur):
            return None
        hit = next((e for e in os.listdir(cur) if e.lower() == part.lower()), None)
        if hit is None:
            return None
        cur = os.path.join(cur, hit)
    return cur


def install(docs: str, mode: str):
    if os.path.exists(os.path.join(docs, MANIFEST)):
        uninstall(docs)
    os.makedirs(docs, exist_ok=True)
    zpath = fetch()
    with tempfile.TemporaryDirectory() as tmp:
        with zipfile.ZipFile(zpath) as z:
            z.extractall(tmp)
        written, backed_up, created = [], [], []
        backup_root = os.path.join(docs, BACKUP)
        for src, rel in plan(tmp, mode):
            existing = find_existing(docs, rel)
            dest = existing or os.path.join(docs, rel)
            if existing:
                # Keep the user's own file; restore it on uninstall.
                bpath = os.path.join(backup_root, os.path.relpath(existing, docs))
                os.makedirs(os.path.dirname(bpath), exist_ok=True)
                shutil.copy2(existing, bpath)
                backed_up.append(os.path.relpath(existing, docs))
            # Note the folders this install creates (only those are removed again).
            d = os.path.dirname(dest)
            while len(d) > len(docs) and not os.path.exists(d):
                created.append(os.path.relpath(d, docs))
                d = os.path.dirname(d)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.copy2(src, dest)
            written.append(os.path.relpath(dest, docs))
    with open(os.path.join(docs, MANIFEST), "w") as f:
        json.dump({"version": "4.4", "mode": mode, "files": written, "backed_up": backed_up,
                   "created_dirs": created}, f, indent=1)
    progress(f"Installed BUG Mod 4.4 ({mode}): {len(written)} files")


def uninstall(docs: str):
    mpath = os.path.join(docs, MANIFEST)
    if not os.path.exists(mpath):
        progress("BUG Mod is not installed (no manifest)")
        return
    with open(mpath) as f:
        m = json.load(f)
    for rel in m["files"]:
        p = os.path.join(docs, rel)
        if os.path.exists(p):
            os.remove(p)
    backup_root = os.path.join(docs, BACKUP)
    for rel in m["backed_up"]:
        b = os.path.join(backup_root, rel)
        if os.path.exists(b):
            os.makedirs(os.path.dirname(os.path.join(docs, rel)), exist_ok=True)
            shutil.move(b, os.path.join(docs, rel))
    # Remove the folders the install created, if nothing else is in them now.
    for rel in sorted(m.get("created_dirs", []), key=len, reverse=True):
        d = os.path.join(docs, rel)
        if os.path.isdir(d) and not os.listdir(d):
            os.rmdir(d)
    shutil.rmtree(backup_root, ignore_errors=True)
    os.remove(mpath)
    progress(f"Removed BUG Mod ({m['mode']}): {len(m['files'])} files, {len(m['backed_up'])} restored")


def status(docs: str):
    mpath = os.path.join(docs, MANIFEST)
    if os.path.exists(mpath):
        with open(mpath) as f:
            m = json.load(f)
        print(json.dumps({"installed": True, "mode": m["mode"], "version": m["version"]}))
    else:
        print(json.dumps({"installed": False}))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["install", "uninstall", "status"])
    ap.add_argument("--docs", default=os.path.expanduser("~/Library/Application Support/Civ4 BtS Native/Civilization IV Beyond the Sword"))
    ap.add_argument("--mode", choices=["customassets", "mod"], default="customassets")
    a = ap.parse_args()
    {"install": lambda: install(a.docs, a.mode), "uninstall": lambda: uninstall(a.docs),
     "status": lambda: status(a.docs)}[a.action]()


if __name__ == "__main__":
    main()
