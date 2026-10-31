#!/usr/bin/env python3
"""
web_manifest.py - the list of files in a game folder, for the web host.

    python3 tools/web_manifest.py GAME_DIR [--out GAME_DIR/manifest.json]

A web server cannot list a directory, so the page's game folder carries a
manifest.json naming every file in it (paths relative to the folder, forward
slashes, the case the files have on disk). src/web/web.c builds WASMFS's tree
from it and fetches each file as the game opens it.

Only what the game reads goes in: thotd.exe, the data folders (ART, CAM, EVT,
SOUND, SPRITE and the rest), the .PAL/.TBL/.CFG files. Installers, DirectX
redistributables and ripped music (MUSIC/, played by the page) are left out.
"""
import argparse, json, os, sys

SKIP_DIRS = {'music', 'directx', 'dx', 'install', 'setup', '__macosx'}
SKIP_EXT = {'.lnk', '.url', '.msi', '.cab', '.hdr', '.inf', '.ini', '.log', '.txt', '.doc', '.htm', '.html', '.pdf', '.wav', '.ogg', '.mp3', '.zip', '.7z', '.rar'}
KEEP_NAMES = {'thotd.exe'}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('game')
    ap.add_argument('--out')
    a = ap.parse_args()
    root = os.path.abspath(a.game)
    files, total = [], 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d.lower() not in SKIP_DIRS and not d.startswith('.'))
        for fn in sorted(filenames):
            low = fn.lower()
            ext = os.path.splitext(low)[1]
            if fn.startswith('.') or low == 'manifest.json': continue
            if low not in KEEP_NAMES and (ext in SKIP_EXT or ext in ('.exe', '.dll')): continue
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, root).replace(os.sep, '/')
            files.append(rel)
            total += os.path.getsize(full)
    if 'thotd.exe' not in [f.lower() for f in files]:
        sys.exit('no thotd.exe in %s' % root)
    out = a.out or os.path.join(root, 'manifest.json')
    with open(out, 'w') as f:
        json.dump({'files': files}, f, indent=0)
    print('%s: %d files, %d MB' % (out, len(files), total // 1000000))


if __name__ == '__main__':
    main()
