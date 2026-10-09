/*
 * Edge zlib inflate, natively.
 *
 * Drakengard 3 streams its packages in 64-128 KB zlib chunks, and every chunk
 * is inflated by Sony's Edge zlib SPU task (spu_0001, edgezlib_inflate_task):
 * the PPU's edgeZlibAddInflateQueueElement (func_00AC7428) pushes a 32-byte
 * request onto an LFQueue and the task pops it, inflates and counts the
 * request's work-to-do counter down. Lifted, that task is the slowest link in
 * streaming: the main thread waits on it in UE3's async archive (usleep at
 * 0x0014163C) for whole frames -- the 300 ms freezes in gameplay.
 *
 * So the push inflates the chunk itself, with the host's zlib, straight from
 * guest memory into the request's output buffer, and hands the task a request
 * that only copies 16 bytes of that output onto themselves. The task still
 * pops it and counts down the counter and sets the event flag exactly as for a
 * real one -- Edge's own completion code, untouched -- it just has nothing left
 * to inflate. A chunk that does not inflate cleanly to the size asked for, or
 * whose zlib trailer's Adler-32 disagrees, goes to the task unchanged.
 *
 * The request (big-endian words, as func_00AC7428 builds it and the task's
 * loop at 0x31C8 reads it):
 *   +00 compressed EA        +04 output EA
 *   +08 compressed bytes     +0C output bytes
 *   +10 work-to-do counter EA | bit 0: 1 inflate, 0 copy
 *   +14 event flag EA        +18 u16 event flag bits
 *   +1A u16 bytes of the stream to drop before the output
 *   +1C u16 bytes of the stream to drop after it
 * The task only takes a copy request of up to 64 KB, hence the short copy.
 *
 * DOD3_ZLIB_NATIVE=0 leaves every request to the task.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <vector>
#include <zlib.h>
#include "cpu/eboot.h"   /* the EBOOT version's addresses */
#include "util.h"

extern "C" uint8_t* vm_base;

namespace {

constexpr uint32_t kAddInflateQueueElementPush = DOD3_A_INFLATE_PUSH_LR;   /* lr of its LFQueue push */

std::atomic<unsigned long long> s_native, s_to_task, s_bad, s_bytes_in, s_bytes_out, s_ns;

bool enabled()
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("DOD3_ZLIB_NATIVE");
        on = !(e && e[0] == '0');
    }
    return on;
}

void report()
{
    const double ms = (double)s_ns.load() / 1e6;
    fprintf(stderr,
            "[edgezlib] %llu chunks inflated natively (%.1f MB -> %.1f MB in %.0f ms), "
            "%llu left to the SPU task, %llu that failed to inflate or check\n",
            s_native.load(), (double)s_bytes_in.load() / 1048576.0, (double)s_bytes_out.load() / 1048576.0, ms,
            s_to_task.load(), s_bad.load());
}

/* Inflate the raw deflate stream at `in` into `out` (exactly `total` bytes);
 * true when it ends there and any Adler-32 trailer after it agrees. */
bool inflate_exact(const uint8_t* in, uint32_t in_size, uint8_t* out, uint32_t total)
{
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit2(&z, -15) != Z_OK) return false;
    z.next_in = const_cast<Bytef*>(in);
    z.avail_in = in_size;
    z.next_out = out;
    z.avail_out = total;
    const int rc = inflate(&z, Z_FINISH);
    const bool ended = rc == Z_STREAM_END && z.total_out == total;
    const uint32_t left = z.avail_in;
    const uint8_t* tail = z.next_in;
    inflateEnd(&z);
    if (!ended) return false;
    if (left >= 4 && dod3_be32(tail) != (uint32_t)adler32(adler32(0, nullptr, 0), out, total)) return false;
    return true;
}

}  // namespace

/* Called by the LFQueue push with the request about to be queued (8 words,
 * host order). Returns 1 when the chunk was inflated here and req now holds
 * the copy request to queue in its place. */
extern "C" int dod3_edgezlib_push(uint32_t lr, uint32_t req[8])
{
    if (lr != kAddInflateQueueElementPush || !enabled()) return 0;
    static bool s_atexit = (atexit(report), true);
    (void)s_atexit;

    const uint32_t in_ea = req[0], out_ea = req[1], in_size = req[2], out_size = req[3];
    const uint32_t skip_begin = req[6] & 0xFFFFu, skip_end = req[7] >> 16;
    if (!(req[4] & 1u) || !out_size || !in_size) {
        s_to_task++;
        return 0;
    }

    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t total = skip_begin + out_size + skip_end;
    bool ok;
    if (!skip_begin && !skip_end) {
        ok = inflate_exact(vm_base + in_ea, in_size, vm_base + out_ea, out_size);
    } else {
        std::vector<uint8_t> tmp(total);
        ok = inflate_exact(vm_base + in_ea, in_size, tmp.data(), total);
        if (ok) memcpy(vm_base + out_ea, tmp.data() + skip_begin, out_size);
    }
    if (!ok) {
        /* The task inflates it again and writes every output byte. */
        if (s_bad++ < 8)
            fprintf(stderr,
                    "[edgezlib] chunk in=0x%08X (%u B) out=0x%08X (%u B, skip %u/%u) "
                    "did not inflate cleanly here; left to the SPU task\n",
                    in_ea, in_size, out_ea, out_size, skip_begin, skip_end);
        return 0;
    }
    s_ns +=
        (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    s_bytes_in += in_size;
    s_bytes_out += out_size;
    if (++s_native % 2000 == 0) report();   /* runs are often killed, not exited */

    /* The copy the task is left: 16 bytes of the output onto themselves, from
     * a 16-byte-aligned spot when the output has one. */
    uint32_t at = (out_ea + 15u) & ~15u, n = 16;
    if (at + n > out_ea + out_size) {
        at = out_ea;
        n = out_size < 16 ? out_size : 16;
    }
    req[0] = at;
    req[1] = at;
    req[2] = n;
    req[3] = n;
    req[4] &= ~1u;                          /* copy */
    req[6] &= 0xFFFF0000u;                  /* event flag bits stay; no skips */
    req[7] = 0;
    return 1;
}
