/*
 * For the port's native code that runs alongside the lifted PPU code: the
 * guest memory's base and the trampoline a lifted function may leave a tail
 * call in, with what finishes it.
 */
#pragma once
#include "ppu_recomp.h"

extern "C" uint8_t* vm_base;
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);

/* Finish any tail call a lifted body left in the trampoline (its caller
 * would run it next), so a call into lifted code returns when it is done. */
static inline void dod3_drain(ppu_context* ctx)
{
    while (g_trampoline_fn) {
        void (*f)(void*) = g_trampoline_fn;
        g_trampoline_fn = 0;
        f((void*)ctx);
    }
}
