"""Generate the Graphics Settings patch for SQEX03GAME.XXX.

menu_patch.py [<overlay dir>] [--header=src/dod3_menu_patch_data.h] [--show]

The overlay is for testing (PS3_VFS_OVERLAY=<dir> and DOD3_SHA_OVERRIDE from
<dir>/sha1.txt); the header is what dod3 applies to the player's own copy.

Settings root: a fifth entry, "Graphics Settings", in the empty row between
Audio Settings and Restore Defaults; it opens the title's unused Display page
(Sqex03GameHUDOptionDisplay, layout HUD_Pause.menu.select_option_display),
rewritten as a list of rows whose text and values come from the port:

  GetString(MAGIC + k)            text the port supplies (see below)
  m_xOption.UpdateDisplayParam(cmd, a, b) -> int
                                  the port's settings bridge (the native's
                                  only caller was this page)
Text: MAGIC+0 root label, MAGIC+1 root description, MAGIC+100+row row label,
MAGIC+200+row value, MAGIC+300+row row description.
Bridge: 0 begin (pending = current), 1 change(row, dir), 2 is-default(row),
3 reset (pending = defaults), 4 apply, 5 changed?
"""
import sys, os, hashlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ue3 import upk, script as ue3script, dis as disasm, pkgpatch
from ue3.build import Pkg, clone, subst, remove, findnode, inner_call
from ue3.script import Node

MAGIC = 900000
ROWS = 7
CMD_BEGIN, CMD_CHANGE, CMD_ISDEF, CMD_RESET, CMD_APPLY, CMD_CHANGED = range(6)

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'game', 'disc', 'PS3_GAME', 'USRDIR', 'SQEX03GAME')
SRC = ROOT + '/COOKEDPS3/SQEX03GAME.XXX'
ORIG_SHA1 = '70e1e44a648eaf8dee338eaf439c69c97d1c0d71'   # the EBOOT's table entry

