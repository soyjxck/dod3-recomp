"""UnrealScript bytecode (UE3 v860, PS3 cook) as an editable tree.

parse(code_bytes) -> [Stmt]; each Stmt is a Node with .mem (its memory
offset in the original). encode(stmts) -> (bytes, memsize), re-deriving
every skip size from the encoded children and remapping absolute jump
targets (Jump, JumpIfNot, Case, Iterator ends) through the statement
offsets, so statements can be inserted, removed or replaced.

Object references are 4 bytes on disk and 8 in memory; names are 8 in both.
"""
import struct

class Node:
    __slots__ = ('op', 'parts', 'mem', 'label')
    def __init__(self, op, parts, mem=None):
        self.op, self.parts, self.mem, self.label = op, parts, mem, None
    def __repr__(self):
        return 'Node(0x%02x, %r)' % (self.op, self.parts)

# part kinds:
#   ('u8', v) ('u16', v) ('i32', v) ('f32', bits) ('obj', idx) ('name', (i, n))
#   ('raw', bytes)            literal bytes (string constants), mem == len
#   ('e', Node)               sub-expression
#   ('jmp', target)           absolute memory offset (u16), remapped on encode
#   ('skip', k)               u16 = memory size of the next k parts

class Parser:
    def __init__(self, b, p, end):
        self.b, self.p, self.end = b, p, end
        self.m = 0
    def _u8(self):
        v = self.b[self.p]; self.p += 1; self.m += 1; return v
    def _u16(self):
        v = struct.unpack_from('>H', self.b, self.p)[0]; self.p += 2; self.m += 2; return v
    def _i32(self):
        v = struct.unpack_from('>i', self.b, self.p)[0]; self.p += 4; self.m += 4; return v
    def _obj(self):
        v = struct.unpack_from('>i', self.b, self.p)[0]; self.p += 4; self.m += 8; return ('obj', v)
    def _name(self):
        i, n = struct.unpack_from('>II', self.b, self.p); self.p += 8; self.m += 8; return ('name', (i, n))
    def peek(self):
        return self.b[self.p]

    def params(self, parts):
        while self.peek() != 0x16:
            parts.append(('e', self.expr()))
        parts.append(('u8', self._u8()))

    def expr(self):
        m0 = self.m
        op = self._u8()
        P = []
        n = Node(op, P, m0)
        if op in (0x00, 0x01, 0x02, 0x03, 0x29, 0x48, 0x3A, 0x20):
            P.append(self._obj())
        elif op == 0x04:
            P.append(('e', self.expr()))
        elif op == 0x05:
            P.append(self._obj()); P.append(('u8', self._u8())); P.append(('e', self.expr()))
        elif op == 0x06:
            P.append(('jmp', self._u16()))
        elif op == 0x07:
            P.append(('jmp', self._u16())); P.append(('e', self.expr()))
        elif op in (0x08, 0x0B, 0x15, 0x16, 0x17, 0x25, 0x26, 0x27, 0x28, 0x2A, 0x30, 0x31, 0x3F, 0x4A, 0x53):
            pass
        elif op == 0x09:
            P.append(('u16', self._u16())); P.append(('u8', self._u8())); P.append(('e', self.expr()))
        elif op == 0x0A:
            t = self._u16()
            if t == 0xFFFF: P.append(('u16', t))
            else: P.append(('jmp', t)); P.append(('e', self.expr()))
        elif op == 0x0D:
            P.append(('e', self.expr()))
        elif op == 0x0E:
            P.append(self._obj()); P.append(('e', self.expr()))
        elif op in (0x0F, 0x10, 0x14, 0x1A, 0x44):
            P.append(('e', self.expr())); P.append(('e', self.expr()))
        elif op == 0x11:
            for _ in range(5): P.append(('e', self.expr()))
        elif op in (0x12, 0x19):
            # o.expr: the u16 skips the trailing expression when o is None.
            # It is the expression's size -- except for an iterator call in a
            # foreach, where it jumps past the whole loop: kept as a target.
            P.append(('e', self.expr()))
            w = self._u16()
            fld = self._obj(); sz = self._u8()
            after = self.m
            e = self.expr()
            if w == self.m - after: P.append(('skip_ctx', None))
            else: P.append(('skip_ctx', after + w))
            P.append(fld); P.append(('u8', sz)); P.append(('e', e))
        elif op in (0x13, 0x2E, 0x52):
            P.append(self._obj()); P.append(('e', self.expr()))
        elif op == 0x18:
            # the skip of && / || also steps over the 0x16 that ends the call
            self._u16(); P.append(('skip1', 1)); P.append(('e', self.expr()))
        elif op == 0x5A:
            P.append(('jmp', self._u16()))
        elif op == 0x1B:
            P.append(self._name()); self.params(P)
        elif op == 0x1C:
            P.append(self._obj()); self.params(P)
        elif op in (0x1D,):
            P.append(('i32', self._i32()))
        elif op == 0x1E:
            P.append(('i32', self._i32()))
        elif op == 0x1F:
            e = self.b.index(b'\0', self.p) + 1
            s = self.b[self.p:e]; self.m += len(s); self.p = e
            P.append(('raw', s))
        elif op == 0x21:
            P.append(self._name())
        elif op in (0x22, 0x23):
            for _ in range(3): P.append(('i32', self._i32()))
        elif op in (0x24, 0x2C):
            P.append(('u8', self._u8()))
        elif op in (0x2D, 0x36, 0x51):
            P.append(('e', self.expr()))
        elif op == 0x2F:
            P.append(('e', self.expr())); P.append(('jmp', self._u16()))
        elif op in (0x32, 0x33):
            P.append(self._obj()); P.append(('e', self.expr())); P.append(('e', self.expr()))
        elif op == 0x34:
            s0 = self.p
            while struct.unpack_from('>H', self.b, self.p)[0]: self.p += 2
            self.p += 2
            s = self.b[s0:self.p]; self.m += len(s); P.append(('raw', s))
        elif op == 0x35:
            P.append(self._obj()); P.append(self._obj()); P.append(('u8', self._u8())); P.append(('u8', self._u8()))
            P.append(('e', self.expr()))
        elif op == 0x37:
            P.append(self._name()); self.params(P)
        elif op == 0x38:
            P.append(('u8', self._u8())); P.append(('e', self.expr()))
        elif op in (0x39, 0x40):
            for _ in range(3): P.append(('e', self.expr()))
        elif op in (0x3B, 0x3C, 0x3D, 0x3E):
            self.params(P)
        elif op == 0x42:
            P.append(('u8', self._u8())); P.append(self._obj()); P.append(self._name()); self.params(P)
        elif op == 0x43:
            P.append(self._name()); P.append(self._obj())
        elif op == 0x45:
            P.append(('e', self.expr()))
            self._u16(); P.append(('skip', 1)); P.append(('e', self.expr()))
            self._u16(); P.append(('skip', 1)); P.append(('e', self.expr()))
        elif op in (0x46, 0x54, 0x55, 0x56):
            if op == 0x54:
                P.append(('e', self.expr())); P.append(('e', self.expr())); P.append(('u8', self._u8()))
            else:
                P.append(('e', self.expr())); self._u16(); P.append(('skip', 2))
                P.append(('e', self.expr())); P.append(('u8', self._u8()))
        elif op in (0x47, 0x57):
            P.append(('e', self.expr())); self._u16(); P.append(('skip', 3))
            P.append(('e', self.expr())); P.append(('e', self.expr())); P.append(('u8', self._u8()))
        elif op == 0x49:
            self._u16(); P.append(('skip', 2)); P.append(('e', self.expr())); P.append(('u8', self._u8()))
        elif op == 0x4B:
            P.append(self._name())
        elif op == 0x58:
            P.append(('e', self.expr())); P.append(('e', self.expr()))
            # has-index flag, then the index expression (0x4A when absent), then the loop end
            P.append(('u8', self._u8())); P.append(('e', self.expr()))
            P.append(('jmp', self._u16()))
        elif op == 0x59:
            P.append(('e', self.expr())); self._u16(); P.append(('skip', 2))
            P.append(('e', self.expr())); P.append(('u8', self._u8()))
        elif 0x60 <= op < 0x70:
            P.append(('u8', self._u8())); self.params(P)
        elif op >= 0x70:
            self.params(P)
        else:
            raise ValueError('unknown op 0x%02x at %d' % (op, self.p - 1))
        return n

