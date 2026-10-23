#!/usr/bin/env python3
"""The Direct3D 12 shader cache a release ships (cache/dxbc): its bytecode and its
pipeline list.

    python tools/shader_cache.py build <cache-dir> <dump-dir or run dir>... [--check DIR] [--jobs N]
    python tools/shader_cache.py list <pipelines.bin>... [--cache DIR] [--merge OUT]

build: every program in the shader dumps (PS3RECOMP_SHADER_DUMP's
<vp|fp>_<text hash>.hlsl; a directory holding an hlsl/ folder is looked into)
compiled the way the engine compiles it (rsx_d3d12_engine.c eng_compile:
D3DCompile, entry "main", vs_5_0 / ps_5_0, OPTIMIZATION_LEVEL3 |
IEEE_STRICTNESS, source name "<vp|fp> <hash>") and written under the engine's
file name (eng_shader: <vp|fp>_<fnv1a64("dxbc-v1", hash ^ (stage + 1) *
0x9E3779B97F4A7C15)>.dxbc). The bytecode is the same for every GPU: the cache
saves the HLSL compile (a fragment program 40 ms, a vertex program up to 2 s)
on any machine. --check compares each blob with another cache's file of the
same name. Windows only (d3dcompiler_47.dll).

list: what a pipeline list (cache/dxbc/pipelines.bin, written by the engine as
it builds pipeline states and built ahead at start-up) holds: how many states,
how many of them also multisampled, the programs they name. --cache says which
of those programs the cache directory lacks (a state naming one is skipped at
start-up). --merge writes the lists' states into one, each once, in the order
first met, the flags of its copies combined.
"""
import argparse
import concurrent.futures as cf
import os
import struct
import sys
import time

M = (1 << 64) - 1
PSO_MAGIC = b"RSXPSO\x00\x01"
PSO_WORDS = 184          # rsx_d3d12_engine.c PSO_REC_WORDS
PSO_MSAA = 1             # PSO_REC_MSAA


def fnv1a64(data, h):
    for b in data:
        h ^= b
        h = (h * 1099511628211) & M
    return h


def cache_name(dump_name):
    what, hx = dump_name[:-5].split("_", 1)
    stage = 0 if what == "vp" else 1
    seed = int(hx, 16) ^ (((stage + 1) * 0x9E3779B97F4A7C15) & M)
    return f"{what}_{fnv1a64(b'dxbc-v1', seed):016x}.dxbc"


# ---- build -----------------------------------------------------------------

