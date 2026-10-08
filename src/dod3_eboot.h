/* The EBOOT this build is lifted from, and every address in it that the
 * port's own code knows.
 *
 *   DOD3_EBOOT=100  BLUS31197 1.00, the disc (the default; recompiled/)
 *   DOD3_EBOOT=101  the 1.01 update (recompiled_101/), beside it:
 *                   cmake -B build_101 -G Ninja -DDOD3_EBOOT=101
 *
 * 1.01 is the same compiler's build of the same code with ~450 functions
 * changed (the Japanese voice DLC, French text, a few fixes), so most code
 * moved rather than changed. Its column was found with
 * tools/eboot_diff.py (1.00 -> 1.01 function map), tools/addr_map.py
 * (code addresses; each checked to sit in code that did not change) and
 * tools/data_map.py (data addresses, from the code that forms them).
 *
 * The SPU side needs nothing here: the 40 SPU programs and both raw SPURS
 * jobs are byte-identical in 1.01, and the runtime finds them by content.
 * libsre's lift (tools/gen_libsre.py) is the firmware module at its own
 * base, not the EBOOT's copy. */
#ifndef DOD3_EBOOT_H
#define DOD3_EBOOT_H

#ifndef DOD3_EBOOT
#define DOD3_EBOOT 100
#endif

