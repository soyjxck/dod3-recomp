"""UnrealScript (UE3 v860, PS3 cook) disassembler for DoD3 packages.

python tools/ue3 dis <package> <Class> [Function]
Prints each function of the class: flags, parameters/locals, and the
bytecode with in-memory offsets (object refs are 4 bytes on disk, 8 in
memory; jump targets are memory offsets).
"""
import sys, os, struct
from . import upk

NATIVE_NAMES = {}

class Dis:
    def __init__(self, b, names, imps, exps):
        self.b, self.names, self.imps, self.exps = b, names, imps, exps
        self.p = 0          # storage offset
        self.m = 0          # memory offset
        self.out = []

    # -- readers ---------------------------------------------------------
    def u8(self):
        v = self.b[self.p]; self.p += 1; self.m += 1; return v
    def u16(self):
        v = struct.unpack_from('>H', self.b, self.p)[0]; self.p += 2; self.m += 2; return v
    def i32(self):
        v = struct.unpack_from('>i', self.b, self.p)[0]; self.p += 4; self.m += 4; return v
    def f32(self):
        v = struct.unpack_from('>f', self.b, self.p)[0]; self.p += 4; self.m += 4; return v
    def obj(self):
        v = struct.unpack_from('>i', self.b, self.p)[0]; self.p += 4; self.m += 8
        return self.oname(v)
    def name(self):
        i, n = struct.unpack_from('>II', self.b, self.p); self.p += 8; self.m += 8
        return self.names[i] + ('_%d' % (n - 1) if n else '')
    def oname(self, v):
        if v == 0: return 'None'
        if v > 0:
            e = self.exps[v - 1]
            return e['name']
        return self.imps[-v - 1][3]

    # -- expressions -----------------------------------------------------
    def params(self):
        a = []
        while self.b[self.p] != 0x16:
            a.append(self.expr())
        self.u8()
        return a

    def expr(self):
        op = self.u8()
        if op == 0x00: return self.obj()
        if op == 0x01: return 'self.' + self.obj()
        if op == 0x02: return 'default.' + self.obj()
        if op == 0x03: return 'state.' + self.obj()
        if op == 0x04: return 'return ' + self.expr()
        if op == 0x05:
            prop = self.obj(); sz = self.u8()
            return 'switch(%s)' % self.expr()
        if op == 0x06: return 'goto 0x%04x' % self.u16()
        if op == 0x07:
            t = self.u16(); return 'if !(%s) goto 0x%04x' % (self.expr(), t)
        if op == 0x08: return 'stop'
        if op == 0x09:
            line = self.u16(); dbg = self.u8(); return 'assert(%s)' % self.expr()
        if op == 0x0A:
            t = self.u16()
            if t == 0xFFFF: return 'default: (next 0x%04x)' % t
            return 'case %s: (next 0x%04x)' % (self.expr(), t)
        if op == 0x0B: return 'nothing'
        if op == 0x0C:
            labs = []
            while True:
                n = self.name(); off = self.i32()
                if n == 'None': break
                labs.append('%s@0x%x' % (n, off))
            return 'labels ' + ','.join(labs)
        if op == 0x0D: return 'gotolabel ' + self.expr()
        if op == 0x0E:
            self.obj(); return self.expr()
        if op == 0x0F:
            a = self.expr(); return '%s = %s' % (a, self.expr())
        if op == 0x10:
            i = self.expr(); return '%s[%s]' % (self.expr(), i)
        if op == 0x11:
            a = [self.expr() for _ in range(5)]
            return 'new(%s)' % ', '.join(a)
        if op == 0x12:
            c = self.expr(); self.u16(); self.obj(); self.u8()
            return '%s.static.%s' % (c, self.expr())
        if op == 0x13:
            c = self.obj(); return 'class<%s>(%s)' % (c, self.expr())
        if op == 0x14:
            a = self.expr(); return '%s = %s' % (a, self.expr())
        if op == 0x15: return '<endparm>'
        if op == 0x16: return '<endparms>'
        if op == 0x17: return 'self'
        if op == 0x18:
            self.u16(); return self.expr()
        if op == 0x19:
            o = self.expr(); self.u16(); self.obj(); self.u8()
            return '%s.%s' % (o, self.expr())
        if op == 0x1A:
            i = self.expr(); return '%s[%s]' % (self.expr(), i)
        if op == 0x1B:
            n = self.name(); return '%s(%s)' % (n, ', '.join(self.params()))
        if op == 0x1C:
            f = self.obj(); return '%s(%s)' % (f, ', '.join(self.params()))
        if op == 0x1D: return str(self.i32())
        if op == 0x1E: return '%gf' % self.f32()
        if op == 0x1F:
            e = self.b.index(b'\0', self.p); s = self.b[self.p:e].decode('latin-1')
            self.m += e + 1 - self.p; self.p = e + 1; return repr(s)
        if op == 0x20: return 'obj(%s)' % self.obj()
        if op == 0x21: return "'%s'" % self.name()
        if op == 0x22: return 'rot(%d,%d,%d)' % (self.i32(), self.i32(), self.i32())
        if op == 0x23: return 'vect(%g,%g,%g)' % (self.f32(), self.f32(), self.f32())
        if op == 0x24: return 'b%d' % self.u8()
        if op == 0x25: return '0'
        if op == 0x26: return '1'
        if op == 0x27: return 'true'
        if op == 0x28: return 'false'
        if op == 0x29: return 'nparm ' + self.obj()
        if op == 0x2A: return 'none'
        if op == 0x2C: return str(self.u8())
        if op == 0x2D: return self.expr()
        if op == 0x2E:
            c = self.obj(); return '%s(%s)' % (c, self.expr())
        if op == 0x2F:
            e = self.expr(); t = self.u16(); return 'foreach %s (end 0x%04x)' % (e, t)
        if op == 0x30: return 'iteratorpop'
        if op == 0x31: return 'iteratornext'
        if op in (0x32, 0x33):
            self.obj(); a = self.expr(); b = self.expr()
            return '(%s %s %s)' % (a, '==' if op == 0x32 else '!=', b)
        if op == 0x34:
            s = ''
            while True:
                c = struct.unpack_from('>H', self.b, self.p)[0]; self.p += 2; self.m += 2
                if not c: break
                s += chr(c)
            return 'u' + repr(s)
        if op == 0x35:
            prop = self.obj(); st = self.obj(); self.u8(); self.u8()
            return '%s.%s' % (self.expr(), prop)
        if op == 0x36: return self.expr() + '.Length'
        if op == 0x37:
            n = self.name(); return 'global.%s(%s)' % (n, ', '.join(self.params()))
        if op == 0x38:
            t = self.u8(); return 'cast%d(%s)' % (t, self.expr())
        if op == 0x39:
            a = self.expr(); i = self.expr(); n = self.expr(); return '%s.Insert(%s,%s)' % (a, i, n)
        if op == 0x3A: return 'return /*%s*/' % self.obj()
        if op in (0x3B, 0x3C, 0x3D, 0x3E):
            return 'delcmp(%s)' % ', '.join(self.params())
        if op == 0x3F: return 'emptydelegate'
        if op == 0x40:
            a = self.expr(); i = self.expr(); n = self.expr(); return '%s.Remove(%s,%s)' % (a, i, n)
        if op == 0x42:
            loc = self.u8(); o = self.obj(); n = self.name()
            return 'delegate %s(%s)' % (n, ', '.join(self.params()))
        if op == 0x43:
            n = self.name(); self.obj(); return 'delegateprop ' + n
        if op == 0x44:
            a = self.expr(); return '%s = %s' % (a, self.expr())
        if op == 0x45:
            c = self.expr(); self.u16(); a = self.expr(); self.u16(); b = self.expr()
            return '(%s ? %s : %s)' % (c, a, b)
        if op == 0x46:
            a = self.expr(); self.u16(); v = self.expr(); self.u8()
            return '%s.Find(%s)' % (a, v)
        if op == 0x47:
            a = self.expr(); self.u16(); v = self.expr(); w = self.expr(); self.u8()
            return '%s.Find(%s, %s)' % (a, v, w)
        if op == 0x48: return 'out ' + self.obj()
        if op == 0x49:
            self.u16(); e = self.expr(); self.u8(); return 'defparm(%s)' % e
        if op == 0x4A: return '<empty>'
        if op == 0x4B: return 'instdelegate ' + self.name()
        if op == 0x51: return 'iface(%s)' % self.expr()
        if op == 0x52:
            c = self.obj(); return '%s(%s)' % (c, self.expr())
        if op == 0x53: return '<end>'
        if op == 0x54:
            a = self.expr(); n = self.expr(); self.u8(); return '%s.Add(%s)' % (a, n)
        if op == 0x55:
            a = self.expr(); self.u16(); n = self.expr(); self.u8(); return '%s.AddItem(%s)' % (a, n)
        if op == 0x56:
            a = self.expr(); self.u16(); n = self.expr(); self.u8(); return '%s.RemoveItem(%s)' % (a, n)
        if op == 0x57:
            a = self.expr(); self.u16(); i = self.expr(); n = self.expr(); self.u8(); return '%s.InsertItem(%s,%s)' % (a, i, n)
        if op == 0x58:
            a = self.expr(); v = self.expr(); hi = self.u8(); i = self.expr() if hi else ''; t = self.u16()
            return 'foreach %s(%s,%s) (end 0x%04x)' % (a, v, i, t)
        if op == 0x59:
            a = self.expr(); self.u16(); f = self.expr(); self.u8(); return '%s.Sort(%s)' % (a, f)
        if 0x60 <= op < 0x70:
            idx = ((op - 0x60) << 8) | self.u8()
            return '%s(%s)' % (NATIVE_NAMES.get(idx, 'native%d' % idx), ', '.join(self.params()))
        if op >= 0x70:
            return '%s(%s)' % (NATIVE_NAMES.get(op, 'native%d' % op), ', '.join(self.params()))
        raise ValueError('unknown op 0x%02x at %d' % (op, self.p - 1))

    def run(self, end):
        while self.p < end:
            m0 = self.m
            try:
                s = self.expr()
            except Exception as ex:
                self.out.append('  %04x: !! %s ; bytes %s' % (m0, ex, self.b[self.p - 1:self.p + 16].hex(' ')))
                return
            self.out.append('  %04x: %s' % (m0, s))


