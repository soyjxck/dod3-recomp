cmake_minimum_required(VERSION 3.20)
project(dod3
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "Recompiled PS3 game project (ps3recomp)"
)
if(APPLE)
    enable_language(OBJCXX)   # src/setup_mac.mm
endif()

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer Ninja if available
if(NOT CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{CMAKE_GENERATOR})
    message(STATUS "Tip: use -G Ninja for faster builds")
endif()

# ---------------------------------------------------------------------------
# The two directories this project is built from
# ---------------------------------------------------------------------------
# PS3RECOMP_DIR is the toolkit checkout: include/, runtime/, libs/, tools/.
# RECOMP_DIR is where the lifter's output for THIS game lives. They are
# separate on purpose -- a port keeps its generated code in its own tree and
# points at whatever toolkit checkout it is building against:
#
#   cmake -B build -G Ninja \
#         -DPS3RECOMP_DIR=/path/to/ps3recomp \
#         -DRECOMP_DIR=/path/to/my_game/recompiled
#
if(NOT DEFINED PS3RECOMP_DIR)
    # Default: the ps3recomp submodule in this repo
    set(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ps3recomp")
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

# The EBOOT the lift is from: 100 (BLUS31197 1.00, the disc; the default) or
# 101 (the 1.01 update, lifted into recompiled_101/). src/dod3_eboot.h holds
# every address the port's own code knows, per version. A 1.01 build sits
# beside the 1.00 one:  cmake -B build_101 -G Ninja -DDOD3_EBOOT=101
set(DOD3_EBOOT 100 CACHE STRING "EBOOT version the lift is from: 100 or 101")
set_property(CACHE DOD3_EBOOT PROPERTY STRINGS 100 101)
if(DOD3_EBOOT STREQUAL "101")
    set(_dod3_recomp_default "${CMAKE_CURRENT_SOURCE_DIR}/recompiled_101")
else()
    set(_dod3_recomp_default "${CMAKE_CURRENT_SOURCE_DIR}/recompiled")
endif()
set(RECOMP_DIR "${_dod3_recomp_default}" CACHE PATH
    "Directory holding the lifter's output: ppu_recomp.h and the lifted sources")
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)

message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")
message(STATUS "Lifted output:   ${RECOMP_DIR}")

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR
        "No ppu_recomp.h in ${RECOMP_DIR}.\n"
        "That header and the lifted sources beside it are what "
        "tools/ppu_lifter.py writes. Run the lifter, drop its output there, "
        "or point -DRECOMP_DIR= at wherever it already is.")
endif()

# The lifted translation units. A game's lift is hundreds of generated files,
# so they are globbed rather than listed; re-run CMake after a fresh lift.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
    "${RECOMP_DIR}/*.c"
    "${RECOMP_DIR}/*.cpp"
)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR
        "${RECOMP_DIR} has a ppu_recomp.h but no .c or .cpp beside it. The "
        "lifted functions and the function table live in those; without them "
        "there is nothing to run.")
endif()
list(LENGTH RECOMP_SOURCES RECOMP_SOURCE_COUNT)
message(STATUS "Lifted sources:  ${RECOMP_SOURCE_COUNT} file(s)")

# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
# Built from source as a subdirectory rather than imported as a prebuilt
# archive. A static library carries none of its own link requirements, so an
# imported one arrives without SDL2, pthreads, the Metal frameworks or the
# Winsock/XInput imports -- the link then fails on every platform for a
# different reason. add_subdirectory hands all of that over with the target.
# Symbols for the host code on Windows (DOD3_PROF, DOD3_STALL_MS in
# src/win_prof.cpp, and crash backtraces). The runtime's own option has to be
# in the cache before its CMakeLists reads it, hence here; the port's sources
# get /Z7 further down and the link gets /DEBUG.
option(DOD3_SYMBOLS "Debug info for the host code (Windows: /Z7 + /DEBUG)" ON)
if(WIN32 AND DOD3_SYMBOLS)
    set(PS3RECOMP_DEBUG_INFO ON CACHE BOOL "Compile the runtime with debug info" FORCE)
