/*
 * First-run setup, Windows: the installer (src/setup_wizard.cpp, in a
 * Direct3D 12 window from src/setup_ui_win.cpp) when something the game
 * needs is missing, or when asked for (--setup). dod3_setup_win() returns 0
 * when everything is in place.
 */
#include "setup_install.h"
#include "setup_ui.h"

#include <windows.h>

extern "C" int dod3_setup_win(const wchar_t* base_dir, int force)
{
    using namespace dod3setup;
    const fs::path base(base_dir);
    if (installed(base).ready() && !force) return 0;
    const int r = run_installer(base);
    if (r >= 0) return r;
    /* no window (no Direct3D 12): the command line does the same */
    MessageBoxW(NULL,
                L"Setup could not open its window (it needs Direct3D 12).\n\n"
                L"The game can be installed from a command prompt instead, in this folder:\n\n"
                L"dod3.exe --install <disc .iso or folder> <update .pkg> [DLC .pkg ...]\n\n"
                L"Details are in dod3.log.",
                L"Drakengard 3 Recompiled", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    return 1;
}
