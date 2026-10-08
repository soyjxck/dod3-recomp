"""python tools/ue3 diff <dir a> <dir b> [-v] [PACKAGE ...]: what changed in the
UnrealScript of two builds -- e.g. the disc's COOKEDPS3 (1.00) against the
1.01 update's PATCH/SQEX03GAME/COOKEDPS3.

Per script package (default: the eight the 1.01 update replaces): exports
only in one build (classes, properties, functions, states), and every
function whose disassembly differs. The disassembly is compared without its
code offsets, and each build is read with its own native-function names,
so a function that only moved or whose package indices shifted does not
count as changed. -v prints a unified diff of each changed function.
"""
import difflib
import os
import sys

from . import dis, upk

PACKAGES = ['CORE.XXX', 'ENGINE.XXX', 'GAMEFRAMEWORK.XXX', 'GFXUI.XXX', 'IPDRV.XXX',
            'ONLINESUBSYSTEMPC.XXX', 'SQEXSEAD.XXX', 'SQEX03GAME.XXX']
NATIVE_SET = ('CORE.XXX', 'ENGINE.XXX', 'GAMEFRAMEWORK.XXX', 'GFXUI.XXX', 'SQEX03GAME.XXX', 'SQEXSEAD.XXX')

def path_of(d, name):
    p = os.path.join(d, name)
    if os.path.exists(p): return p
    for n in os.listdir(d):
        if n.upper() == name.upper(): return os.path.join(d, n)
    return None

def read(d, pkg):
    dis.NATIVE_NAMES.clear()
    dis.load_natives([p for p in (path_of(d, n) for n in NATIVE_SET) if p])
    u = upk.decompress(path_of(d, pkg))
    s, names, imps, exps = upk.tables(u)
    objs, funcs = set(), {}
    for e in exps:
        cls = upk.objname(e['cls'], imps, exps)
        key = '%s.%s' % (upk.objname(e['outer'], imps, exps), e['name'])
        objs.add('%s %s' % (cls, key))
        if cls != 'Function': continue
        b = u[e['off']:e['off'] + e['size']]
        try:
            nxt, sup, ch, msz, ssz, code, native, flags = dis.func_layout(b)
        except Exception:
            continue
        dz = dis.Dis(b, names, imps, exps)
        dz.p = code
        dz.run(code + ssz)
        funcs[key] = ['flags 0x%08x native %d' % (flags, native)] + [l.split(': ', 1)[-1] for l in dz.out]
    return objs, funcs

def main():
    a, b = sys.argv[1], sys.argv[2]
    verbose = '-v' in sys.argv
    pkgs = [x for x in sys.argv[3:] if not x.startswith('-')] or PACKAGES
    for pkg in pkgs:
        if not path_of(a, pkg) or not path_of(b, pkg):
            print('== %s: not in both' % pkg); continue
        oa, fa = read(a, pkg)
        ob, fb = read(b, pkg)
        changed = sorted(k for k in fa if k in fb and fa[k] != fb[k])
        print('== %s: %d functions, %d changed; %d exports only in a, %d only in b'
              % (pkg, len(fb), len(changed), len(oa - ob), len(ob - oa)))
        for x in sorted(oa - ob): print('   - %s' % x)
        for x in sorted(ob - oa): print('   + %s' % x)
        for k in changed:
            print('   ~ %s (%d -> %d statements)' % (k, len(fa[k]), len(fb[k])))
            if verbose:
                for l in list(difflib.unified_diff(fa[k], fb[k], lineterm='', n=1))[2:]:
                    print('       ' + l)
