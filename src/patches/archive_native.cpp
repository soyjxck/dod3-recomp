/*
 * FArchiveAsync::Serialize natively, for the bytes that are already there.
 *
 * Every value Unreal's package loader reads -- each property, each name
 * index, a byte at a time as often as a block -- is a virtual call to the
 * linker's archive, FArchiveAsync::Serialize(Data, Count): if the bytes are
 * inside the region the archive has precached and that read has come back,
 * copy them out of the precache buffer (appMemcpy) and move the position on;
 * otherwise ask for them (Precache) and wait. Lifted, the common case was a
 * function of 90 guest instructions and then the guest's memcpy, both with a
 * page-table test on every load and store: the two were a fifth of the game
 * thread in a blocking load (the New Game frame of 400 ms, the start-up), and
 * the most frequent leaves of every load.
 *
 * Here the common case is one memcpy. Anything else -- bytes outside the
 * precached region, a read still in flight, guest pages the lifted code would
 * not touch inline (a diagnostic's watch, an SPU reservation), the two ranges
 * overlapping -- runs the lifted function as before. The archive's fields
 * (1.01, the same layout in UE3's source):
 *   +0x98 CurrentPos            +0x9C PrecacheStartPos[CURRENT]
 *   +0xA4 PrecacheEndPos[CURRENT]  +0xAC PrecacheBuffer[CURRENT]
 *   +0xB4 PrecacheReadStatus[CURRENT] (the I/O thread's counter, 0 when done)
 *
 * The linker reads through its own FArchive, FLinkerLoad::Serialize, which
 * only passes the call on to its archive (Loader, at +0x498): a second
 * virtual call for every read. When the archive's Serialize is the one here,
 * the linker's goes straight to it; and a name -- the reference the package
 * format is made of, an index and a number -- is read in one step.
 *
 * DOD3_ARCHIVE_NATIVE=0 turns all three off.
 */
#include "cpu/ppu.h"
#include "cpu/eboot.h"
#include "ppu_vm_page.h"   /* g_ppu_vm_page: which pages the inline accessors may touch */
#include <stdlib.h>
#include <string.h>
#include <atomic>