endif()
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                 EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# The HLE NID table
# ---------------------------------------------------------------------------
# Maps each firmware import NID onto the handler in the runtime that answers
# it. A port may keep a hand-curated one beside its lift; otherwise generate
# the canonical every-module table, which is what the toolkit's own boot
# harness uses.
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
    message(STATUS "HLE NID table:   ${HLE_NID_TABLE}")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
        COMMENT "Generating the HLE NID -> handler table")
    message(STATUS "HLE NID table:   generated (every module)")
endif()

# ---------------------------------------------------------------------------
# The game executable
# ---------------------------------------------------------------------------
# The PPU boot scaffold is compiled here rather than taken from the runtime
# library: every one of these files #includes the lifter's per-game
# ppu_recomp.h, so they can only be built against a particular lift.
set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)

# ---------------------------------------------------------------------------
# Lifted SPU images (optional)
# ---------------------------------------------------------------------------
# A title that runs SPU work gets its images lifted into <SPU_DIR>/<image>/ by
# tools/spu_lifter.py, with a registration TU generated by
# tools/build_spu_workloads.py. cellSpurs dispatches them by content
# fingerprint, so the generated table is keyed to the images THIS build ships:
# a table generated from another release registers signatures that can never
# match, which is worse than shipping none. Both the table and the job-module
# registry self-register from constructors and are therefore optional -- a tree
# that has not lifted its SPU images yet configures and links without them.
set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spu" CACHE PATH
    "Directory holding the lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")
option(PORT_ENABLE_SPU "Compile and register the title's lifted SPU images" ON)

