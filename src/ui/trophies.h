/* The launcher's Trophies panel (src/ui/launcher.cpp): the disc's trophy list
 * (ps3recomp trophy_pack.h, read from the install's PS3_GAME/TROPDIR) with
 * what this install has earned (gamedata/trophies, which the game writes),
 * by grade and by chapter, earned ones with their dates; a hidden trophy
 * keeps its secret until it is earned (Show Hidden reveals them).
 *
 * Tests: DOD3_LAUNCHER_OPEN=trophies (launcher.cpp) opens it;
 * DOD3_TROPHIES_SCROLL=<design units> starts its list scrolled. */
#pragma once
#include "ui/look.h"

#include <filesystem>
#include <memory>
#include <string>

namespace dod3setup {

struct TrophiesState;

class TrophiesPanel {
public:
    TrophiesPanel();
    ~TrophiesPanel();   /* after ui_shutdown: the icons' textures are the backend's until then */

    /* read the pack, the unlocks and the account (on a thread) */
    void start(const std::filesystem::path& base);
    /* the menu's line under "Trophies" */
    std::string summary() const;
    /* the panel's page; `focus`: its first item takes the keyboard's focus */
    void draw(look::Kit& k, ImVec2 body, ImVec2 size, bool focus);
    /* typing into a field: the launcher leaves Esc and the pad's back to it */
    bool typing() const;

private:
    std::unique_ptr<TrophiesState> st;
};

}  // namespace dod3setup