def main():
    args = [x for x in sys.argv[1:] if not x.startswith('--')]
    out = args[0] if args else None
    show = '--show' in sys.argv
    header = None
    for x in sys.argv[1:]:
        if x.startswith('--header='): header = x[9:]
    disasm.load_natives([ROOT + '/COOKEDPS3/' + n for n in ('CORE.XXX', 'ENGINE.XXX', 'GAMEFRAMEWORK.XXX', 'SQEX03GAME.XXX')])
    u = upk.decompress(SRC)
    assert hashlib.sha1(u).hexdigest() == ORIG_SHA1, 'not the BLUS31197 1.00 script package'
    pk = Pkg(u)
    new = {}

    OPT, ROOTC, DISP = 'Sqex03GameHUDOption', 'Sqex03GameHUDOptionRoot', 'Sqex03GameHUDOptionDisplay'

    # ---- Sqex03GameHUDOption.Initialize: a fifth child, the Display page ----
    f = pk.func(OPT, 'Initialize'); st = pk.parse(f)
    i = pk.find(st, 'self.m_xChild.Add(4)')
    subst(st[i], lambda n: n.op == 0x2C and n.parts == [('u8', 4)], lambda n: pk.int_(5))
    i = pk.find(st, 'self.m_xChild[3] = new(nothing, nothing, nothing, obj(Sqex03GameHUDOptionSound), nothing)')
    s4 = clone(st[i])
    subst(s4, lambda n: n.op == 0x2C and n.parts == [('u8', 3)], lambda n: pk.int_(4))
    disp_cls = pk.export(DISP, 'None', 'None')   # a UClass export has no class index
    subst(s4, lambda n: n.op == 0x20, lambda n: Node(0x20, [('obj', disp_cls)]))
    st.insert(i + 1, s4)
    new[f] = st

    # ---- Sqex03GameHUDOption.Update: the reset request moves from 4 to 7 ----
    f = pk.func(OPT, 'Update'); st = pk.parse(f)
    i = pk.find(st, 'if !(EqualEqual_IntInt(self.m_iNextType, 4)) goto 0x01c1')
    subst(st[i], lambda n: n.op == 0x2C and n.parts == [('u8', 4)], lambda n: pk.int_(7))
    new[f] = st

    # ---- Sqex03GameHUDOption.Draw: five rows; row 3 is ours ----
    # The layout's empty row is one row high (Audio at y 286, Restore at 358)
    # but its own sprites (11, 12, text 50) are a narrower, indented bar from
    # some earlier design; row 3 is drawn with Audio's (9, 10, text 49) one row
    # (36) lower instead -- Sqex03Menu.Draw takes an offset.
    f = pk.func(OPT, 'Draw'); st = pk.parse(f)
    I = pk.local(f, 'I')
    i = pk.find(st, 'if !(Less_IntInt(I, 4)) goto 0x03ce')
    subst(st[i], lambda n: n.op == 0x2C and n.parts == [('u8', 4)], lambda n: pk.int_(5))
    i_lt3 = pk.find(st, 'if !(Less_IntInt(I, 3)) goto 0x0250')
    d0 = pk.find(st, 'self.m_xMenu.Draw(Add_IntInt(5, Multiply_IntInt(2, I)), self.m_xHUD.self.Canvas, <empty>, <empty>, <empty>, <empty>, <empty>, <empty>)')
    gp = pk.find(st, 'xParam = self.m_xMenu.GetFrameAnimParam(Add_IntInt(47, I), <empty>)')
    fa = pk.find(st, 'fAlpha = Divide_FloatFloat(cast60(xParam.m_bColA), 255f)', 2)[0]
    tx = pk.find(st, 'sText = self.m_MessageData.GetString(Add_IntInt(72, I))')
    jd = pk.find(st, 'goto 0x0360')
    ds = pk.find(st, 'DrawString(sText, xParam.m_fPosX, xParam.m_fPosY, <empty>, <empty>, false, fAlpha, b0, 14f, <empty>, <empty>)')
    posy = findnode(st[ds], 'xParam.m_fPosY')
    def bar(k):
        n = clone(st[d0])
        c = inner_call(n)
        args = [x for x in c.parts if x[0] == 'e']
        args[0] = ('e', pk.int_(k))
        args[3] = ('e', pk.float_(36.0))
        c.parts = c.parts[:1] + args + [('u8', 0x16)]
        return n
    g_if = Node(0x07, [('jmp', st[ds - 5].mem), ('e', pk.native('EqualEqual_IntInt', clone(I), pk.int_(3)))])
    g_if.label = ['row3']
    g_gp = clone(st[gp]); subst(g_gp, lambda n: pk.text(n) == 'Add_IntInt(47, I)', lambda n: pk.int_(49))
    g_y = pk.let(clone(posy), pk.native('Add_FloatFloat', clone(posy), pk.float_(36.0)))
    g_tx = clone(st[tx]); subst(g_tx, lambda n: pk.text(n) == 'Add_IntInt(72, I)', lambda n: pk.int_(MAGIC + 0))
    g_jd = clone(st[jd])
    # row 3 goes before the Restore Defaults branch (the statement the `I < 3` test jumped to)
    else_at = ds - 5
    assert pk.text(st[else_at]).startswith('self.m_xMenu.Draw(13,'), pk.text(st[else_at])
    st[else_at:else_at] = [g_if, bar(9), bar(10), g_gp, g_y, clone(st[fa]), g_tx, g_jd]
    st[i_lt3].parts[0] = ('jmp', 'row3')
    new[f] = st

    # ---- Root.UpdateSelect: five entries; Restore Defaults is now index 4 ----
    f = pk.func(ROOTC, 'UpdateSelect'); st = pk.parse(f)
    for t in ('self.m_iSelect = AddValueLimit(self.m_iSelect, -1, 4, <empty>)',
              'self.m_iSelect = AddValueLimit(self.m_iSelect, 1, 4, <empty>)'):
        i = pk.find(st, t)
        subst(st[i], lambda n: n.op == 0x2C and n.parts == [('u8', 4)], lambda n: pk.int_(5))
    i = pk.find(st, 'if !(NotEqual_IntInt(self.m_iSelect, 3)) goto 0x0230')
    subst(st[i], lambda n: n.op == 0x2C and n.parts == [('u8', 3)], lambda n: pk.int_(4))
    i = pk.find(st, 'self.m_xParent.self.m_iNextType = 4')
    subst(st[i], lambda n: n.op == 0x2C and n.parts == [('u8', 4)], lambda n: pk.int_(7))
    new[f] = st

    # ---- Root.Draw: the cursor; row 3 is Audio's position one row lower ----
    f = pk.func(ROOTC, 'Draw'); st = pk.parse(f)
    sel = pk.inst('Sqex03GameHUDOptionBase', 'm_iSelect')
    i = pk.find(st, 'iAdjust = (Less_IntInt(self.m_iSelect, 3) ? 0 : 1)')
    st[i].parts[1] = ('e', pk.cond(pk.native('EqualEqual_IntInt', clone(sel), pk.int_(3)), pk.int_(-1), pk.int_(0)))
    dc = pk.find(st, 'self.m_xHUD.self.m_Common.DrawCursor(xParam.m_fPosX, xParam.m_fPosY, <empty>)')
    posy = findnode(st[dc], 'xParam.m_fPosY')
    st[dc:dc] = [Node(0x07, [('jmp', st[dc].mem), ('e', pk.native('EqualEqual_IntInt', clone(sel), pk.int_(3)))]),
                 pk.let(clone(posy), pk.native('Add_FloatFloat', clone(posy), pk.float_(36.0)))]
    new[f] = st

    # ---- Root.DrawDetail: descriptions; ours for index 3 ----
    f = pk.func(ROOTC, 'DrawDetail'); st = pk.parse(f)
    i = pk.find(st, 'iAdjust = (Less_IntInt(self.m_iSelect, 3) ? 0 : 2)')
    subst(st[i], lambda n: n.op == 0x2C and n.parts == [('u8', 3)], lambda n: pk.int_(4))
    subst(st[i], lambda n: n.op == 0x2C and n.parts == [('u8', 2)], lambda n: pk.int_(1))
    i = pk.find(st, 'sText = self.m_xParent.self.m_MessageData.GetString(Add_IntInt(Add_IntInt(78, self.m_iSelect), iAdjust))')
    sel = pk.inst('Sqex03GameHUDOptionBase', 'm_iSelect')
    def desc(n):
        return pk.cond(pk.native('EqualEqual_IntInt', clone(sel), pk.int_(3)), pk.int_(MAGIC + 1), n)
    subst(st[i], lambda n: pk.text(n) == 'Add_IntInt(Add_IntInt(78, self.m_iSelect), iAdjust)', desc)
    new[f] = st

    graphics_page(pk, new, show)

    if show:
        for f, st in new.items():
            print('\n== %s.%s' % (pk.outer(f), pk.exps[f - 1]['name']))
            pk.show(st)

    # ---- write ----
    u2 = u
    bodies = []
    for f in sorted(new):
        old = pk.body(f)
        body = pk.rebuild(f, new[f])
        bodies.append((f, old, body))
        u2 = pkgpatch.replace_export(u2, pk.exps, f, body)
    sha = hashlib.sha1(u2).hexdigest()
    if out:
        ov = out + '/PS3_GAME/USRDIR/SQEX03GAME'
        csize = pkgpatch.save_full(u2, ov + '/COOKEDPS3/SQEX03GAME.XXX', pkgpatch.block_size_of(SRC))
        n = pkgpatch.toc_set(ROOT + '/PS3TOC.TXT', ov + '/PS3TOC.TXT', 'Sqex03Game.xxx', csize, len(u2))
        assert n == 2
        pkgpatch.mirror_fios(out, 'SQEX03GAME.XXX')
        open(out + '/sha1.txt', 'w').write(sha)
    if header:
        write_header(header, bodies, len(u2), sha)
    print('patched %d functions; %d bytes, sha1 %s' % (len(new), len(u2), sha))


