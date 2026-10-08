"""Helpers to read and build UnrealScript bytecode trees (ue3script.Node).

Pkg wraps an uncompressed package: names/imports/exports, function lookup,
parsing a function's statements, rendering nodes as text (to find them),
building new nodes, and re-serializing a function export with new code.
"""
import struct, copy
from . import upk, script as ue3script, dis as disasm
from .script import Node

class Pkg:
    def __init__(self, u):
        self.u = u
        self.s, self.names, self.imps, self.exps = upk.tables(u)
        self.name_idx = {}
        for i, n in enumerate(self.names):
            self.name_idx.setdefault(n, i)
        self.nat = {}            # native function name -> index
        for k, v in disasm.NATIVE_NAMES.items():
            self.nat.setdefault(v, k)
        _PK[0] = self

    # -- lookup ----------------------------------------------------------
    def oname(self, v):
        if v == 0: return 'None'
        return self.exps[v - 1]['name'] if v > 0 else self.imps[-v - 1][3]
    def cls(self, i):
        return upk.objname(self.exps[i - 1]['cls'], self.imps, self.exps)
    def outer(self, i):
        return upk.objname(self.exps[i - 1]['outer'], self.imps, self.exps)
    def export(self, name, cls=None, outer=None):
        hits = [i + 1 for i, e in enumerate(self.exps) if e['name'] == name
                and (cls is None or self.cls(i + 1) == cls)
                and (outer is None or self.outer(i + 1) == outer)]
        assert len(hits) == 1, (name, cls, outer, hits)
        return hits[0]
    def func(self, cls, fn):
        return self.export(fn, 'Function', cls)
    def name(self, s):
        return (self.name_idx[s], 0)

    # -- functions -------------------------------------------------------
    def body(self, idx):
        e = self.exps[idx - 1]
        return self.u[e['off']:e['off'] + e['size']]
    def parse(self, idx):
        b = self.body(idx)
        nxt, sup, ch, msz, ssz, code, native, flags = disasm.func_layout(b)
        stmts, m = ue3script.parse(b, code, code + ssz)
        return stmts
    def rebuild(self, idx, stmts):
        """The function export with its script replaced by stmts."""
        b = self.body(idx)
        nxt, sup, ch, msz, ssz, code, native, flags = disasm.func_layout(b)
        enc, mem = ue3script.encode(stmts)
        out = bytearray(b[:code - 8])
        out += struct.pack('>II', mem, len(enc))
        out += enc
        out += b[code + ssz:]
        return bytes(out)

    # -- text ------------------------------------------------------------
    def text(self, node):
        """Render like disasm.py (for finding statements)."""
        enc, _ = ue3script.encode([_strip(node)])
        d = disasm.Dis(enc + b'\x53', self.names, self.imps, self.exps)
        return d.expr()

    def show(self, stmts):
        for s in stmts:
            print('  %s' % self.text(s))

    def find(self, stmts, text, count=1):
        hits = [i for i, s in enumerate(stmts) if self.text(s) == text]
        assert len(hits) == count, (text, hits, [self.text(s) for s in stmts])
        return hits if count != 1 else hits[0]

    # -- building --------------------------------------------------------
    def int_(self, v):
        if v == 0: return Node(0x25, [])
        if v == 1: return Node(0x26, [])
        if 0 <= v < 256: return Node(0x2C, [('u8', v)])
        return Node(0x1D, [('i32', v)])
    def float_(self, v):
        return Node(0x1E, [('i32', struct.unpack('>i', struct.pack('>f', v))[0])])
    def byte_(self, v):
        return Node(0x24, [('u8', v)])
    def native(self, name, *args):
        idx = self.nat[name]
        P = [('e', a) for a in args] + [('u8', 0x16)]
        if idx < 0x100 and idx >= 0x70:
            return Node(idx, P)
        return Node(0x60 + (idx >> 8), [('u8', idx & 0xFF)] + P)
    def local(self, fidx, name):
        return Node(0x00, [('obj', self._member(fidx, name))])
    def inst(self, cls, name):
        return Node(0x01, [('obj', self.export(name, outer=cls))])
    def let(self, a, b):
        return Node(0x0F, [('e', a), ('e', b)])
    def cond(self, c, a, b):
        return Node(0x45, [('e', c), ('skip', 1), ('e', a), ('skip', 1), ('e', b)])
    def ret_nothing(self, fidx):
        return Node(0x04, [('e', Node(0x0B, []))])
    def _member(self, fidx, name):
        b = self.body(fidx)
        nxt, sup, ch, msz, ssz, code, native, flags = disasm.func_layout(b)
        c = ch
        while c > 0:
            if self.exps[c - 1]['name'] == name: return c
            cb = self.body(c)
            c = struct.unpack_from('>i', cb, 12)[0]
        raise KeyError(name)


def _strip(node):
    """A copy with jumps neutralised so a lone statement encodes."""
    n = copy.deepcopy(node)
    def walk(x):
        for i, (k, v) in enumerate(x.parts):
            if k == 'jmp': x.parts[i] = ('u16', v if isinstance(v, int) else 0xFFFE)
            elif k == 'skip_ctx' and v is not None: x.parts[i] = ('skip_ctx', None)
            elif k == 'e': walk(v)
    walk(n)
    n.mem = None; n.label = None
    return n


def clone(node):
    n = copy.deepcopy(node)
    def walk(x):
        x.mem = None; x.label = None
        for k, v in x.parts:
            if k == 'e': walk(v)
    walk(n)
    return n


def findnode(node, text, pk=None):
    """The first sub-expression of node whose rendering is text."""
    pk = pk or _PK[0]
    stack = [node]
    while stack:
        x = stack.pop(0)
        if x is not node and pk.text(x) == text: return x
        stack += [v for k, v in x.parts if k == 'e']
    raise KeyError(text)


def inner_call(node):
    """The innermost function call (EX_VirtualFunction / EX_FinalFunction) in a context chain."""
    for k, v in node.parts:
        if k == 'e':
            r = inner_call(v)
            if r: return r
    return node if node.op in (0x1B, 0x1C) else None


_PK = [None]


def remove(st, i, j):
    """Delete st[i:j]; jumps that landed on them land on the statement after."""
    gone = []
    for s in st[i:j]:
        if s.mem is not None: gone.append(s.mem)
        gone += (s.label or [])
    del st[i:j]
    if i < len(st):
        st[i].label = (st[i].label or []) + gone


def subst(node, pred, make):
    """Replace every sub-expression for which pred(node) holds with make(node)."""
    for i, (k, v) in enumerate(node.parts):
        if k == 'e':
            if pred(v): node.parts[i] = ('e', make(v))
            else: subst(v, pred, make)
