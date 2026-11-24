/*
 * The installer's core, shared by every platform's UI (the setup,
 * src/ui/wizard.cpp, and the launcher, src/ui/launcher.cpp) and the command
 * line (dod3 --install <file>...).
 *
 * What the player gives it, each recognised by itself:
 *   the disc        a decrypted .iso, or the folder holding PS3_GAME
 *                   (src/install/iso.h checks it is BLUS31197 v01.00)
 *   or the game     the US PSN release's package (NPUB31251, 1.00), in a
 *   from PSN        1.01 build: the disc's files but for its executable and
 *                   one config file, both under the buyer's licence and both
 *                   brought by the update (the disc release's, for either)
 *   the update      the 1.01 update package (a 1.01 build needs it)
 *   DLC             any of the 20 PSN packs (UP0082-NPUB31251_00-...), optional
 *   mods            optional fan-made additions: packages for this game
 *                   that are none of the above (the Spanish translation's,
 *                   which it knows by name, or any other: mod_layout in
 *                   install.cpp says where a package's files go) and movie
 *                   archives, and the folder of the DLC chapters' Japanese
 *                   voices (the DLC Undub); see install_mod in install.cpp
 *   keys            what opens the game's executable (src/install/keys.h): the
 *                   setup makes it from the update's EBOOT.BIN (1.01) or the
 *                   disc's (1.00), and checks it by its hash
 * (an EBOOT.ELF already made, with the right hash, is taken too) and where
 * it puts them, under `base`:
 *   game/disc/                                   the disc (or the PSN
 *                                                package's files, laid out
 *                                                as the disc's)
 *   game/disc/game/BLES00000/                    the update, as the console
 *                                                installs it (the title boots
 *                                                as its update)
 *   elf/EBOOT.ELF or elf/EBOOT_101.ELF           the executable
 *   keys.txt                                     the player's keys
 *   game/disc/game/NPUB31251/                    the DLC, as the console
 *                                                installs it
 *   game/mods/                                   a mod's files, at the path
 *                                                under game/disc each stands
 *                                                in for (src/kernel/mods.cpp
 *                                                serves them; the player's
 *                                                own files stay as they
 *                                                are), and installed.txt,
 *                                                the mods installed
 *   game/modfiles/                               each mod's list of its files
 *                                                (<id>.txt), what the player
 *                                                is told of it (<id>.info),
 *                                                and the update's files a mod
 *                                                replaced (<id>/orig/), so a
 *                                                mod can be removed by
 *                                                itself; switched off, its
 *                                                files set aside (<id>/off/)
 *                                                and <id>.off; a mod's own
 *                                                file list for the title
 *                                                (PS3TOC.TXT, <id>/toc/): the
 *                                                one the title reads is made
 *                                                from those of the mods
 *                                                that are on (toc/)
 *   .../PATCH/                                   a mod's files for the
 *                                                update's folder, which the
 *                                                title lists: there, as the
 *                                                console has them (removing
 *                                                the mod puts the update's
 *                                                own back)
 *   .../NPUB31251/USRDIR/DLC_JPV/UNDUB/          the DLC chapters' Japanese
 *                                                voices, beside the voice
 *                                                pack's (src/kernel/dlc.cpp,
 *                                                src/kernel/mods.cpp)
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

enum class SourceKind { Unknown, Disc, Update, Eboot, Dlc, Mod };
enum class ModForm { Pkg, Zip, Undub };   /* what a mod came as */

/* What the player is told of a mod package besides its name. */
struct ModInfo {
    std::string content_id;   /* the package's: "UP0082-BLUS31197_00-..." */
    std::string file;         /* the file it came as */
    bool eboot = false;       /* it carries an EBOOT.BIN, which the port does not use */
    unsigned skipped = 0;     /* its other files the port has no use for, left out */
    bool script = false;      /* it has its own script package (SQEX03GAME.XXX) */
};

struct Source {
    SourceKind kind = SourceKind::Unknown;
    fs::path path;
    std::string id;          /* DLC: "ADDJAPANESEVOICE"; update: its content ID */
    std::string name;        /* for the player: "Japanese Voice Pack" */
    uint64_t bytes = 0;      /* what installing it writes */
    bool psn = false;        /* the game: the US PSN release's package, not the disc */
    ModForm form = ModForm::Pkg;   /* a mod */
    ModInfo about;                 /* a mod package */
    bool set() const { return kind != SourceKind::Unknown; }
};

struct DlcInfo {
    const char* id;
    const char* name;
};
const std::vector<DlcInfo>& known_dlc();            /* the 20 packs: voice, story, music, costumes */
const std::vector<DlcInfo>& known_mods();           /* the mods the setup knows by name */