def write_header(path, bodies, size, sha):
    """The patch as src/dod3_menu_patch.cpp applies it to the player's own
    package: per function, its export index and its new body as copies from
    the original body plus the bytes that are new -- none of the title's
    bytecode is in the file, only offsets into it."""
    import difflib, struct
    stream = bytearray()
    for f, old, new in bodies:
        ops = bytearray()
        nops = 0
        for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, old, new, autojunk=False).get_opcodes():
            if tag == 'equal':
                ops += struct.pack('<BII', 0, i1, i2 - i1)
            elif j2 > j1:
                ops += struct.pack('<BI', 1, j2 - j1) + new[j1:j2]
            else:
                continue
            nops += 1
        stream += struct.pack('<IIII', f, len(old), len(new), nops) + ops
    ver = hashlib.sha1(bytes(stream)).hexdigest()[:16]
    def arr(b):
        return ',\n'.join('    ' + ','.join('0x%02x' % x for x in b[i:i + 16]) for i in range(0, len(b), 16))
    with open(path, 'w', newline='\n') as h:
        h.write('/* Generated by tools/menu_patch.py -- do not edit. The Graphics Settings\n'
                ' * patch to SQEX03GAME.XXX (see src/dod3_menu_patch.cpp). */\n')
        h.write('static const char k_menu_patch_version[] = "%s";\n' % ver)
        h.write('static const char k_menu_patch_orig_sha1[] = "%s";\n' % ORIG_SHA1)
        h.write('static const char k_menu_patch_sha1[] = "%s";\n' % sha)
        h.write('static const unsigned k_menu_patch_size = %d;\n' % size)
        h.write('static const unsigned k_menu_patch_count = %d;\n' % len(bodies))
        h.write('/* per function: u32 export, old size, new size, op count (LE); ops: 0 copy(off, len) | 1 insert(len, bytes) */\n')
        h.write('static const unsigned char k_menu_patch_ops[] = {\n%s\n};\n' % arr(stream))
    print('wrote %s (%d bytes of ops, version %s)' % (path, len(stream), ver))


