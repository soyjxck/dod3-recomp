#!/usr/bin/env python3
"""Lay out Drakengard 3's game-data install without running the installer.

    python tools/preinstall_gamedata.py [game/disc] [--copy] [--force]

On first boot the title copies COOKEDSOUND (399 MB, 5,424 files) and
COOKEDPS3 (4.7 GB, 3,259 files) from the disc into
/dev_hdd0/game/BLES00000DATA/USRDIR/FIOS-UNREALENGINE3/SQEX03GAME/, one
step per frame behind a progress dialog: about eight minutes. The files are
byte-for-byte copies under the same names, and cellGameDataCheck only asks
whether the directory exists, so a host-side layout is all it takes:

  <root>/game/BLES00000DATA/ICON0.PNG                   copied from PS3_GAME
  <root>/game/BLES00000DATA/USRDIR/FIOS-UNREALENGINE3/SQEX03GAME/COOKEDSOUND
  <root>/game/BLES00000DATA/USRDIR/FIOS-UNREALENGINE3/SQEX03GAME/COOKEDPS3

The two folders are directory junctions (Windows) or symlinks (elsewhere)
onto the disc's own folders, so nothing is copied; --copy copies instead.
--force replaces a partial tree left by a killed install (such a tree makes
the title skip the install and then fail to find its sound files).
"""
import os
import shutil
import sys

TITLE_DIR = "BLES00000DATA"
FOLDERS = ("COOKEDSOUND", "COOKEDPS3")


def link_dir(src, dst):
    if sys.platform == "win32":
        import _winapi
        _winapi.CreateJunction(os.path.abspath(src), os.path.abspath(dst))
    else:
        os.symlink(os.path.abspath(src), dst)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    root = args[0] if args else "game/disc"
    copy = "--copy" in sys.argv
    force = "--force" in sys.argv
    disc_game = os.path.join(root, "PS3_GAME", "USRDIR", "SQEX03GAME")
    if not os.path.isdir(disc_game):
        print(f"no disc at {root} ({disc_game} missing)")
        return 1
    data = os.path.join(root, "game", TITLE_DIR)
    target = os.path.join(data, "USRDIR", "FIOS-UNREALENGINE3", "SQEX03GAME")
    if os.path.isdir(data):
        if not force:
            print(f"{data} exists; --force replaces it")
            return 1
        # A junction must be removed as a directory, not with rmtree through it.
        for f in FOLDERS:
            p = os.path.join(target, f)
            if os.path.isdir(p) and (os.path.islink(p) or (sys.platform == "win32" and os.lstat(p).st_file_attributes & 0x400)):
                os.rmdir(p)
        shutil.rmtree(data)
    os.makedirs(target)
    shutil.copyfile(os.path.join(root, "PS3_GAME", "ICON0.PNG"), os.path.join(data, "ICON0.PNG"))
    for f in FOLDERS:
        src, dst = os.path.join(disc_game, f), os.path.join(target, f)
        if copy:
            shutil.copytree(src, dst)
        else:
            link_dir(src, dst)
        print(f"{dst} -> {src} ({'copied' if copy else 'junction'})")
    print("game data laid out; the title will skip its install")
    return 0


if __name__ == "__main__":
    sys.exit(main())
