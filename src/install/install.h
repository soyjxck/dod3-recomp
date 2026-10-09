/*
 * The installer's core, shared by every platform's UI (src/ui/wizard.cpp)
 * and the command line (dod3 --install <file>...).
 *
 * What the player gives it, each recognised by itself:
 *   the disc        a decrypted .iso, or the folder holding PS3_GAME
 *                   (src/install/iso.h checks it is BLUS31197 v01.00)
 *   the update      the 1.01 update package (a 1.01 build needs it)
 *   DLC             any of the 20 PSN packs (UP0082-NPUB31251_00-...), optional
 *   keys            what opens the game's executable (src/install/keys.h): the
 *                   setup makes it from the update's EBOOT.BIN (1.01) or the
 *                   disc's (1.00), and checks it by its hash
 * (an EBOOT.ELF already made, with the right hash, is taken too) and where
 * it puts them, under `base`:
 *   game/disc/                                   the disc
 *   game/disc/game/BLES00000/                    the update, as the console
 *                                                installs it (the title boots
 *                                                as its update)
 *   elf/EBOOT.ELF or elf/EBOOT_101.ELF           the executable
 *   keys.txt                                     the player's keys
 *   game/disc/game/NPUB31251/                    the DLC, as the console
 *                                                installs it
 * Each part is unpacked into game/setup.partial/ and moved into place once it
 * is whole, so a failure, a cancel or a crash leaves no half-installed part
 * (the parts before it stay installed). A run that adds DLC to an install
 * leaves the rest alone; reinstalling the disc keeps the update and the DLC.
 */
#pragma once
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>
#include <filesystem>

#include "install/keys.h"

namespace dod3setup {

namespace fs = std::filesystem;

enum class SourceKind { Unknown, Disc, Update, Eboot, Dlc };

struct Source {
    SourceKind kind = SourceKind::Unknown;
    fs::path path;
    std::string id;          /* DLC: "ADDJAPANESEVOICE"; update: its content ID */
    std::string name;        /* for the player: "Japanese Voice Pack" */
    uint64_t bytes = 0;      /* what installing it writes */
    bool set() const { return kind != SourceKind::Unknown; }
};

struct DlcInfo {
    const char* id;
    const char* name;
};
const std::vector<DlcInfo>& known_dlc();            /* the 20 packs: voice, story, music, costumes */

/* Does this build need the update (1.01)? */
bool update_required();

/* What the player picked. False with `err` (one sentence, what to do) if it
 * is none of the above, or the wrong version of one. Checking a disc or an
 * ELF reads it (a hash); a package is checked again as it is installed. */
bool identify(const fs::path& p, Source* s, std::string* err);

/* What is installed under `base`. */
struct Status {
    bool disc = false, update = false, eboot = false;
    bool eboot_bin = false;                              /* what the executable is made from */
    std::set<std::string> dlc;                           /* ids */
    bool ready() const { return disc && eboot && (update || !update_required()); }
};
Status installed(const fs::path& base);

/* The ELF the game starts from, relative to `base`. */
const char* eboot_path();

/* An install: whatever is set is installed (replacing what was there), in
 * the order disc, EBOOT.ELF, update, DLC. */
struct Plan {
    Source disc, update, eboot;
    std::vector<Source> dlc;
    Keys keys;                                           /* when the executable is to be made */
    uint64_t bytes() const;
};

/* The encrypted executable: from the plan's update (1.01) or disc (1.00) if
 * set, else from the install. */
bool read_eboot_bin(const fs::path& base, const Plan& plan, std::vector<uint8_t>* self, std::string* err);

/* EBOOT.BIN -> the executable, checked by its hash. */
bool make_elf(const std::vector<uint8_t>& self, const Keys& keys, std::vector<uint8_t>* elf, bool* wrong_keys,
              std::string* err);
uint64_t free_bytes(const fs::path& base);

/* progress(done, total, what) returns false to cancel. */
bool install(const fs::path& base, const Plan& plan,
             const std::function<bool(uint64_t, uint64_t, const std::string&)>& progress, std::string* err);

/* dod3 --install [--keys <file>] <file>...: identify each, install, report
 * on stdout. The keys come from `keys_path`, else <base>/keys.txt. */
int install_files_cli(const fs::path& base, const std::vector<fs::path>& files, const fs::path& keys_path);

/* The title's game-data install, laid out under the game tree's root
 * (game/disc): on first boot the title copies COOKEDSOUND and COOKEDPS3 (5.1
 * GB) to /dev_hdd0/game/BLES00000DATA one step per frame, about 8 minutes,
 * and a copy cut short breaks it for good. The two folders are links onto the
 * disc's instead (junctions on Windows, relative symlinks elsewhere); links
 * that point nowhere (a moved folder) are made again, and a copy cut short is
 * replaced. Run by the installer and at every boot (main.cpp). Where no link
 * can be made, no tree is left and the title copies the data itself. */
bool gamedata_layout(const fs::path& game_root);

/* src/kernel/dlc.cpp: the Japanese voice pack's file lists, under the game
 * tree's root (game/disc); false if it is not installed. */
bool dlc_write_jpv_lists(const fs::path& game_root, bool force);

}  // namespace dod3setup