def parse(b, start, end):
    ps = Parser(b, start, end)
    out = []
    while ps.p < end:
        out.append(ps.expr())
    assert ps.p == end, (ps.p, end)
    return out, ps.m

# ---- encoding ---------------------------------------------------------------

def _enc(node, out, mpos, fix):
    """Append node to out (bytearray); mpos is the memory offset at the start.
    fix collects (byte_pos, old_target) for absolute jumps. Returns mem size."""
    out.append(node.op); m = 1
    P = node.parts
    i = 0
    while i < len(P):
        k, v = P[i]
        if k == 'u8': out.append(v); m += 1
        elif k == 'u16': out += struct.pack('>H', v); m += 2
        elif k == 'i32': out += struct.pack('>i', v); m += 4
        elif k == 'obj': out += struct.pack('>i', v); m += 8
        elif k == 'name': out += struct.pack('>II', *v); m += 8
        elif k == 'raw': out += v; m += len(v)
        elif k == 'e': m += _enc(v, out, mpos + m, fix)
        elif k == 'jmp':
            fix.append((len(out), v)); out += b'\0\0'; m += 2
        elif k in ('skip', 'skip1'):
            pos = len(out); out += b'\0\0'; m += 2
            sub = 0
            for j in range(i + 1, i + 1 + v):
                sub += _enc_part(P[j], out, mpos + m + sub, fix)
            struct.pack_into('>H', out, pos, sub + (1 if k == 'skip1' else 0))
            m += sub
            i += v
        elif k == 'skip_ctx':
            # u16 skip, then field obj + size byte (not counted), then the expression (counted)
            pos = len(out); out += b'\0\0'; m += 2
            m += _enc_part(P[i + 1], out, mpos + m, fix)
            m += _enc_part(P[i + 2], out, mpos + m, fix)
            sub = _enc_part(P[i + 3], out, mpos + m, fix)
            if v is None:
                struct.pack_into('>H', out, pos, sub)
            else:
                fix.append((pos, v, mpos + m))   # relative to here, to the old target v
            m += sub
            i += 3
        else:
            raise ValueError(k)
        i += 1
    return m