/* Does this build need the update (1.01)? */
bool update_required();

/* What the player picked. False with `err` (one sentence, what to do) if it
 * is none of the above, or the wrong version of one. Checking a disc or an
 * ELF reads it (a hash); a package is checked again as it is installed. */
bool identify(const fs::path& p, Source* s, std::string* err);

/* The game's key art (PS3_GAME/PIC1.PNG) from the game's source -- the disc
 * image, the folder holding PS3_GAME, or the PSN package -- for the setup to
 * show. False if it has none. */
bool key_art(const Source& game, std::vector<uint8_t>* png);

/* What is installed under `base`. */
struct Status {
    bool disc = false, update = false, eboot = false;
    bool eboot_bin = false;                              /* what the executable is made from */
    std::set<std::string> dlc;                           /* ids */
    std::vector<std::pair<std::string, std::string>> mods;   /* installed: id, name */
    bool ready() const { return disc && eboot && (update || !update_required()); }
};
Status installed(const fs::path& base);

/* The ELF the game starts from, relative to `base`. */
const char* eboot_path();

/* An install: whatever is set is installed (replacing what was there), in
 * the order disc, EBOOT.ELF, update, DLC, mods. */
struct Plan {
    Source disc, update, eboot;
    std::vector<Source> dlc, mods;
    /* the installed mods removed first: each listed one by itself (the
     * update's own files put back), game/mods and the DLC chapters' voices;
     * an unlisted mod's files in the update's folder go only with the update
     * installed again, which the plan then needs (mods_need_update) */
    bool remove_mods = false;
    /* these installed mods removed first, each by itself (mod_removable) */
    std::vector<std::string> remove_ids;
    Keys keys;                                           /* when the executable is to be made */
    uint64_t bytes() const;   /* what it writes, but for an executable it makes (made_eboot_bytes) */
};

/* A recognised file put in the plan: the disc, the update or the executable
 * in place of any before; a DLC pack or a mod in place of one with its id. */
void plan_add(Plan& plan, const Source& s);

/* The size of the executable the setup makes from EBOOT.BIN. */
uint64_t made_eboot_bytes();

/* The DLC Undub is in the plan, and the Japanese Voice Pack it joins neither
 * installed nor in it. */
bool needs_voice_pack(const Plan& plan, const Status& have);

/* Removing the installed mods needs the update package: a mod put files in
 * its folder and its list of them was not kept (a mod installed before the
 * lists were). */
bool mods_need_update(const fs::path& base);

/* Can the mod `id` be removed by itself: its files are listed, or it is the
 * DLC Undub. */
bool mod_removable(const fs::path& base, const std::string& id);

/* Can the mod `id` be switched off and on: its files are listed, and it is
 * not the DLC Undub (the game's Voice option switches that). */
bool mod_switchable(const fs::path& base, const std::string& id);

/* Is the mod `id` switched on (any mod not switched off is). */
bool mod_enabled(const fs::path& base, const std::string& id);

/* A switchable mod switched off or on, the update package not needed: off,
 * its files are set aside (game/modfiles/<id>/off) and the update's own files
 * it replaced put back; on, the other way. The game sees it at its next
 * start. False with `err` if a file could not be moved (doing it again
 * finishes the switch), or, switching on, if a mod that is on replaces one
 * of the same files (mod_conflict's sentence). */
bool set_mod_enabled(const fs::path& base, const std::string& id, bool on, std::string* err);

/* Two mods that replace the same file cannot be on together (the file lists
 * the title reads, PS3TOC.TXT, apart: those are made from every mod's). ""
 * if the mod to add, or the installed mod `id` to switch on, shares no file
 * with an installed mod that is on; else one sentence naming that mod, which
 * is to be switched off first. */
std::string mod_conflict(const fs::path& base, const Source& mod);
std::string mod_conflict(const fs::path& base, const std::string& id);

/* What was kept of an installed mod package (game/modfiles/<id>.info); empty
 * for a mod installed before it was kept, or one that is not a package. */
ModInfo mod_info(const fs::path& base, const std::string& id);

/* What the player is told about a mod package, a sentence each: that its
 * EBOOT.BIN is not used, how many of its files were left out, and that the
 * port's Settings pages are off with a script package of its own. */
std::vector<std::string> mod_notes(const ModInfo& m);

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

/* dod3 --install [--keys <file>] [--remove-mods] <file>...: identify each,
 * install, report on stdout. The keys come from `keys_path`, else
 * <base>/keys.txt. */
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