def graphics_page(pk, new, show):
    """Sqex03GameHUDOptionDisplay -> the Graphics page."""
    DISP = 'Sqex03GameHUDOptionDisplay'

    # the bridge call, from ReflectValue's `self.m_xGameInfo.self.m_xOption.UpdateDisplayParam()`
    f = pk.func(DISP, 'ReflectValue'); st = pk.parse(f)
    i = pk.find(st, 'self.m_xGameInfo.self.m_xOption.UpdateDisplayParam()')
    call_tpl = st[i]
    ret = st[-2:]      # return nothing; <end>
    def bridge(cmd, a=None, b=None):
        n = clone(call_tpl)
        # the innermost call node: 0x1B/0x1C with only the end-parms part left
        def inner(x):
            for k, v in x.parts:
                if k == 'e':
                    r = inner(v)
                    if r: return r
            if x.op in (0x1B, 0x1C): return x
            return None
        c = inner(n)
        args = [pk.int_(cmd), a if a is not None else pk.int_(0), b if b is not None else pk.int_(0)]
        c.parts = c.parts[:1] + [('e', x) for x in args] + [('u8', 0x16)]
        return n

    def body(fn, stmts):
        f = pk.func(DISP, fn)
        new[f] = stmts + [clone(x) for x in ret]
        return f

    body('InitValue', [bridge(CMD_BEGIN)])
    body('ResetValue', [bridge(CMD_RESET)])
    body('ReflectValue', [bridge(CMD_APPLY)])

    # IsChangeValue: return bridge(changed) != 0
    f = pk.func(DISP, 'IsChangeValue'); st = pk.parse(f)
    st[0].parts = [('e', pk.native('NotEqual_IntInt', bridge(CMD_CHANGED), pk.int_(0)))]
    new[f] = st

    # OperateValue(iSelect, iValue): the bridge changes the row; the arrows flash as before
    f = pk.func(DISP, 'OperateValue'); st = pk.parse(f)
    i = pk.find(st, 'if !(bLeftChange) goto 0x00ee')
    lft = clone(st[pk.find(st, 'bLeftChange = Less_IntInt(iValue, 0)', 2)[0]])
    rgt = clone(st[pk.find(st, 'bRightChange = Greater_IntInt(iValue, 0)', 2)[0]])
    remove(st, 0, i)
    st[0:0] = [lft, rgt, bridge(CMD_CHANGE, pk.local(f, 'iSelect'), pk.local(f, 'iValue'))]
    new[f] = st

    # UpdateSelect: ROWS rows; left/right on every row
    f = pk.func(DISP, 'UpdateSelect'); st = pk.parse(f)
    for t in ('self.m_iSelect = AddValueLimit(self.m_iSelect, -1, 2, <empty>)',
              'self.m_iSelect = AddValueLimit(self.m_iSelect, 1, 2, <empty>)'):
        i = pk.find(st, t)
        subst(st[i], lambda n: n.op == 0x2C and n.parts == [('u8', 2)], lambda n: pk.int_(ROWS))
    for t in ('if !(LessEqual_IntInt(self.m_iSelect, 1)) goto 0x011e',
              'if !(LessEqual_IntInt(self.m_iSelect, 1)) goto 0x0164'):
        i = pk.find(st, t)
        subst(st[i], lambda n: n.op == 0x26, lambda n: pk.int_(ROWS - 1))
    new[f] = st

    # Draw: every row in the loop -- bar, icon, label, value, arrows
    f = pk.func(DISP, 'Draw'); st = pk.parse(f)
    I = pk.local(f, 'I')
    loop = pk.find(st, 'if !(LessEqual_IntInt(I, 1)) goto 0x021f')
    subst(st[loop], lambda n: n.op == 0x26, lambda n: pk.int_(ROWS - 1))
    # label: GetString(I == 0 ? 119 : 122) is spelled as an if/else; replace both with MAGIC+100+I
    i0 = pk.find(st, 'if !(EqualEqual_IntInt(I, 0)) goto 0x0171')
    i1 = pk.find(st, 'sText = self.m_xParent.self.m_MessageData.GetString(119)')
    i2 = pk.find(st, 'goto 0x01b1')
    i3 = pk.find(st, 'sText = self.m_xParent.self.m_MessageData.GetString(122)')
    lbl = clone(st[i3])
    subst(lbl, lambda n: n.op == 0x2C and n.parts == [('u8', 122)],
          lambda n: pk.native('Add_IntInt', pk.int_(MAGIC + 100), clone(I)))
    st[i3] = lbl
    remove(st, i0, i3)
    # the row-0 value block (statements from the param-32 lookup to its DrawArrow) as the loop's value code
    v0 = pk.find(st, 'xParam = self.m_xMenu.GetFrameAnimParam(32, <empty>)')
    v1 = pk.find(st, 'DrawArrow(32, sText, EqualEqual_IntInt(self.m_iSelect, 0))')
    w0 = pk.find(st, 'xParam = self.m_xMenu.GetFrameAnimParam(33, <empty>)')
    w1 = pk.find(st, 'DrawArrow(33, sText, EqualEqual_IntInt(self.m_iSelect, 1))')
    block = [clone(x) for x in st[v0:v1 + 1]]
    for x in block:
        subst(x, lambda n: n.op == 0x2C and n.parts == [('u8', 32)],
              lambda n: pk.native('Add_IntInt', pk.int_(32), clone(I)))
    # value text: GetString(MAGIC+200+I); colour: the bridge's is-default
    t = pk.find(block, 'sText = self.m_xParent.self.m_MessageData.GetString(Add_IntInt(68, Subtract_IntInt(1, self.m_iTmpDispSpurtBlood)))')
    subst(block[t], lambda n: pk.text(n) == 'Add_IntInt(68, Subtract_IntInt(1, self.m_iTmpDispSpurtBlood))',
          lambda n: pk.native('Add_IntInt', pk.int_(MAGIC + 200), clone(I)))
    t = pk.find(block, 'eColor = (NotEqual_IntInt(self.m_iTmpDispSpurtBlood, self.m_iDefDispSpurtBlood) ? b6 : b0)')
    subst(block[t], lambda n: pk.text(n) == 'NotEqual_IntInt(self.m_iTmpDispSpurtBlood, self.m_iDefDispSpurtBlood)',
          lambda n: pk.native('EqualEqual_IntInt', bridge(CMD_ISDEF, clone(I)), pk.int_(0)))
    t = pk.find(block, 'DrawArrow(Add_IntInt(32, I), sText, EqualEqual_IntInt(self.m_iSelect, 0))')
    subst(block[t], lambda n: n.op == 0x25, lambda n: clone(I))
    # splice: the loop body ends with `AddAdd_PreInt(I)`; the value code goes before it, the
    # unrolled blocks after the loop go
    inc = pk.find(st, 'AddAdd_PreInt(I)')
    st[inc:inc] = block
    v0 += len(block); w0 += len(block); v1 += len(block); w1 += len(block)
    remove(st, w0, w1 + 1)
    remove(st, v0, v1 + 1)
    new[f] = st

    # DrawDetail: the description of the selected row
    f = pk.func(DISP, 'DrawDetail'); st = pk.parse(f)
    i = pk.find(st, 'sText = self.m_xParent.self.m_MessageData.GetString(82)')
    sel = pk.inst('Sqex03GameHUDOptionBase', 'm_iSelect')
    subst(st[i], lambda n: n.op == 0x2C and n.parts == [('u8', 82)],
          lambda n: pk.native('Add_IntInt', pk.int_(MAGIC + 300), sel))
    new[f] = st


if __name__ == '__main__':
    main()