#if DOD3_EBOOT == 101
#define DOD3_EBOOT_NAME "1.01"
/* lifted functions the port calls or replaces */
#define DOD3_FN_GC_REACH              func_00EE7728          /* FArchiveRealtimeGC reachability (src/dod3_gc_native.cpp) */
#define DOD3_FN_GC_REACH_LIFTED       func_00EE7728_lifted
#define DOD3_FN_COLLECT_GARBAGE       func_000C1E50
#define DOD3_FN_COLLECT_GARBAGE_LIFTED func_000C1E50_lifted
#define DOD3_FN_PERIODIC_GC           func_0041C170
#define DOD3_FN_COLLISION_TEST        func_00272E58          /* src/dod3_hot.cpp */
#define DOD3_FN_COLLISION_TEST_LIFTED func_00272E58_lifted
#define DOD3_FN_GET_MAX_TICK_RATE     func_00424380          /* UGameEngine::GetMaxTickRate, the title's own */
#define DOD3_FN_APP_REALLOC           func_0001028C
#define DOD3_FN_EXEC_GETSTRING        func_0145E274          /* USqex03DataMessage::execGetString */
#define DOD3_FN_EXEC_BRIDGE           func_013F43A0          /* USqex03GameOption::execUpdateDisplayParam */
#define DOD3_FN_GMALLOC_CREATE        func_008EBD40
#define DOD3_FN_TARRAY_ADDITEM        func_00ECEF6C
#define DOD3_FN_GC_PREPARE            func_000C1D48
#define DOD3_FN_GC_REACH_A            func_00EE7550
#define DOD3_FN_GC_REACH_B            func_00EE75F8
#define DOD3_FN_APP_ERRORF            func_00012538
/* code addresses */
#define DOD3_A_EXEC_GETSTRING         0x0145E274u
#define DOD3_A_EXEC_BRIDGE            0x013F43A0u
#define DOD3_A_FAST_POLL_LR           0x008B0D18u            /* the render thread's fence poll */
#define DOD3_A_PERIODIC_GC_RET        0x0041C1C8u
#define DOD3_A_LIMITER_SLEEP_RET      0x008EDE80u
#define DOD3_A_INFLATE_PUSH_LR        0x00AC8768u            /* edgeZlibAddInflateQueueElement's LFQueue push */
#define DOD3_GC_CODE_SHIFT            0x11F0u                /* func_00EE6538 and its constants, 1.00 -> 1.01 */
/* data */
#define DOD3_A_FPS_NUMERATOR          0x008EDBCCu            /* 1.0f in the frame limiter */
#define DOD3_A_GENGINE                0x01999164u
#define DOD3_A_SHA_TABLE_LO           0x019AD000u
#define DOD3_A_SHA_TABLE_HI           0x019AD480u
#define DOD3_A_GNATIVES               0x019BF2F0u
#define DOD3_A_GWORLD                 0x0199B880u
#define DOD3_A_GOBJOBJECTS            0x01A0C234u            /* TArray<UObject*>: data, num */
#define DOD3_A_GOBJ_FIRST_GC_INDEX    0x01A0C2BCu
#define DOD3_A_GC_VISIT_COUNTER       0x019C80ECu
#define DOD3_A_PERM_OBJ_START         0x019907CCu
#define DOD3_A_PERM_OBJ_END           0x019907D0u
#define DOD3_A_GMALLOC                0x0197FFA0u
#define DOD3_A_GERROR                 0x0197FF84u
#define DOD3_A_GC_ERROR_FMT           0x01639D0Cu
#define DOD3_A_GCM_CACHE64            0x01A2A150u            /* DOD3_GCM_WATCH diagnostics only */
#define DOD3_A_GCM_CONTEXT            0x01AC3E38u
#define DOD3_A_SHADER_JOB_CHAIN       0x01A2A800u
#define DOD3_A_SHADER_JOB_DESCS       0x01A2B980u
#else
#define DOD3_EBOOT_NAME "1.00"
#define DOD3_FN_GC_REACH              func_00EE6538
#define DOD3_FN_GC_REACH_LIFTED       func_00EE6538_lifted
#define DOD3_FN_COLLECT_GARBAGE       func_000C1E50
#define DOD3_FN_COLLECT_GARBAGE_LIFTED func_000C1E50_lifted
#define DOD3_FN_PERIODIC_GC           func_0041C270
#define DOD3_FN_COLLISION_TEST        func_00272F58
#define DOD3_FN_COLLISION_TEST_LIFTED func_00272F58_lifted
#define DOD3_FN_GET_MAX_TICK_RATE     func_00424460
#define DOD3_FN_APP_REALLOC           func_0001028C
#define DOD3_FN_EXEC_GETSTRING        func_0145D030
#define DOD3_FN_EXEC_BRIDGE           func_013F31B0
#define DOD3_FN_GMALLOC_CREATE        func_008EBDD0
#define DOD3_FN_TARRAY_ADDITEM        func_00ECDD7C
#define DOD3_FN_GC_PREPARE            func_000C1D48
#define DOD3_FN_GC_REACH_A            func_00EE6360
#define DOD3_FN_GC_REACH_B            func_00EE6408
#define DOD3_FN_APP_ERRORF            func_00012538
#define DOD3_A_EXEC_GETSTRING         0x0145D030u
#define DOD3_A_EXEC_BRIDGE            0x013F31B0u
#define DOD3_A_FAST_POLL_LR           0x008B0DD8u
#define DOD3_A_PERIODIC_GC_RET        0x0041C2C8u
#define DOD3_A_LIMITER_SLEEP_RET      0x008EDF10u
#define DOD3_A_INFLATE_PUSH_LR        0x00AC7568u
#define DOD3_GC_CODE_SHIFT            0u
#define DOD3_A_FPS_NUMERATOR          0x008EDC5Cu
#define DOD3_A_GENGINE                0x01999164u
#define DOD3_A_SHA_TABLE_LO           0x019AD000u
#define DOD3_A_SHA_TABLE_HI           0x019AD480u
#define DOD3_A_GNATIVES               0x019BF370u
#define DOD3_A_GWORLD                 0x0199B880u
#define DOD3_A_GOBJOBJECTS            0x01A0C2B4u
#define DOD3_A_GOBJ_FIRST_GC_INDEX    0x01A0C33Cu
#define DOD3_A_GC_VISIT_COUNTER       0x019C816Cu
#define DOD3_A_PERM_OBJ_START         0x019907CCu
#define DOD3_A_PERM_OBJ_END           0x019907D0u
#define DOD3_A_GMALLOC                0x0197FFA0u
#define DOD3_A_GERROR                 0x0197FF84u
#define DOD3_A_GC_ERROR_FMT           0x016386CCu
#define DOD3_A_GCM_CACHE64            0x01A2A1D0u
#define DOD3_A_GCM_CONTEXT            0x01AC3E38u
#define DOD3_A_SHADER_JOB_CHAIN       0x01A2A880u
#define DOD3_A_SHADER_JOB_DESCS       0x01A2BA00u
#endif

/* A code address inside func_00EE6538 (the GC's reachability pass), written
 * as its 1.00 value. */
#define DOD3_GC_PC(a) ((uint32_t)(a) + DOD3_GC_CODE_SHIFT)

#endif /* DOD3_EBOOT_H */
