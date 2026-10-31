/* The EBOOT this build is lifted from, and every address in it that the
 * port's own code knows.
 *
 *   DOD3_EBOOT=101  the 1.01 update (recompiled_101/): the default, and
 *                   the version the port is built and tested for
 *   DOD3_EBOOT=100  BLUS31197 1.00, the disc (recompiled/), beside it:
 *                   cmake -B build_100 -G Ninja -DDOD3_EBOOT=100
 *                   (kept for comparison; not maintained)
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
#define DOD3_EBOOT 101
#endif

#if DOD3_EBOOT == 101
#define DOD3_EBOOT_NAME "1.01"
/* lifted functions the port calls or replaces */
#define DOD3_FN_GC_REACH               func_00EE7728 /* FArchiveRealtimeGC reachability (src/patches/gc_native.cpp) */
#define DOD3_FN_GC_REACH_LIFTED        func_00EE7728_lifted
#define DOD3_FN_COLLECT_GARBAGE        func_000C1E50
#define DOD3_FN_COLLECT_GARBAGE_LIFTED func_000C1E50_lifted
#define DOD3_FN_PERIODIC_GC            func_0041C170
#define DOD3_FN_COLLISION_TEST         func_00272E58 /* src/cpu/hot.cpp */
#define DOD3_FN_COLLISION_TEST_LIFTED  func_00272E58_lifted
#define DOD3_FN_GET_MAX_TICK_RATE      func_00424380 /* UGameEngine::GetMaxTickRate, the title's own */
#define DOD3_FN_PROCESS_ASYNC_LOADING  func_0005E59C /* UObject::ProcessAsyncLoading (src/patches/load_idle.cpp) */
#define DOD3_FN_APP_REALLOC            func_0001028C
#define DOD3_FN_EXEC_GETSTRING         func_0145E274 /* USqex03DataMessage::execGetString */
#define DOD3_FN_EXEC_BRIDGE            func_013F43A0 /* USqex03GameOption::execUpdateDisplayParam */
#define DOD3_FN_GMALLOC_CREATE         func_008EBD40
#define DOD3_FN_TARRAY_ADDITEM         func_00ECEF6C
#define DOD3_FN_GC_PREPARE             func_000C1D48
#define DOD3_FN_GC_REACH_A             func_00EE7550
#define DOD3_FN_GC_REACH_B             func_00EE75F8
#define DOD3_FN_APP_ERRORF             func_00012538
#define DOD3_FN_TREE_CREATE_PROXY      func_00817FD4 /* USpeedTreeComponent::CreateSceneProxy (patches/sysset.cpp) */
#define DOD3_FN_CANVAS_PROJECT         func_00643480 /* UCanvas::execProject (patches/hud_aspect.cpp) */
#define DOD3_FN_HUD_CALC_TRANSFORM     func_009DEAB8 /* ASqex03GameHUD::CalcTransformPosition (patches/hud_aspect.cpp) */
#define DOD3_FN_SONG_JUDGE             func_0091D8F0 /* the note judge of branch D's final song (patches/song_autoplay.cpp) */
#define DOD3_FN_SET_DEPTH_OF_FIELD     func_00A54554 /* the title's own switches of FSystemSettings (patches/sysset.cpp) */
#define DOD3_FN_SET_MOTION_BLUR        func_00A54578
#define DOD3_FN_FLIP_DEPTH_OF_FIELD    func_00A54720
#define DOD3_FN_FLIP_MOTION_BLUR       func_00A5474C
#define DOD3_FN_DRAGON_SHOOTING_CURSOR func_009D26F8 /* ASqex03GamePawnPlayerDragon::UpdateShootingCursor (patches/dragon_cursor.cpp) */
#define DOD3_FN_SHOOTING_CURSOR_ON     func_0096B9A8 /* ASqex03GamePawn::IsEnableShootingCursor */
#define DOD3_FN_UBER_CREATE_PROXY      func_00306728 /* UUberPostProcessEffect::CreateSceneProxy (patches/sysset.cpp) */
#define DOD3_FN_DOFBLOOM_CREATE_PROXY  func_001B1B64 /* UDOFAndBloomEffect::CreateSceneProxy */
#define DOD3_FN_HUD_BLOOD_DRAW         func_009E5150 /* USqex03GameHUDBlood::Draw (patches/sysset.cpp) */
#define DOD3_FN_HUD_DYING_UPDATE       func_009E8028 /* USqex03GameHUDDying::Update */
/* code addresses */
#define DOD3_A_EXEC_GETSTRING    0x0145E274u
#define DOD3_A_EXEC_BRIDGE       0x013F43A0u
#define DOD3_A_FAST_POLL_LR      0x008B0D18u /* the render thread's fence poll */
#define DOD3_A_PERIODIC_GC_RET   0x0041C1C8u
#define DOD3_A_LIMITER_SLEEP_RET 0x008EDE80u
#define DOD3_A_INFLATE_PUSH_LR   0x00AC8768u /* edgeZlibAddInflateQueueElement's LFQueue push */
#define DOD3_GC_CODE_SHIFT       0x11F0u /* func_00EE6538 and its constants, 1.00 -> 1.01 */
/* data */
#define DOD3_A_FPS_NUMERATOR        0x008EDBCCu /* 1.0f in the frame limiter */
#define DOD3_A_GENGINE              0x01999164u
#define DOD3_A_GOBJ_ASYNC_PACKAGES  0x01A0C2A0u /* GObjAsyncPackages, a TArray: data, num, max (src/patches/load_idle.cpp) */
#define DOD3_A_RENDER_IDLE_TICK_HZ  0x01991234u /* GRenderingThreadMaxIdleTickFrequency (40.0f) */
#define DOD3_A_SHA_TABLE_LO         0x019AD000u
#define DOD3_A_SHA_TABLE_HI         0x019AD480u
#define DOD3_A_GNATIVES             0x019BF2F0u
#define DOD3_A_GWORLD               0x0199B880u
#define DOD3_A_GOBJOBJECTS          0x01A0C234u /* TArray<UObject*>: data, num */
#define DOD3_A_GOBJ_FIRST_GC_INDEX  0x01A0C2BCu
#define DOD3_A_GC_VISIT_COUNTER     0x019C80ECu
#define DOD3_A_PERM_OBJ_START       0x019907CCu
#define DOD3_A_PERM_OBJ_END         0x019907D0u
#define DOD3_A_GMALLOC              0x0197FFA0u
#define DOD3_A_GERROR               0x0197FF84u
#define DOD3_A_GC_ERROR_FMT         0x01639D0Cu
#define DOD3_A_SYSTEM_SETTINGS_DATA 0x01A12D8Cu /* GSystemSettings + 4 (src/patches/sysset.cpp) */
#define DOD3_A_TREE_CREATE_PROXY    0x00817FD4u
#define DOD3_A_CANVAS_PROJECT       0x00643480u
#define DOD3_A_HUD_CALC_TRANSFORM   0x009DEAB8u
#define DOD3_A_SONG_JUDGE           0x0091D8F0u
#define DOD3_A_SET_DEPTH_OF_FIELD   0x00A54554u
#define DOD3_A_SET_MOTION_BLUR      0x00A54578u
#define DOD3_A_FLIP_DEPTH_OF_FIELD  0x00A54720u
#define DOD3_A_FLIP_MOTION_BLUR     0x00A5474Cu
#define DOD3_A_DRAGON_SHOOTING_CURSOR 0x009D26F8u
#define DOD3_A_UBER_CREATE_PROXY    0x00306728u
#define DOD3_A_DOFBLOOM_CREATE_PROXY 0x001B1B64u
#define DOD3_A_HUD_BLOOD_DRAW       0x009E5150u
#define DOD3_A_HUD_DYING_UPDATE     0x009E8028u
#define DOD3_O_HUD_DYING_ALPHA      0xB4u   /* USqex03GameHUDDying: m_fFrameColor */
#define DOD3_O_DOF_PROXY_BLUR_NEAR  0x14u   /* FDOFPostProcessSceneProxy: MaxNearBlurAmount (MaxFar at +0x1C) */
#define DOD3_O_SHOOTING_CURSOR      0x13F4u /* ASqex03GamePawn: m_xTarget.m_xShooting.m_vCurPos, (x, y) of the screen */
#define DOD3_O_SHOOTING_RECT        0x1448u /* m_xTarget.m_xShooting.m_fShootingRect_Up, _Bottom, _Left, _Right */
#define DOD3_O_HUD_CANVAS          0x414u /* ASqex03GameHUD: AHUD::Canvas */
#define DOD3_O_HUD_SCREEN_POS       0x748u /* ASqex03GameHUD: CalcTransformPosition's results, a TArray of (x, y) */
#define DOD3_A_TREE_PROXY_VTABLE    0x016E8C34u /* FSpeedTreeSceneProxy */
#else
#define DOD3_EBOOT_NAME                "1.00"
#define DOD3_FN_GC_REACH               func_00EE6538
#define DOD3_FN_GC_REACH_LIFTED        func_00EE6538_lifted
#define DOD3_FN_COLLECT_GARBAGE        func_000C1E50
#define DOD3_FN_COLLECT_GARBAGE_LIFTED func_000C1E50_lifted
#define DOD3_FN_PERIODIC_GC            func_0041C270
#define DOD3_FN_COLLISION_TEST         func_00272F58
#define DOD3_FN_COLLISION_TEST_LIFTED  func_00272F58_lifted
#define DOD3_FN_GET_MAX_TICK_RATE      func_00424460
#define DOD3_FN_APP_REALLOC            func_0001028C
#define DOD3_FN_EXEC_GETSTRING         func_0145D030
#define DOD3_FN_EXEC_BRIDGE            func_013F31B0
#define DOD3_FN_GMALLOC_CREATE         func_008EBDD0
#define DOD3_FN_TARRAY_ADDITEM         func_00ECDD7C
#define DOD3_FN_GC_PREPARE             func_000C1D48
#define DOD3_FN_GC_REACH_A             func_00EE6360
#define DOD3_FN_GC_REACH_B             func_00EE6408
#define DOD3_FN_APP_ERRORF             func_00012538
#define DOD3_A_EXEC_GETSTRING          0x0145D030u
#define DOD3_A_EXEC_BRIDGE             0x013F31B0u
#define DOD3_A_FAST_POLL_LR            0x008B0DD8u
#define DOD3_A_PERIODIC_GC_RET         0x0041C2C8u
#define DOD3_A_LIMITER_SLEEP_RET       0x008EDF10u
#define DOD3_A_INFLATE_PUSH_LR         0x00AC7568u
#define DOD3_GC_CODE_SHIFT             0u
#define DOD3_A_FPS_NUMERATOR           0x008EDC5Cu
#define DOD3_A_GENGINE                 0x01999164u
#define DOD3_A_RENDER_IDLE_TICK_HZ     0x01991234u /* as 1.01's: the data is laid out alike (checked by its value) */
#define DOD3_A_SHA_TABLE_LO            0x019AD000u
#define DOD3_A_SHA_TABLE_HI            0x019AD480u
#define DOD3_A_GNATIVES                0x019BF370u
#define DOD3_A_GWORLD                  0x0199B880u
#define DOD3_A_GOBJOBJECTS             0x01A0C2B4u
#define DOD3_A_GOBJ_FIRST_GC_INDEX     0x01A0C33Cu
#define DOD3_A_GC_VISIT_COUNTER        0x019C816Cu
#define DOD3_A_PERM_OBJ_START          0x019907CCu
#define DOD3_A_PERM_OBJ_END            0x019907D0u
#define DOD3_A_GMALLOC                 0x0197FFA0u
#define DOD3_A_GERROR                  0x0197FF84u
#define DOD3_A_GC_ERROR_FMT            0x016386CCu
#define DOD3_A_SYSTEM_SETTINGS_DATA    0x01A12E0Cu
#endif

/* A code address inside func_00EE6538 (the GC's reachability pass), written
 * as its 1.00 value. */
#define DOD3_GC_PC(a) ((uint32_t)(a) + DOD3_GC_CODE_SHIFT)

#endif /* DOD3_EBOOT_H */
