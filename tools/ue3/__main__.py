"""The UnrealScript tools, on the game's cooked packages:

    python tools/ue3 dis  <package> <Class> [Function]
        disassemble a class's UnrealScript functions, e.g.
        game/disc/PS3_GAME/USRDIR/SQEX03GAME/COOKEDPS3/SQEX03GAME.XXX Sqex03GameHUDOptionRoot
    python tools/ue3 diff <dir a> <dir b> [-v] [PACKAGE ...]
        what changed in the scripts of two builds, e.g. the disc's COOKEDPS3
        (1.00) against the 1.01 update's PATCH/SQEX03GAME/COOKEDPS3
    python tools/ue3 info <package> [exports]
        the package's tables: version, name count, the first exports

tools/menu_patch.py builds the settings-menu patch on the same package code.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ue3 import diff, dis, upk  # noqa: E402


def info(argv):
    u = upk.decompress(argv[1])
    s, names, imps, exps = upk.tables(u)
    print('ver', s['ver'], 'names', len(names), 'imports', len(imps), 'exports', len(exps), 'size', len(u))
    for i, e in enumerate(exps[:int(argv[2]) if len(argv) > 2 else 40]):
        print(i + 1, upk.objname(e['cls'], imps, exps), e['name'], 'outer', upk.objname(e['outer'], imps, exps),
              e['size'], hex(e['off']))


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    sys.argv = [sys.argv[0] + ' ' + cmd] + sys.argv[2:]
    if cmd == 'dis' and len(sys.argv) >= 3:
        dis.main()
    elif cmd == 'diff' and len(sys.argv) >= 3:
        diff.main()
    elif cmd == 'info' and len(sys.argv) >= 2:
        info(sys.argv)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