def func_layout(b):
    """(next, super, children, memsize, storage, code_off, native, flags)"""
    p = 4 + 8                     # NetIndex, None tag
    nxt, sup, ch, msz, ssz = struct.unpack_from('>iiiII', b, p); p += 20
    code = p
    p += ssz
    native = struct.unpack_from('>H', b, p)[0]; p += 2
    prec = b[p]; p += 1
    flags = struct.unpack_from('>I', b, p)[0]
    return nxt, sup, ch, msz, ssz, code, native, flags


def load_natives(paths):
    for path in paths:
        u = upk.decompress(path)
        s, names, imps, exps = upk.tables(u)
        for e in exps:
            if upk.objname(e['cls'], imps, exps) != 'Function': continue
            b = u[e['off']:e['off'] + e['size']]
            try:
                nxt, sup, ch, msz, ssz, code, native, flags = func_layout(b)
            except Exception:
                continue
            if native and native not in NATIVE_NAMES:
                NATIVE_NAMES[native] = e['name']


def main():
    pkg = sys.argv[1]; cls = sys.argv[2]; fn = sys.argv[3] if len(sys.argv) > 3 else None
    d = os.path.dirname(pkg)
    load_natives([os.path.join(d, n) for n in ('CORE.XXX', 'ENGINE.XXX', 'GAMEFRAMEWORK.XXX', 'GFXUI.XXX', 'SQEX03GAME.XXX', 'SQEXSEAD.XXX')])
    u = upk.decompress(pkg)
    s, names, imps, exps = upk.tables(u)
    for i, e in enumerate(exps):
        if upk.objname(e['cls'], imps, exps) != 'Function': continue
        if upk.objname(e['outer'], imps, exps) != cls: continue
        if fn and e['name'] != fn: continue
        b = u[e['off']:e['off'] + e['size']]
        nxt, sup, ch, msz, ssz, code, native, flags = func_layout(b)
        # parameter / local chain
        locs = []
        c = ch
        while c > 0:
            ce = exps[c - 1]
            cb = u[ce['off']:ce['off'] + ce['size']]
            locs.append('%s %s' % (upk.objname(ce['cls'], imps, exps).replace('Property', ''), ce['name']))
            # UField.Next comes after NetIndex + None tag
            c = struct.unpack_from('>i', cb, 12)[0]
        print('\n== %s.%s (export %d) flags 0x%08x native %d super %s mem %d' % (
            cls, e['name'], i + 1, flags, native, upk.objname(sup, imps, exps), msz))
        if locs: print('   locals: ' + ', '.join(locs))
        dz = Dis(b, names, imps, exps)
        dz.p = code
        dz.run(code + ssz)
        print('\n'.join(dz.out))