def _enc_part(part, out, mpos, fix):
    k, v = part
    if k == 'e': return _enc(v, out, mpos, fix)
    if k == 'u8': out.append(v); return 1
    if k == 'u16': out += struct.pack('>H', v); return 2
    if k == 'i32': out += struct.pack('>i', v); return 4
    if k == 'obj': out += struct.pack('>i', v); return 8
    if k == 'name': out += struct.pack('>II', *v); return 8
    if k == 'raw': out += v; return len(v)
    if k == 'jmp':
        fix.append((len(out), v)); out += b'\0\0'; return 2
    raise ValueError(k)

def encode(stmts):
    """Statements -> (bytes, memsize). Jump targets in the statements are old
    memory offsets (or Label objects); they are mapped to the new offsets of
    the statements that started there."""
    out = bytearray(); fix = []
    newpos = {}
    m = 0
    for s in stmts:
        if s.mem is not None and s.mem not in newpos: newpos[s.mem] = m
        for l in (s.label or []): newpos.setdefault(l, m)
        m += _enc(s, out, m, fix)
    for f in fix:
        pos, tgt = f[0], f[1]
        if tgt in newpos: t = newpos[tgt]
        else: raise ValueError('jump to %r is not a statement start' % (tgt,))
        if len(f) == 3: t -= f[2]       # a context skip: relative to its own end
        struct.pack_into('>H', out, pos, t)
    return bytes(out), m
