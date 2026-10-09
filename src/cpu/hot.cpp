/*
 * Timing wrappers for hot lifted functions (lifted with --hook <addr>; the
 * lifted body is func_<addr>_lifted). DOD3_HOT_LOG=1 prints, every 5 s, the
 * calls per second and the cycles per call of each, so a code-generation
 * change can be measured on the function it targets rather than on the
 * whole frame.
 *
 *   func_00272F58  the float-heavy collision test that dominates the game
 *                  thread when scenery breaks up (32-37% of it there)
 */
#include "cpu/ppu.h"
#include "cpu/eboot.h"   /* the EBOOT version's addresses */
#include "cpu/cycles.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>

namespace {
struct HotStat {
    const char* name;
    uint64_t calls, cycles;
};
HotStat s_hot[] = { { "collision test", 0, 0 } };

int hot_log(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("DOD3_HOT_LOG");
        on = (e && *e != '0') ? 1 : 0;
    }
    return on;
}

void hot_report(void)
{
    static uint64_t last = 0;
    const uint64_t now = dod3_now_us();
    if (!last) {
        last = now;
        return;
    }
    if (now - last < 5000000) return;
    const double secs = (now - last) / 1e6;
    last = now;
    for (HotStat& h : s_hot) {
        if (!h.calls) continue;
        fprintf(stderr, "[hot] %s: %.0f calls/s, %.0f cycles each, %.1f Mcycles/s\n", h.name, h.calls / secs,
                (double)h.cycles / (double)h.calls, h.cycles / secs / 1e6);
        h.calls = 0;
        h.cycles = 0;
    }
}
}  // namespace

void DOD3_FN_COLLISION_TEST(ppu_context* ctx)
{
    if (!hot_log()) {
        DOD3_FN_COLLISION_TEST_LIFTED(ctx);
        return;
    }
    const uint64_t c0 = dod3_cycles();
    DOD3_FN_COLLISION_TEST_LIFTED(ctx);
    dod3_drain(ctx);
    s_hot[0].cycles += dod3_cycles() - c0;
    s_hot[0].calls++;
    hot_report();
}
