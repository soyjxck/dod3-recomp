/*
 * Text that is uncovered a letter at a time, at frame rates above the
 * title's own.
 *
 * A talk window's lines, a mission's result messages and a quest's outline
 * in the book are a USqex03GameMessage: each letter has a value that runs
 * from 0 to 1 and the letters start one after another, so the text fades in
 * from its first letter to its last. Every frame the message's Update (the
 * native's work is func_009FCB78, vtable slot 0x128) is handed the frame's
 * time in 1/60 s and a step, 1 letter or, in the book, 6:
 *
 *   - a letter that has started gains a twentieth of the frame's time, so
 *     it is full a third of a second later: by the frame's time, the same at
 *     any frame rate;
 *   - a letter starts when the one a step before it has (the first is
 *     started by SetMessage, at 0.1): one step a call, whatever the frame's
 *     time. At the console's 30 frames a second that is 30 steps a second;
 *     at 60 the wave runs twice as fast, at 120 four times, and a long line
 *     is all there almost at once.
 *
 * At 30 frames a second a call adds 0.1, so a letter always stands 0.1
 * behind the one a step before it. Here that is the rule itself: a letter is
 * at least the value of the one a step before it less 0.1, and gains the
 * frame's share once it is above 0 -- the same values frame for frame at 30,
 * and the same wave in time at any other rate. It is the rule of the
 * frame-rate patch RPCS3 ships for the title, which replaces the function as
 * this does (it is reached through the vtable only, so the function registry
 * replaces it without a re-lift).
 *
 * DOD3_TEXT_REVEAL=0 keeps the title's own behaviour. DOD3_TEXT_REVEAL_LOG=1
 * prints each text once its last letter is full: its letters, the step, and
 * the time and calls it took.
 *
 * Measured on the 1.01 EBOOT in the book's Quests tab, a quest's outline of
 * 110 letters, 6 a step. As the title has it: 0.90 s at 30 fps (28 calls),
 * 0.62 s at 60, 0.48 s at 120. With the rule here: 0.90 s at 30 (the same
 * 28 calls), 0.92 s at 60, 0.93 s at 120; outlines of 123 to 146 letters
 * likewise, 0.97 to 1.13 s at every rate.
 */
#include "cpu/ppu.h"
#include "cpu/eboot.h"   /* the EBOOT version's addresses */
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DOD3_FN_TEXT_REVEAL
namespace {

const uint32_t kValues = 0x44;   /* the letters' values, floats */
const uint32_t kCount = 0x50;    /* how many */
const float kShare = 0.05f;      /* of the frame's time in 1/60 s, a call */
const float kBehind = 0.1f;      /* a letter behind the one a step before it */

bool s_fix = true;   /* DOD3_TEXT_REVEAL=0: the title's own */
bool s_log;          /* DOD3_TEXT_REVEAL_LOG=1 */

float guest_float(uint32_t ea)
{
    const uint32_t w = vm_read32(ea);
    float f;
    memcpy(&f, &w, 4);
    return f;
}
void set_guest_float(uint32_t ea, float f)
{
    uint32_t w;
    memcpy(&w, &f, 4);
    vm_write32(ea, w);
}

/* DOD3_TEXT_REVEAL_LOG: the texts on their way */
struct Text {
    uint32_t self;
    std::chrono::steady_clock::time_point from;
    unsigned calls;
    float time;   /* the frames' time handed in, 1/60 s */
} s_texts[4];

void log_before(uint32_t self, uint32_t values, int n, float steps)
{
    if (n <= 0 || !values) return;
    Text* t = nullptr;
    for (Text& x : s_texts)
        if (x.self == self) t = &x;
    /* SetMessage has just set the first letter to 0.1, and the last (of more than one) is not started */
    const bool fresh = vm_read32(values) == 0x3DCCCCCDu && (n == 1 || vm_read32(values + 4 * (uint32_t)(n - 1)) == 0);
    if (fresh) {
        if (!t) {
            static unsigned s_next;
            t = &s_texts[s_next++ % 4];
        }
        *t = { self, std::chrono::steady_clock::now(), 0, 0.0f };
    }
    if (t) t->calls++, t->time += steps;
}
void log_after(uint32_t self, uint32_t values, int n, int step)
{
    if (n <= 0 || !values) return;
    for (Text& t : s_texts) {
        if (t.self != self || guest_float(values + 4 * (uint32_t)(n - 1)) < 1.0f) continue;
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t.from).count();
        fprintf(stderr, "[text-reveal] %d letters, %d a step: uncovered in %.2f s, %u calls (%.1f frames of 1/60 s)\n", n,
                step, s, t.calls, (double)t.time);
        t.self = 0;
    }
}

void hook_reveal(ppu_context* ctx)
{
    const uint32_t self = (uint32_t)ctx->gpr[3];
    const float steps = (float)ctx->fpr[1];
    const int step = (int32_t)ctx->gpr[5];
    const uint32_t values = vm_read32(self + kValues);
    const int n = (int32_t)vm_read32(self + kCount);
    if (s_log) log_before(self, values, n, steps);
    if (!s_fix) {
        DOD3_FN_TEXT_REVEAL(ctx);
        dod3_drain(ctx);
    } else if (n > 0 && values) {
        const float gain = steps * kShare;
        for (int i = 0; gain > 0.0f && i < n; i++) {
            const uint32_t ea = values + 4 * (uint32_t)i;
            float v = guest_float(ea);
            if (i == 0 || v > 0.0f) v += gain;
            if (i != 0) {
                const int j = i - step > 0 ? i - step : 0;
                if (j < n) {
                    const float lead = guest_float(values + 4 * (uint32_t)j) - kBehind;
                    if (lead > v) v = lead;
                }
            }
            set_guest_float(ea, v < 1.0f ? v : 1.0f);
        }
    }
    if (s_log) log_after(self, values, n, step);
}

}  // namespace
#endif

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));

void dod3_text_reveal_install()
{
#ifdef DOD3_FN_TEXT_REVEAL
    if (const char* e = getenv("DOD3_TEXT_REVEAL")) s_fix = strcmp(e, "0") != 0;
    if (const char* e = getenv("DOD3_TEXT_REVEAL_LOG")) s_log = atoi(e) != 0;
    if (!s_fix && !s_log) return;
    ppu_register_function(DOD3_A_TEXT_REVEAL, hook_reveal);
    if (!s_fix) fprintf(stderr, "[text-reveal] text uncovered a letter at a time: the title's own (DOD3_TEXT_REVEAL=0)\n");
#endif
}