set(SPU_SOURCES "")
if(PORT_ENABLE_SPU)
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
    if(SPU_LIFTED)
        list(LENGTH SPU_LIFTED SPU_IMAGE_COUNT)
        message(STATUS "Lifted SPU:      ${SPU_IMAGE_COUNT} image(s)")
        # The musttail indirect-branch fast path. Excluded from the runtime
        # library on purpose (see the SDK root CMakeLists): it must be built
        # here, by clang, or the tail call degrades to a plain call and a guest
        # loop that branches indirectly walks the thread stack off its guard
        # page a few thousand iterations in.
        list(APPEND SPU_SOURCES ${SPU_LIFTED}
             "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/dod3_spu_hooks.c"     # --native-hook targets
             "${CMAKE_CURRENT_SOURCE_DIR}/src/dod3_msdsp_hooks.c"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/dod3_mp3_native.c"
             "${CMAKE_CURRENT_SOURCE_DIR}/src/dod3_mp3dec.c")
        # Generated beside the lift; each is present only once its generator
        # has been run, so each is included only if it is actually here.
        foreach(gen IN ITEMS spu_workloads.c spu_overlays.c jobmods_register.c taskset_policy_bytes.c)
            if(EXISTS "${SPU_DIR}/${gen}")
                list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    else()
        message(STATUS "Lifted SPU:      none found in ${SPU_DIR}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Parts of libsre (SPURS LFQueue push, SPURS queue), lifted at build time
# ---------------------------------------------------------------------------
# src/spurs_lfqueue.cpp and src/spurs_queue.cpp drive Sony's own code out of
# libsre.prx; tools/gen_libsre.py lifts it into the build tree. The result is
# firmware-derived, so it is generated, never committed.
set(LIBSRE_PRX "${PS3RECOMP_DIR}/fw_spu/libsre.prx" CACHE FILEPATH
    "Decrypted libsre.prx the SPURS queue paths are lifted from")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.venv/bin/python")
    set(DOD3_PYTHON "${CMAKE_CURRENT_SOURCE_DIR}/.venv/bin/python")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.venv/Scripts/python.exe")   # a Windows venv
    set(DOD3_PYTHON "${CMAKE_CURRENT_SOURCE_DIR}/.venv/Scripts/python.exe")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(DOD3_PYTHON "${Python3_EXECUTABLE}")
endif()
set(LIBSRE_LIFTED_CPP "${CMAKE_CURRENT_BINARY_DIR}/gen/libsre_lifted.cpp")
add_custom_command(
    OUTPUT "${LIBSRE_LIFTED_CPP}"
    COMMAND "${DOD3_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_libsre.py"
            "${PS3RECOMP_DIR}" "${LIBSRE_PRX}"
            "${CMAKE_CURRENT_BINARY_DIR}/libsre" "${LIBSRE_LIFTED_CPP}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_libsre.py"
            "${LIBSRE_PRX}"
            "${PS3RECOMP_DIR}/tools/ppu_lifter.py"
            "${PS3RECOMP_DIR}/tools/lift_prx.py"
    COMMENT "Lifting libsre's SPURS queue paths")
set_source_files_properties("${LIBSRE_LIFTED_CPP}" PROPERTIES COMPILE_OPTIONS "-w;-ffp-contract=off")

if(WIN32)
    list(APPEND PORT_WIN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/win_prof.cpp"
                                 "${CMAKE_CURRENT_SOURCE_DIR}/src/setup_win.cpp")
elseif(APPLE)
    # The first-run setup's panels (AppKit), the counterpart of setup_win.cpp.
    list(APPEND PORT_WIN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/setup_mac.mm")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/setup_mac.mm"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()

# The installer (src/setup_install.h): the core on every platform; the pages
# (src/setup_wizard.cpp) in Dear ImGui (third_party/imgui, v1.92.9b), in a
# window from one layer per platform (src/setup_ui.h).
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
set(SETUP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/setup_install.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/setup_pkg.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/setup_crypto.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/setup_self.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/setup_keys.cpp")
set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp")
if(WIN32)
    list(APPEND IMGUI_SOURCES
        "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
        "${IMGUI_DIR}/backends/imgui_impl_dx12.cpp")
    list(APPEND SETUP_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/setup_wizard.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/setup_ui_win.cpp"
        ${IMGUI_SOURCES})
elseif(APPLE)
    # The Mac's window layer (src/setup_ui_mac.mm): a window with a Metal
    # view, ImGui's OSX and Metal backends (Objective-C++, ARC).
    list(APPEND IMGUI_SOURCES
        "${IMGUI_DIR}/backends/imgui_impl_osx.mm"
        "${IMGUI_DIR}/backends/imgui_impl_metal.mm")
    list(APPEND SETUP_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/setup_wizard.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/setup_ui_mac.mm"
        ${IMGUI_SOURCES})
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/setup_ui_mac.mm"
                        "${IMGUI_DIR}/backends/imgui_impl_osx.mm"
                        "${IMGUI_DIR}/backends/imgui_impl_metal.mm"
                 APPEND PROPERTY COMPILE_OPTIONS "-fobjc-arc")
endif()
# The retail PKG key, read from the submodule's tools/pkg_extract.py
# (PS3_GPKG_KEY) into a header in the build tree: this repository carries no
# copy, and the key stays off the compile command lines and build logs.
set(_pkg_py "${PS3RECOMP_DIR}/tools/pkg_extract.py")
file(STRINGS "${_pkg_py}" _pkgkey_line REGEX "^PS3_GPKG_KEY *= *bytes\\.fromhex")
string(REGEX MATCH "\"([0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F])\"" _m "${_pkgkey_line}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "${_pkg_py}: no PS3_GPKG_KEY -- the installer reads the retail PKG key from there")
endif()
string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," _pkgkey_c "${CMAKE_MATCH_1}")
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/gen/ps3recomp_pkg_key.h" CONTENT
"/* Generated from ps3recomp/tools/pkg_extract.py (PS3_GPKG_KEY); not in git. */
static const unsigned char k_ps3recomp_pkg_key[16] = {${_pkgkey_c}};
")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pkg_py}")
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/setup_pkg.cpp" APPEND PROPERTY INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}/gen")
set_property(SOURCE ${SETUP_SOURCES} APPEND PROPERTY INCLUDE_DIRECTORIES "${IMGUI_DIR}")
# third-party code: its warnings are not ours to act on
set_property(SOURCE ${IMGUI_SOURCES} APPEND PROPERTY COMPILE_OPTIONS "-w")

add_executable(${PROJECT_NAME}
    main.cpp
    stubs.cpp
    ${PORT_WIN_SOURCES}
    ${SETUP_SOURCES}
    src/spurs_lfqueue.cpp
    src/spurs_queue.cpp
    src/dod3_edgezlib.cpp
    src/dod3_gc.cpp
    src/dod3_gc_native.cpp
    src/dod3_hot.cpp
    src/dod3_settings_menu.cpp
    src/dod3_menu_patch.cpp
    src/dod3_dlc.cpp
    src/dod3_sysset.cpp
    src/setup_iso.cpp
    ${LIBSRE_LIFTED_CPP}
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${RECOMP_SOURCES}
    ${SPU_SOURCES}
)

# The lifted SPU sources use bare #include "spu_context.h". Scope that include
# directory (and their flags) to those TUs alone: adding it to the whole target
# changes the compile command of every lifted PPU TU, and those are large enough
# that the needless rebuild is measured in tens of minutes.
#   SPU_USE_DISPATCH_MT -- route SPU_IB_DISPATCH through the musttail path.
#   -O2                 -- the target-wide -O1 exists for the huge PPU TUs; the
#                          SPU sources are small and decode-hot, so they get
#                          full optimization. The last -O on the line wins.
if(SPU_SOURCES)
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
        COMPILE_DEFINITIONS SPU_USE_DISPATCH_MT)
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # x86: SSE4.1 for spu_helpers.h's pshufb shuffle (without __SSSE3__ it
        # is a 16-iteration byte loop, 30% of the audio mixer's time on
        # Windows) and FMA so the lifted fmaf() is one instruction rather than
        # a libm call (another 10%). Every x86 this runs on has both.
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;-msse4.1;-mfma")
        else()
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")
        endif()
    endif()