#ifdef DOD3_FN_ARCHIVE_ASYNC_SERIALIZE
namespace {

std::atomic<uint64_t> s_native{ 0 }, s_lifted{ 0 };

/* Every 64 KiB page of [a, a + n) has `bit` (PPU_VM_PAGE_RD: loads may be
 * inline; PPU_VM_PAGE_ST: stores may). */
inline bool pages(uint32_t a, uint32_t n, unsigned bit)
{
    if (!n) return true;
    const uint64_t last = (uint64_t)a + n - 1;
    if (last > 0xFFFFFFFFull) return false;
    for (uint32_t p = a >> 16; p <= (uint32_t)(last >> 16); p++)
        if (!(__atomic_load_n(&g_ppu_vm_page[p], __ATOMIC_RELAXED) & bit)) return false;
    return true;
}

inline uint32_t be32(uint32_t a)
{
    uint32_t v;
    memcpy(&v, vm_base + a, 4);
    return __builtin_bswap32(v);
}
inline uint64_t be64(uint32_t a)
{
    uint64_t v;
    memcpy(&v, vm_base + a, 8);
    return __builtin_bswap64(v);
}
inline void put32(uint32_t a, uint32_t v)
{
    v = __builtin_bswap32(v);
    memcpy(vm_base + a, &v, 4);
}

/* The function in slot 2 of an object's vtable (Serialize, in an FArchive):
 * the code address in the descriptor the slot holds, or 0. */
inline uint32_t vslot2(uint32_t obj)
{
    if (!obj || !pages(obj, 4, PPU_VM_PAGE_RD)) return 0;
    const uint32_t vtable = be32(obj);
    if (!vtable || !pages(vtable + 8, 4, PPU_VM_PAGE_RD)) return 0;
    const uint32_t opd = be32(vtable + 8);
    return opd && pages(opd, 4, PPU_VM_PAGE_RD) ? be32(opd) : 0;
}

/* An FArchiveAsync's next n bytes, when they are inside its precache buffer
 * and that read has come back: where they are, and the position they start
 * at. The position is the caller's to move on. */
inline bool async_ready(uint32_t ar, uint32_t n, uint32_t* src, int32_t* pos)
{
    if (!pages(ar + 0x98, 0x20, PPU_VM_PAGE_RD | PPU_VM_PAGE_ST)) return false;
    const int32_t p = (int32_t)be32(ar + 0x98);
    const int32_t start = (int32_t)be32(ar + 0x9C);
    const int32_t end = (int32_t)be32(ar + 0xA4);
    /* The I/O thread's counter, read before the buffer it guards. */
    uint32_t status;
    __atomic_load(reinterpret_cast<uint32_t*>(vm_base + ar + 0xB4), &status, __ATOMIC_ACQUIRE);
    if (p < start || (int64_t)p + n > end || status != 0) return false;
    *src = be32(ar + 0xAC) + (uint32_t)(p - start);
    *pos = p;
    return pages(*src, n, PPU_VM_PAGE_RD);
}

/* The FArchiveAsync a linker's archive (`lar`, the FArchive inside the
 * FLinkerLoad) reads through -- its Loader -- when that one's Serialize is
 * the one here; 0 otherwise. */
inline uint32_t linker_async(uint32_t lar)
{
    if (!pages(lar + 0x498, 4, PPU_VM_PAGE_RD)) return 0;
    const uint32_t loader = be32(lar + 0x498);
    return vslot2(loader) == DOD3_A_ARCHIVE_ASYNC_SERIALIZE ? loader : 0;
}

void hook_archive_async_serialize(ppu_context* ctx)
{
    const uint32_t ar = (uint32_t)ctx->gpr[3];
    const uint32_t data = (uint32_t)ctx->gpr[4];
    const int32_t count = (int32_t)ctx->gpr[5];
    uint32_t src;
    int32_t pos;
    if (count >= 0 && async_ready(ar, (uint32_t)count, &src, &pos)) {
        const uint64_t s0 = src, d0 = data, n = (uint32_t)count;
        if ((d0 + n <= s0 || s0 + n <= d0) && pages(data, (uint32_t)n, PPU_VM_PAGE_ST)) {
            memcpy(vm_base + data, vm_base + src, (size_t)n);
            put32(ar + 0x98, (uint32_t)(pos + count));
            ctx->gpr[3] = (uint32_t)(pos + count);   /* what the lifted one leaves in r3 */
            s_native.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    s_lifted.fetch_add(1, std::memory_order_relaxed);
    DOD3_FN_ARCHIVE_ASYNC_SERIALIZE(ctx);
    dod3_drain(ctx);
}

/* FLinkerLoad::Serialize(Data, Count): Loader->Serialize(Data, Count). */
void hook_linker_serialize(ppu_context* ctx)
{
    if (const uint32_t loader = linker_async((uint32_t)ctx->gpr[3])) {
        ctx->gpr[3] = loader;
        hook_archive_async_serialize(ctx);
        return;
    }
    DOD3_FN_LINKER_SERIALIZE(ctx);
    dod3_drain(ctx);
}

/* FLinkerLoad::operator<<(FName&), through the FArchive's thunk (this is the
 * archive, at linker+0x16C): the name's index into the package's name map
 * and its number, two Serialize calls of 4 bytes, then the map's entry (an
 * FName: index and number) gives the name -- or NAME_None, for an entry of 0.
 * An index outside the map is the lifted function's to report (appErrorf). */
void hook_linker_name(ppu_context* ctx)
{
    const uint32_t lar = (uint32_t)ctx->gpr[3];
    const uint32_t name = (uint32_t)ctx->gpr[4];
    const uint32_t linker = lar - 0x16C;
    uint32_t async, src;
    int32_t pos;
    if (vslot2(lar) == DOD3_A_LINKER_SERIALIZE && (async = linker_async(lar)) && async_ready(async, 8, &src, &pos) &&
        pages(linker + 0xE4, 8, PPU_VM_PAGE_RD) && pages(name, 8, PPU_VM_PAGE_ST)) {
        const int32_t index = (int32_t)be32(src);
        const uint32_t number = be32(src + 4);
        if (index >= 0 && index < (int32_t)be32(linker + 0xE8)) {
            const uint32_t entry = be32(linker + 0xE4) + ((uint32_t)index << 3);
            if (pages(entry, 8, PPU_VM_PAGE_RD)) {
                const bool none = be64(entry) == 0;
                put32(name, none ? 0 : be32(entry));
                put32(name + 4, none ? 0 : number);
                put32(async + 0x98, (uint32_t)(pos + 8));
                ctx->gpr[3] = lar;
                s_native.fetch_add(2, std::memory_order_relaxed);
                return;
            }
        }
    }
    DOD3_FN_LINKER_NAME(ctx);
    dod3_drain(ctx);
}

}  // namespace
#endif

/* How many reads took each path since the last call (DOD3_LOAD_LOG). */
extern "C" void dod3_archive_native_counts(uint64_t* native, uint64_t* lifted)
{
#ifdef DOD3_FN_ARCHIVE_ASYNC_SERIALIZE
    *native = s_native.exchange(0, std::memory_order_relaxed);
    *lifted = s_lifted.exchange(0, std::memory_order_relaxed);
#else
    *native = *lifted = 0;
#endif
}

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));

void dod3_archive_native_install()
{
#ifdef DOD3_FN_ARCHIVE_ASYNC_SERIALIZE
    const char* e = getenv("DOD3_ARCHIVE_NATIVE");
    if (e && *e == '0') return;
    ppu_register_function(DOD3_A_ARCHIVE_ASYNC_SERIALIZE, hook_archive_async_serialize);
    ppu_register_function(DOD3_A_LINKER_SERIALIZE, hook_linker_serialize);
    ppu_register_function(DOD3_A_LINKER_NAME, hook_linker_name);
#endif
}
