/*
 * The port's own HLE handlers, registered before the toolkit's.
 *
 * main.cpp calls ps3_load_prx_modules once, after the lifted function table
 * is registered and vm_base is live, before the game runs, and before
 * ppu_hle_init and ppu_sysprx_register: handlers registered here win over
 * theirs. No PRX image is loaded; the SPURS queues run libsre's own code,
 * lifted at build time (tools/gen_libsre.py).
 */

extern "C" void dod3_register_spurs_lfqueue(void);   /* src/spurs_lfqueue.cpp */
extern "C" void dod3_register_spurs_queue(void);     /* src/spurs_queue.cpp */

extern "C" void ps3_load_prx_modules(void)
{
    dod3_register_spurs_lfqueue();
    dod3_register_spurs_queue();
}