endif()

# /Z7 on everything but the lifted PPU TUs, /DEBUG at link. The lifted code
# is named through its function table instead (src/win_prof.cpp), so the PDB
# stays a few tens of MB and the link fast.
if(WIN32 AND DOD3_SYMBOLS)
    set_source_files_properties(main.cpp stubs.cpp src/spurs_lfqueue.cpp src/spurs_queue.cpp
        src/dod3_edgezlib.cpp ${PORT_WIN_SOURCES} ${PS3RECOMP_SCAFFOLD} ${HLE_NID_TABLE}
        PROPERTIES COMPILE_OPTIONS "/Z7")
    # These already carry options (set above); append rather than replace.
    set_property(SOURCE ${SPU_SOURCES} "${LIBSRE_LIFTED_CPP}" ${SETUP_SOURCES} APPEND PROPERTY COMPILE_OPTIONS "/Z7")
    target_link_options(${PROJECT_NAME} PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

# The player's settings file goes beside the executable once; a copy the
# player has edited is never overwritten.
if(NOT EXISTS "${CMAKE_BINARY_DIR}/dod3.ini")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/dod3.ini" DESTINATION "${CMAKE_BINARY_DIR}")
endif()

# app.rc carries the icon, the version block and the manifest, and needs a
# resource compiler. Listing it as a source on a host that has none is not a
# link error, it is a "no rule to make" for a file nothing can consume.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE DOD3_EBOOT=${DOD3_EBOOT})
message(STATUS "EBOOT:           BLUS31197 ${DOD3_EBOOT}")

target_include_directories(${PROJECT_NAME} PRIVATE
    "${RECOMP_DIR}"                            # the generated ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"        # win32_compat.h and friends
    "${PS3RECOMP_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ps3recomp_runtime
)
# The installer's window on the Mac (src/setup_ui_mac.mm): MetalKit's view,
# pads for ImGui through GameController, file types for the open panels.
if(APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "-framework MetalKit" "-framework GameController" "-framework UniformTypeIdentifiers")
endif()

# The runtime is C++, and on Apple also Objective-C. Letting CMake infer the
# link language from the generated C would leave the C++ standard library out.
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# Windows system import libraries. dbghelp symbolizes crash-handler addresses
# against the PDB, user32 and gdi32 are what the backend's window and its GDI
# present are made of, and winmm is timeBeginPeriod. None of the four names
# anything a POSIX linker can find; the D3D12 backend's own imports come in
# through the #pragma comment(lib) in libs/video/rsx_d3d12_backend.c.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dbghelp
        user32
        gdi32
        winmm
    )
    # A windowed program, so a double-click opens no console. Output still goes
    # wherever a parent redirects it (the scripts' logs), else to the parent
    # terminal's console, else to dod3.log beside the executable (main.cpp,
    # win_stdio_init). -DDOD3_CONSOLE=ON builds a console program again.
    option(DOD3_CONSOLE "Link dod3.exe as a console program" OFF)
    if(NOT DOD3_CONSOLE)
        target_link_options(${PROJECT_NAME} PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    endif()
endif()

# ---------------------------------------------------------------------------
# Compiler-specific flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
    )
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus         # report correct __cplusplus value
        # ppu_recomp.h defines PPU_FENCE, the lowering of the guest's
        # sync/lwsync/eieio/isync, and in C that is <stdatomic.h>. The lifter
        # writes C++ by default, but --single-file writes ppu_recomp.c, and a
        # port that took it would not compile without this. The runtime's own
        # build already carries the same flag for the same reason.
        /experimental:c11atomics
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# The lifted translation units are machine-generated and can run to hundreds of
# thousands of lines each, so they get their own flags:
#
#   - warnings off. They are about generated code and nobody will act on them,
#     and at this volume they bury the diagnostics that matter.
#   - no floating-point contraction. arm64 fuses a multiply and an add by
#     default; a PPC fmadd is spelt as one where the lifter means one.
#   - /bigobj on MSVC. A single lifted file routinely passes the 65,536-section
#     limit of the default object format.
#   - on x86-64 with clang-cl, DOD3_X86_LEVEL (default x86-64-v3: AVX2, FMA,
#     BMI, LZCNT, MOVBE -- any Intel since Haswell, any AMD since Excavator).
#     At the baseline level every lifted fmadd is a call into the C runtime's
#     fma(), sqrt() and trunc() are calls as well, cntlzw is a bsr with a
#     branch around it, and the AltiVec byte shuffles have no pshufb. With the
#     level raised contraction has to be switched off by name, as it is on
#     arm64, and -fno-math-errno lets sqrt be the instruction. main.cpp checks
#     the CPU at start (DOD3_X86_V3). Set DOD3_X86_LEVEL to an empty string
#     for a build that runs on anything.
if(MSVC)
    set(_recomp_opts "/w;/bigobj")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
        set(DOD3_X86_LEVEL "x86-64-v3" CACHE STRING
            "-march for the lifted PPU code on x86-64 (empty: baseline x86-64)")
        if(DOD3_X86_LEVEL)
            list(APPEND _recomp_opts "-march=${DOD3_X86_LEVEL}"
                 "/clang:-ffp-contract=off" "/clang:-fno-math-errno")
            if(DOD3_X86_LEVEL MATCHES "v3|v4|haswell|skylake|znver")
                set_property(SOURCE main.cpp APPEND PROPERTY COMPILE_DEFINITIONS DOD3_X86_V3)
            endif()
        endif()
    endif()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "${_recomp_opts}")
else()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-w;-ffp-contract=off")
endif()
# PPU_INLINE_VM: the lifted code's loads and stores inline the runtime's fast
# path (ps3recomp/runtime/ppu/ppu_vm_fast.h) instead of calling vm_read*/
# vm_write*. The render thread spent ~60% of its time in those calls.
option(DOD3_INLINE_VM "Inline the guest-memory fast path into the lifted code" ON)
if(DOD3_INLINE_VM)
    set_property(SOURCE ${RECOMP_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS PPU_INLINE_VM)
endif()
set_property(SOURCE ${RECOMP_SOURCES} APPEND PROPERTY INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/ppu")

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
