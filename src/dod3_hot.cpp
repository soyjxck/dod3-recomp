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
#include "ppu_recomp.h"
#include "dod3_cycles.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);

namespace {
struct HotStat { const char* name; uint64_t calls, cycles; };
HotStat s_hot[] = { { "func_00272F58", 0, 0 } };

int hot_log(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("DOD3_HOT_LOG"); on = (e && *e != '0') ? 1 : 0; }
    return on;
}

void hot_report(void)
{
#ifdef _WIN32
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (!last) { last = now; return; }
    if (now - last < 5000) return;
    const double secs = (now - last) / 1000.0;
    last = now;
    for (HotStat& h : s_hot) {
        if (!h.calls) continue;
        fprintf(stderr, "[hot] %s: %.0f calls/s, %.0f cycles each, %.1f Mcycles/s\n",
                h.name, h.calls / secs, (double)h.cycles / (double)h.calls, h.cycles / secs / 1e6);
        h.calls = 0; h.cycles = 0;
    }
#endif
}

inline void drain(ppu_context* ctx)
{
    while (g_trampoline_fn) {
        void (*f)(void*) = g_trampoline_fn;
        g_trampoline_fn = 0;
        f((void*)ctx);
    }
}
}  // namespace

void func_00272F58(ppu_context* ctx)
{
    if (!hot_log()) { func_00272F58_lifted(ctx); return; }
    const uint64_t c0 = dod3_cycles();
    func_00272F58_lifted(ctx);
    drain(ctx);
    s_hot[0].cycles += dod3_cycles() - c0;
    s_hot[0].calls++;
    hot_report();
}