def compiler():
    import ctypes
    d3dc = ctypes.WinDLL("d3dcompiler_47.dll")
    d3dc.D3DCompile.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p,
                                ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint,
                                ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]
    d3dc.D3DCompile.restype = ctypes.c_long
    get_ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)
    get_size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)
    release = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)

    def blob_bytes(p):   # ID3DBlob: QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize
        vt = ctypes.cast(ctypes.cast(p, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
        data = ctypes.string_at(get_ptr(vt[3])(p), get_size(vt[4])(p))
        release(vt[2])(p)
        return data

    flags = (1 << 15) | (1 << 13)   # D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS

    def compile_one(path):
        name = os.path.basename(path)
        what, hx = name[:-5].split("_", 1)
        src = open(path, "rb").read()
        code, err = ctypes.c_void_p(), ctypes.c_void_p()
        t0 = time.perf_counter()
        hr = d3dc.D3DCompile(src, len(src), f"{what} {hx}".encode(), None, None, b"main",
                             b"vs_5_0" if what == "vp" else b"ps_5_0", flags, 0, ctypes.byref(code), ctypes.byref(err))
        ms = (time.perf_counter() - t0) * 1000.0
        msg = blob_bytes(err.value) if err.value else b""
        if hr < 0 or not code.value:
            return name, None, ms, msg
        return name, blob_bytes(code.value), ms, b""

    return compile_one


def cmd_build(a):
    dumps = {}
    for s in a.sources:
        d = os.path.join(s, "hlsl") if os.path.isdir(os.path.join(s, "hlsl")) else s
        if not os.path.isdir(d):
            continue
        for n in os.listdir(d):
            if n.endswith(".hlsl") and n[:3] in ("vp_", "fp_"):
                dumps.setdefault(n, os.path.join(d, n))
    os.makedirs(a.cache, exist_ok=True)
    print(f"{len(dumps)} programs; compiling with {a.jobs} threads")
    compile_one = compiler()
    times = {"vp": [], "fp": []}
    fails, same, differ, absent = [], 0, 0, 0
    t0 = time.perf_counter()
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for name, data, ms, msg in ex.map(compile_one, sorted(dumps.values())):
            times[name[:2]].append(ms)
            if data is None:
                fails.append((name, msg))
                continue
            out = cache_name(name)
            with open(os.path.join(a.cache, out), "wb") as f:
                f.write(data)
            if a.check:
                p = os.path.join(a.check, out)
                if not os.path.exists(p):
                    absent += 1
                elif open(p, "rb").read() == data:
                    same += 1
                else:
                    differ += 1
    for k, v in times.items():
        if v:
            v.sort()
            print(f"{k}: {len(v)} programs, compile ms median {v[len(v) // 2]:.0f}, p90 {v[len(v) * 9 // 10]:.0f}, "
                  f"max {v[-1]:.0f}, {sum(v) / 1000:.1f} s in all")
    print(f"{time.perf_counter() - t0:.1f} s; {len(fails)} failed")
    for n, m in fails[:10]:
        print("  FAILED", n, m.decode("latin-1", "replace").strip()[:200])
    if a.check:
        print(f"against {a.check}: {same} identical, {differ} different, {absent} not there")
    return 1 if fails or differ else 0


# ---- list ------------------------------------------------------------------

def read_list(path):
    """The records of a pipelines.bin, as tuples of words, in file order."""
    data = open(path, "rb").read()
    if len(data) < 16 or data[:8] != PSO_MAGIC or struct.unpack_from("<I", data, 8)[0] != PSO_WORDS:
        sys.exit(f"{path}: not a pipeline list of this layout")
    size = PSO_WORDS * 4
    recs, off = [], 16
    while off + size <= len(data):
        if data[off:off + 8] == PSO_MAGIC:   # a second header (an early build wrote one a session)
            off += 16
            continue
        recs.append(struct.unpack_from(f"<{PSO_WORDS}I", data, off))
        off += size
    return recs


def dxbc_ids(cache):
    """The checksum (header bytes 4..20) of every .dxbc in a cache directory."""
    ids = set()
    for n in os.listdir(cache):
        if n.endswith(".dxbc"):
            with open(os.path.join(cache, n), "rb") as f:
                h = f.read(20)
            if len(h) == 20 and h[:4] == b"DXBC":
                ids.add(h[4:20])
    return ids


def cmd_list(a):
    merged, order = {}, []
    lines = 0
    for p in a.lists:
        recs = read_list(p)
        lines += len(recs)
        for r in recs:
            key = r[1:]
            if key in merged:
                merged[key] |= r[0]
            else:
                merged[key] = r[0]
                order.append(key)
    msaa = sum(1 for k in order if merged[k] & PSO_MSAA)
    progs = {}
    for k in order:
        for off, what in ((0, "vs"), (4, "ps")):
            progs.setdefault(struct.pack("<4I", *k[off:off + 4]), what)
    print(f"{len(order)} pipeline states ({lines} lines), {msaa} of them also multisampled; "
          f"{sum(1 for w in progs.values() if w == 'vs')} vertex and {sum(1 for w in progs.values() if w == 'ps')} "
          f"pixel programs named")
    if a.cache:
        have = dxbc_ids(a.cache)
        lack = [i for i in progs if i not in have]
        hit = sum(1 for k in order if struct.pack("<4I", *k[0:4]) in have and struct.pack("<4I", *k[4:8]) in have)
        print(f"{a.cache}: {len(have)} programs; {len(lack)} named programs missing; "
              f"{hit} of {len(order)} states can be built ahead")
    if a.merge:
        with open(a.merge, "wb") as f:
            f.write(PSO_MAGIC + struct.pack("<II", PSO_WORDS, 0))
            for k in order:
                f.write(struct.pack(f"<{PSO_WORDS}I", merged[k], *k))
        print(f"wrote {a.merge}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="compile shader dumps into a bytecode cache")
    b.add_argument("cache", help="the cache directory to write")
    b.add_argument("sources", nargs="+", help="shader dump directories, or run directories holding hlsl/")
    b.add_argument("--check", help="another cache to compare each blob with")
    b.add_argument("--jobs", type=int, default=os.cpu_count() or 4, help="compile threads")
    l = sub.add_parser("list", help="summarise, check or merge pipeline lists")
    l.add_argument("lists", nargs="+", help="pipelines.bin files")
    l.add_argument("--cache", help="the bytecode cache the states' programs should be in")
    l.add_argument("--merge", help="write the states of all the lists to this file")
    a = ap.parse_args()
    return cmd_build(a) if a.cmd == "build" else cmd_list(a)


if __name__ == "__main__":
    sys.exit(main())
