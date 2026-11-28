/* The installer's pages, shared by every platform (ui/ui.h has the window;
 * install/install.h does the work). In the spirit of Unleashed Recompiled's
 * installer: the player adds files -- any mix of the disc, the update, DLC
 * packages and mods, on any page -- each is recognised on its own, and a
 * light per item shows what is still needed. The keys for the game's executable
 * are checked as they are entered, by decrypting it.
 *
 *   Welcome -> Game files -> DLC (optional) -> Mods (optional; the 1.01 build)
 *   -> Keys (only while the executable is still to be made and no keys are
 *   built in) -> Install (space check) -> Installing -> Done / Failed
 *
 * It looks as the launcher does (ui/look.h): the title and the steps on the
 * left, the page in a panel on the right, over the game's key art -- the
 * install's, or on a first install the game's own as soon as it is added
 * (read from the disc or the PSN package, it fades in).
 *
 * Tests: DOD3_SETUP_ADD='<file>|<file>' adds files as Add Files would;
 * DOD3_SETUP_AUTO=<n> presses the primary button of the next n pages; with
 * DOD3_SETUP_GRAB=<prefix>, every page is saved as <prefix>_<n>.ppm (the
 * platform's grab: <prefix>.req -> <prefix>.ppm), DOD3_SETUP_GRAB_AT=<n>
 * frames after it opens (40). */
#include "ui/ui.h"
#include "ui/look.h"
#include "ui/setup_shared.h"   /* the files added and the install, on their own threads */
#include "install/install.h"
#include "install/crypto.h"    /* to_hex */
#include "lang.h"              /* tr: the text in the player's language */
#include "util.h"
#include "app_version.h"       /* generated: the port's version label */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <map>
#include <utility>

namespace dod3setup {

namespace {

using dod3::tr;
using dod3::trc;
using dod3::trf;
using dod3::trnf;
using dod3::utf8;
using namespace look;

enum class Page { Welcome, GameFiles, Dlc, Mods, Keys, Confirm, Installing, Done, Failed };

/* the Keys page's check of the keys: not yet, they open the executable, they
 * do not, or something else went wrong (key_msg) */
enum class KeyCheck { Unknown, Works, Wrong, Failed };

/* a button at the foot of a page (nav) */
struct Btn {
    const char* label;
    bool primary = false;   /* the accent; Enter or a pad's confirm button when nothing shows the focus */
    bool enabled = true;
    bool back = false;      /* Esc or a pad's back button */
};

struct Wizard : Kit {
    fs::path base;
    Page page = Page::Welcome;
    bool page_new = true;   /* the first frame of a page: its primary button takes the focus */
    int page_frames = 0;
    Status have;
    std::map<std::string, ModInfo> abouts;   /* what was kept of an installed mod package, read once */
    Plan plan;
    FileChecks checks;
    std::vector<std::string> notes;   /* what the last files added came to */
    InstallJob job;

    /* the Keys page: a field per key; a check (decrypting the executable)
     * runs whenever all of them are well-formed and have changed since */
    struct KeyBox {
        char text[160] = "";
    };
    std::vector<KeyBox> boxes = std::vector<KeyBox>(needed_keys().size());
    int key_gen = 0, checked_gen = -1, running_gen = -1;
    std::future<std::pair<KeyCheck, std::string>> key_check;
    KeyCheck key_result = KeyCheck::Unknown;
    std::string key_msg, key_note;

    /* the key art: the install's, else the added game's once it is read */
    ImTextureData* art = nullptr;
    double art_at = kLongAgo;
    struct Pixels {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        bool ok = false;
    };
    std::future<Pixels> art_job;
    bool art_loading = false;
    fs::path art_of;   /* the game source it was read from */

    /* tests (the header) */
    struct {
        std::string grab;
        int grab_at = 40, grabs = 0;
        int auto_left = 0;   /* the primary buttons still to press */
    } tests;

    void go(Page p)
    {
        page = p;
        page_new = true;
        page_frames = 0;
    }

    /* ---- the files added ---- */

    void add(const std::vector<fs::path>& paths)
    {
        if (paths.empty()) return;
        notes.clear();
        checks.add(paths);
    }
    void add_files(bool dlc)
    {
        add(ui_pick_files(
            dlc ? tr("Add DLC packages") : tr("Add Drakengard 3 files"),
            { { tr("Drakengard 3 files (.iso, .pkg, .zip)"), "*.iso;*.pkg;*.zip" }, { tr("All files"), "*.*" } }));
    }
    void add_folder()
    {
        const fs::path p = ui_pick_folder(tr("The folder that holds PS3_GAME, or a mod's folder"));
        if (!p.empty()) add({ p });
    }

    void poll_checks()
    {
        for (const FileChecks::Finished& f : checks.take_finished()) {
            if (!f.src.set())
                notes.push_back(f.name + ": " + f.err);
            else if (const std::string clash = mod_conflict(base, f.src); !clash.empty())
                notes.push_back(f.name + ": " + clash + ".");
            else
                plan_add(plan, f.src);
        }
    }

    /* the game's art, read once the game is added (when the install has none) */
    void poll_art()
    {
        if (art_loading && art_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            Pixels p = art_job.get();
            art_loading = false;
            if (p.ok && !art) {
                art = art_texture(p.rgba, p.w, p.h);
                art_at = now;
            }
        }
        if (art || art_loading || !plan.disc.set() || plan.disc.path == art_of) return;
        art_of = plan.disc.path;
        art_loading = true;
        const Source game = plan.disc;
        art_job = std::async(std::launch::async, [game] {
            Pixels p;
            std::vector<uint8_t> png;
            p.ok = key_art(game, &png) && art_pixels(png, &p.rgba, &p.w, &p.h);
            return p;
        });
    }

    bool base_ready() const
    { return (plan.disc.set() || have.disc) && (!update_required() || plan.update.set() || have.update); }
    /* the executable is still to be made; the player is asked for its keys
     * only when the setup has none built in */
    bool make_elf_needed() const { return !plan.eboot.set() && !have.eboot; }
    bool need_keys() const { return make_elf_needed() && !builtin_keys().complete(); }
    bool keys_ok() const { return key_result == KeyCheck::Works && checked_gen == key_gen; }
    bool eboot_source() const { return (update_required() ? plan.update.set() : plan.disc.set()) || have.eboot_bin; }
    bool anything() const
    {
        return plan.disc.set() || plan.update.set() || plan.eboot.set() || !plan.dlc.empty() || !plan.mods.empty();
    }
    /* what installing the plan writes, the executable made too */
    uint64_t need_bytes() const { return plan.bytes() + (make_elf_needed() && !plan.eboot.set() ? made_eboot_bytes() : 0); }

    /* ---- the keys ---- */

    Keys keys_from_boxes() const
    {
        Keys k;
        const auto& f = needed_keys();
        for (size_t i = 0; i < f.size(); i++) {
            std::vector<uint8_t> b;
            if (key_problem(f[i], boxes[i].text, &b).empty()) k.v[f[i].id] = b;
        }
        return k;
    }

    void fill_boxes(const Keys& k)
    {
        const auto& f = needed_keys();
        for (size_t i = 0; i < f.size(); i++) {
            auto it = k.v.find(f[i].id);
            if (it == k.v.end()) continue;
            snprintf(boxes[i].text, sizeof boxes[i].text, "%s", to_hex(it->second.data(), it->second.size()).c_str());
        }
        key_gen++;
    }

    /* start a check when the fields changed; collect one that finished */
    void poll_keys()
    {
        if (running_gen >= 0 && key_check.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto [r, m] = key_check.get();
            if (running_gen == key_gen) {
                key_result = r;
                key_msg = m;
                checked_gen = running_gen;
            }
            running_gen = -1;
        }
        if (running_gen >= 0 || checked_gen == key_gen || !need_keys() || !eboot_source()) return;
        const Keys k = keys_from_boxes();
        if (!k.complete()) {
            key_result = KeyCheck::Unknown;
            return;
        }
        running_gen = key_gen;
        const Plan p = plan;
        const fs::path b = base;
        key_check = std::async(std::launch::async, [b, p, k] {
            std::vector<uint8_t> self, elf;
            std::string err;
            bool wrong = false;
            if (!read_eboot_bin(b, p, &self, &err)) return std::make_pair(KeyCheck::Failed, err);
            if (!make_elf(self, k, &elf, &wrong, &err)) return std::make_pair(wrong ? KeyCheck::Wrong : KeyCheck::Failed, err);
            return std::make_pair(KeyCheck::Works, std::string());
        });
    }

    /* ---- pieces ---- */

    /* what is being checked, and what the last files came to */
    void status_lines()
    {
        checking(checks.pending());
        for (const std::string& n : notes) para(F.regular, 14, kErr, n.c_str());
    }

    void adding_line(uint64_t bytes)
    {
        if (!bytes) return;
        para(F.regular, 13, kFaint, trf("Adding {1}", size_text(bytes)).c_str());
        ImGui::Dummy(ImVec2(0, px(4)));
    }

    /* a card's title with a pill at its right */
    void card_title(const char* t, const char* pill_text, ImU32 pill_col)
    {
        label(F.semibold, 18, kText, t);
        const ImVec2 tp = ImGui::GetItemRectMin();
        pill(ImGui::GetWindowDrawList(), ImVec2(card_x1 - px(18), tp.y + px(2)), pill_text, pill_col);
    }

    /* ---- the pages ---- */

    /* a part of the game on the Welcome page: what is needed, and what of it there is */
    struct State {
        const char* text;
        ImU32 col;
    };
    static State state_of(bool added, bool installed, bool required)
    {
        if (added) return { tr("Added"), kOk };
        if (installed) return { tr("Installed"), kOk };
        return required ? State{ tr("Required"), kWarn } : State{ tr("Optional"), kFaint };
    }
    void need_row(const char* name, const char* about, State st)
    {
        const float wr = wrap_right;
        wrap_right = wr - text_size(F.semibold, 12, st.text).x - px(34);   /* the name clear of the pill */
        const ImVec2 p = ImGui::GetCursorScreenPos();
        para(F.semibold, 15, kText, name);
        wrap_right = wr;
        para(F.regular, 13, kDim, about);
        pill(ImGui::GetWindowDrawList(), ImVec2(card_x1 - px(18), p.y), st.text, st.col);
        ImGui::Dummy(ImVec2(0, px(7)));
    }

    void page_welcome()
    {
        card_begin();
        need_row(tr("The game"),
                 update_required() ? tr("The disc, as a decrypted .iso or the folder holding PS3_GAME; or the US PSN "
                                        "package (.pkg).")
                                   : tr("The disc, as a decrypted .iso or the folder holding PS3_GAME."),
                 state_of(plan.disc.set(), have.disc, true));
        if (update_required())
            need_row(tr("Update 1.01"), tr("The update package, UP0082-BLUS31197_00-DOD3PATCH0000000 (.pkg)."),
                     state_of(plan.update.set(), have.update, true));
        need_row(tr("DLC"), tr("The DLC packages (.pkg) you own. No licence files are needed."),
                 state_of(!plan.dlc.empty(), !have.dlc.empty(), false));
        if (update_required())
            need_row(tr("Mods"),
                     tr("Fan-made additions: the Spanish translation, the DLC Undub, any other mod package."),
                     state_of(!plan.mods.empty(), !have.mods.empty(), false));
        if (need_keys())
            need_row(tr("Keys"),
                     update_required() ? tr("The keys that open the update's EBOOT.BIN, the game's executable.")
                                       : tr("The keys that open the disc's EBOOT.BIN, the game's executable."),
                     state_of(keys_ok(), false, true));
        card_end();
        para(F.regular, 13, kDim,
             have.ready() ? tr("Add files on the next pages to replace any of the game's, or to add DLC.")
                          : tr("Files can be added on any page; each one is recognised by itself. Nothing is changed "
                               "in the files you add."));
        ImGui::Dummy(ImVec2(0, px(2)));
        path_line(F.regular, 13, kFaint, have.ready() ? tr("Installed in ") : tr("Installing to "), base);
    }

    void game_card(const char* title, const char* id_line, const char* need, const Source& src, bool installed,
                   const char* kind)
    {
        card_begin();
        card_title(title, src.set() ? tr("Added") : installed ? tr("Installed") : tr("Needed"),
                   src.set() || installed ? kOk : kWarn);
        para(F.medium, 13, kDim, id_line);
        ImGui::Dummy(ImVec2(0, px(4)));
        if (src.set()) {
            path_line(F.regular, 13, alpha(kText, 0.82f), "", src.path);
            para(F.regular, 13, kFaint, (std::string(kind) + " \xC2\xB7 " + size_text(src.bytes)).c_str());
        } else {
            para(F.regular, 14, alpha(kText, 0.82f), installed ? tr("Installed. Add it again to replace it.") : need);
        }
        card_end();
    }

    void page_game_files()
    {
        std::error_code ec;
        const bool folder = fs::is_directory(plan.disc.path, ec);
        game_card(tr("Game"),
                  update_required() ? tr("The disc (BLUS31197) or the US PSN package (NPUB31251)")
                                    : tr("The disc (BLUS31197)"),
                  update_required()
                      ? tr("Add a decrypted .iso or the PSN package (UP0082-NPUB31251_00-DOD3MAINGAME0000) with Add "
                           "Files, or the folder holding PS3_GAME with Add Folder.")
                      : tr("Add a decrypted .iso with Add Files, or the folder holding PS3_GAME with Add Folder."),
                  plan.disc, have.disc,
                  plan.disc.psn ? tr("PSN package") : folder ? tr("Disc folder") : tr("Disc image"));
        if (update_required())
            game_card(
                tr("Update 1.01"), "UP0082-BLUS31197_00-DOD3PATCH0000000",
                tr("Add the update package (.pkg) with Add Files: the disc release's, which the PSN release takes "
                   "too."),
                plan.update, have.update, tr("Package"));
        status_lines();
    }

    void page_dlc()
    {
        struct Group {
            const char* title;
            const char* prefix;
        };
        const Group groups[] = { { TR_NOOP("STORY"), "ADDSCENARIO" },
                                 { TR_NOOP("VOICES"), "ADDJAPANESEVOICE" },
                                 { TR_NOOP("MUSIC"), "ADDSONG" },
                                 { TR_NOOP("OUTFITS AND WEAPONS"), "ADDWEAPONCLOTHE" } };
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float cw = (ImGui::GetContentRegionAvail().x - px(12)) / 2;
        uint64_t adding = 0;
        for (const Source& x : plan.dlc) adding += x.bytes;
        adding_line(adding);
        status_lines();
        for (const Group& g : groups) {
            label(F.semibold, 12, kDim, tr(g.title));
            ImGui::Dummy(ImVec2(0, px(2)));
            int col = 0;
            for (const DlcInfo& d : known_dlc()) {
                if (strncmp(d.id, g.prefix, strlen(g.prefix))) continue;
                bool added = false;
                for (const Source& x : plan.dlc) added |= x.id == d.id;
                const bool in = have.dlc.count(d.id) != 0;
                if (col) ImGui::SameLine(0, px(12));
                const ImVec2 p = ImGui::GetCursorScreenPos(), sz(cw, px(44));
                ImGui::Dummy(sz);
                dl->AddRectFilled(p, p + sz,
                                  added ? alpha(kOk, 0.10f) : in ? IM_COL32(255, 255, 255, 12) : IM_COL32(255, 255, 255, 5),
                                  px(10));
                if (added) dl->AddRect(p, p + sz, alpha(kOk, 0.35f), px(10));
                if (added || in)
                    tick(dl, ImVec2(p.x + px(18), p.y + sz.y / 2), kOk);
                else
                    ring(dl, ImVec2(p.x + px(18), p.y + sz.y / 2), kFaint);
                /* its state at the right, the name in the room before it */
                float room = sz.x - px(36) - px(14);
                if (const char* st = added ? tr("Added") : in ? tr("Installed") : nullptr) {
                    const ImVec2 ss = text_size(F.semibold, 12, st);
                    dl->AddText(F.semibold, px(12), ImVec2(p.x + sz.x - ss.x - px(14), p.y + (sz.y - ss.y) / 2),
                                added ? kOk : kFaint, st);
                    room -= ss.x + px(10);
                }
                const std::string name = fit_line(F.medium, px(14), tr(d.name), room);
                const ImVec2 ts = text_size(F.medium, 14, name.c_str());
                dl->AddText(F.medium, px(14), ImVec2(p.x + px(36), p.y + (sz.y - ts.y) / 2), added || in ? kText : kDim,
                            name.c_str());
                col ^= 1;
            }
            ImGui::Dummy(ImVec2(0, px(10)));
        }
    }

    void mod_card(const ModGroup& g)
    {
        auto is_added = [&](const char* id) {
            for (const Source& m : plan.mods)
                if (m.id == id) return true;
            return false;
        };
        const std::vector<std::string> in = installed_parts(g, have);
        int added = 0;
        bool off = !in.empty();   /* switched off in the launcher */
        for (const ModPart& p : g.parts) added += is_added(p.id);
        for (const std::string& id : in) off = off && !mod_enabled(base, id);
        card_begin();
        if (added)
            card_title(tr(g.title), tr("Added"), kOk);
        else if (off)
            card_title(tr(g.title), tr("Switched off"), kFaint);
        else if (in.size() == g.parts.size())
            card_title(tr(g.title), tr("Installed"), kOk);
        else if (!in.empty())
            card_title(tr(g.title), tr("Partly installed"), kWarn);
        else
            card_title(tr(g.title), tr("Not added"), kFaint);
        para(F.medium, 13, kDim, tr(g.by));
        ImGui::Dummy(ImVec2(0, px(4)));
        para(F.regular, 14, alpha(kText, 0.82f), tr(g.about));
        if (g.parts.size() > 1) {
            ImGui::Dummy(ImVec2(0, px(4)));
            for (const ModPart& p : g.parts) {
                const bool a = is_added(p.id), i = has_mod(have, p.id);
                const char* after = a ? tr("added") : i ? tr("installed") : p.needs ? tr(p.needs) : nullptr;
                list_row(a || i ? kTickMark : kRingMark, a || i ? kOk : kFaint, tr(p.name), a || i ? kText : kDim,
                         after, a ? kOk : kFaint);
            }
        }
        if (&g == &kUndub && needs_voice_pack(plan, have)) {
            ImGui::Dummy(ImVec2(0, px(4)));
            para(F.medium, 13, kWarn, tr("Needs the Japanese Voice Pack: add its package on the DLC page too."));
        }
        card_end();
    }

    void page_mods()
    {
        uint64_t adding = 0;
        for (const Source& x : plan.mods) adding += x.bytes;
        adding_line(adding);
        status_lines();
        mod_card(kSpanish);
        mod_card(kUndub);
        /* others: added, or installed before */
        auto about_lines = [&](const ModInfo& about) {
            const std::string from = mod_source_line(about);
            if (!from.empty()) para(F.medium, 13, kDim, from.c_str());
            for (const std::string& n : mod_notes(about)) {
                ImGui::Dummy(ImVec2(0, px(2)));
                para(F.regular, 13, alpha(kText, 0.82f), n.c_str());
            }
        };
        for (const Source& m : plan.mods)
            if (!known_mod(m.id)) {
                card_begin();
                card_title(m.name.c_str(), tr("Added"), kOk);
                about_lines(m.about);
                card_end();
            }
        for (const auto& m : have.mods) {
            bool listed = known_mod(m.first);
            for (const Source& x : plan.mods) listed |= x.id == m.first;
            if (listed) continue;
            const bool on = mod_enabled(base, m.first);
            card_begin();
            card_title(m.second.c_str(), on ? tr("Installed") : tr("Switched off"), on ? kOk : kFaint);
            if (!abouts.count(m.first)) abouts[m.first] = mod_info(base, m.first);
            about_lines(abouts[m.first]);
            card_end();
        }
        if (plan.mods.empty() && have.mods.empty())
            para(F.regular, 13, kFaint, tr("Any other mod package for the game (.pkg) can be added too."));
        if (!have.mods.empty())
            para(F.regular, 13, kFaint, tr("Installed mods are taken out with Remove, in the launcher's Mods page."));
    }

    void page_keys()
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        para(
            F.regular, 14, alpha(kText, 0.82f),
            update_required()
                ? tr("The game's executable is made from the update's EBOOT.BIN, which is encrypted. Enter the keys "
                     "that open it, as hex. They are checked by decrypting it, and kept in this install for next time.")
                : tr("The game's executable is made from the disc's EBOOT.BIN, which is encrypted. Enter the keys "
                     "that open it, as hex. They are checked by decrypting it, and kept in this install for next "
                     "time."));
        ImGui::Dummy(ImVec2(0, px(8)));
        const auto& f = needed_keys();
        for (size_t i = 0; i < f.size(); i++) {
            std::vector<uint8_t> b;
            const bool empty = boxes[i].text[0] == 0;
            const std::string problem = empty ? "" : key_problem(f[i], boxes[i].text, &b);
            const std::string bytes = trnf(f[i].bytes, "{1} byte", "{1} bytes", f[i].bytes);
            const ImVec2 c = ImGui::GetCursorScreenPos();
            list_row(kNoMark, 0, tr(f[i].label), kText, bytes.c_str(), kFaint, F.semibold, 15);
            if (!empty) {   /* at the line's end: well-formed, or not */
                const float right = c.x + ImGui::GetContentRegionAvail().x - px(8);
                const float mid = c.y + ImGui::GetItemRectSize().y / 2;
                if (problem.empty())
                    tick(dl, ImVec2(right, mid), kOk);
                else
                    dot(dl, ImVec2(right, mid), kWarn);
            }
            char id[16];
            snprintf(id, sizeof id, "##key%zu", i);
            ImGui::PushFont(F.medium, 15);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText(id, boxes[i].text, sizeof boxes[i].text)) key_gen++;
            ImGui::PopFont();
            if (!problem.empty()) para(F.regular, 13, kErr, problem.c_str());
            ImGui::Dummy(ImVec2(0, px(8)));
        }
        if (button(tr("Load Key File..."), 0)) {
            const auto picked = ui_pick_files(tr("A key file (name=hex lines)"),
                                              { { tr("Key files (.txt)"), "*.txt" }, { tr("All files"), "*.*" } });
            if (!picked.empty()) {
                Keys k;
                std::string err;
                if (keys_load(picked[0], &k, &err)) {
                    fill_boxes(k);
                    key_note = trf("{1} of {2} keys found in {3}", k.v.size(), f.size(), utf8(picked[0].filename()));
                } else {
                    key_note = utf8(picked[0].filename()) + ": " + err;
                }
            }
        }
        if (!key_note.empty()) {   /* beside the button */
            const ImVec2 bmax = ImGui::GetItemRectMax(), bmin = ImGui::GetItemRectMin();
            const ImVec2 ts = text_size(F.regular, 13, key_note.c_str());
            dl->AddText(F.regular, px(13), ImVec2(bmax.x + px(14), bmin.y + (bmax.y - bmin.y - ts.y) / 2), kDim,
                        key_note.c_str());
        }
        ImGui::Dummy(ImVec2(0, px(8)));
        if (!eboot_source()) {
            para(F.regular, 14, kDim,
                 update_required()
                     ? tr("Add the update package on the Game Files page: the executable is made from it.")
                     : tr("Add the disc on the Game Files page: the executable is made from it."));
        } else if (running_gen >= 0) {
            para(F.regular, 14, kDim, tr("Checking the keys..."));
        } else if (keys_ok()) {
            list_row(kTickMark, kOk, tr("These keys open the game's executable."), kText, nullptr, kFaint, F.medium,
                     14);
        } else if (checked_gen == key_gen && key_result == KeyCheck::Wrong) {
            para(F.medium, 14, kErr,
                 tr("These keys do not open EBOOT.BIN. Check each one against your key set (key revision 0x1C)."));
        } else if (checked_gen == key_gen && key_result == KeyCheck::Failed) {
            para(F.medium, 14, kErr, key_msg.c_str());
        }
    }

    void page_confirm()
    {
        if (!anything() && !make_elf_needed()) {
            para(F.regular, 14, kDim,
                 have.ready() ? tr("Everything added is installed already.") : tr("Add the game's files first."));
            return;
        }
        auto row = [&](const std::string& name, uint64_t bytes) {
            list_row(kDotMark, kOk, name.c_str(), kText, bytes ? size_text(bytes).c_str() : nullptr, kFaint, F.medium, 15);
            ImGui::Dummy(ImVec2(0, px(2)));
        };
        card_begin();
        if (plan.disc.set()) row(plan.disc.psn ? tr("Game (PSN package)") : tr("Game disc"), plan.disc.bytes);
        if (plan.update.set()) row(tr("Update 1.01"), plan.update.bytes);
        if (plan.eboot.set())
            row("EBOOT.ELF", plan.eboot.bytes);
        else if (make_elf_needed())
            row(need_keys()         ? tr("The game's executable, made with your keys")
                : update_required() ? tr("The game's executable, made from the update")
                                    : tr("The game's executable, made from the disc"),
                made_eboot_bytes());
        for (const Source& d : plan.dlc) row(d.name, d.bytes);
        for (const Source& m : plan.mods) row(tr(m.name.c_str()), m.bytes);
        card_end();
        const uint64_t need = need_bytes(), space = free_bytes(base);
        para(F.regular, 14, kDim, trf("Needs {1}; {2} free on the drive.", size_text(need), size_text(space)).c_str());
        path_line(F.regular, 13, kFaint, tr("To "), base);
        if (space && need > space) {
            ImGui::Dummy(ImVec2(0, px(4)));
            para(F.medium, 14, kErr,
                 trf("Not enough space. Free {1}, or install fewer DLC packs.", size_text(need - space)).c_str());
        }
    }

    void page_installing()
    {
        ImGui::Dummy(ImVec2(0, px(10)));
        const uint64_t d = job.done, t = job.total;
        std::string rate;
        if (job.seconds() > 3 && d > 0 && d < t) {
            const double per_s = d / job.seconds(), left = (t - d) / per_s;
            char speed[32], sec[8];
            snprintf(speed, sizeof speed, "%.0f", per_s / 1048576.0);
            snprintf(sec, sizeof sec, "%02d", (int)left % 60);
            rate = trf("  \xC2\xB7  {1} MB/s  \xC2\xB7  about {2} min {3} s left", speed, (int)left / 60, sec);
        }
        install_progress(job.what(), d, t, rate);
        if (job.cancel) para(F.regular, 13, kWarn, tr("Cancelling..."));
    }

    void page_done()
    {
        card_begin();
        auto row = [&](bool in, const char* name, const char* after) {
            list_row(in ? kTickMark : kRingMark, in ? kOk : kFaint, name, kText, after, kFaint, F.medium, 15);
            ImGui::Dummy(ImVec2(0, px(2)));
        };
        row(true, tr("The game"), nullptr);
        if (update_required()) row(true, tr("Update 1.01"), nullptr);
        const std::string dlc = trf("{1} of {2}", have.dlc.size(), known_dlc().size());
        row(!have.dlc.empty(), tr("DLC"), dlc.c_str());
        if (update_required()) {
            std::vector<std::string> names;
            for (const auto& m : have.mods) names.push_back(tr(m.second.c_str()));
            row(!names.empty(), tr("Mods"), names.empty() ? tr("none") : nullptr);
            if (!names.empty()) {
                std::string list;
                for (const std::string& n : names) list += (list.empty() ? "" : ", ") + n;
                ImGui::Indent(px(22));
                para(F.regular, 13, kDim, list.c_str());
                ImGui::Unindent(px(22));
            }
        }
        card_end();
        para(F.regular, 14, alpha(kText, 0.82f),
             tr("From now on the game opens on its launcher: Play, and the mods, the DLC and the settings."));
    }

    void page_failed()
    {
        if (!job.cancel) {
            para(F.medium, 14, kErr, job.error.c_str());
            ImGui::Dummy(ImVec2(0, px(6)));
        }
        para(F.regular, 14, kDim, tr("The part that was being installed has been taken out again; the parts before it "
                                     "stay."));
    }

    void start_install()
    {
        plan.keys = builtin_keys().complete() ? builtin_keys() : keys_from_boxes();
        job.start(base, plan);
        go(Page::Installing);
    }

    /* ---- the steps, on the left ---- */

    bool locked() const { return page == Page::Installing || page == Page::Done || page == Page::Failed; }

    std::string step_note(Page p) const
    {
        char t[64];
        switch (p) {
        case Page::GameFiles:
            if (!plan.disc.set() && !have.disc) return tr("The game is needed");
            if (update_required() && !plan.update.set() && !have.update) return tr("The update is needed");
            return plan.disc.set() || plan.update.set() ? tr("Added") : tr("Installed");
        case Page::Dlc:
            if (!plan.dlc.empty()) return trnf(plan.dlc.size(), "{1} added", "{1} added", plan.dlc.size());
            if (!have.dlc.empty()) return trf("{1} of {2} installed", have.dlc.size(), known_dlc().size());
            return tr("Optional");
        case Page::Mods:
            if (!plan.mods.empty()) return trnf(plan.mods.size(), "{1} added", "{1} added", plan.mods.size());
            if (!have.mods.empty()) return trnf(have.mods.size(), "{1} installed", "{1} installed", have.mods.size());
            return tr("Optional");
        case Page::Keys: return keys_ok() ? tr("Checked") : tr("Needed for the executable");
        case Page::Confirm:
            if (page == Page::Installing) {
                snprintf(t, sizeof t, "%d%%", (int)(100.0 * (double)job.done / (double)job.total));
                return t;
            }
            return page == Page::Done     ? tr("Done")
                   : page == Page::Failed ? (job.cancel ? tr("Cancelled") : tr("Failed"))
                                          : "";
        default: return "";
        }
    }

    void steps()
    {
        struct Step {
            const char* label;
            Page page;
        };
        std::vector<Step> list = { { tr("Welcome"), Page::Welcome },
                                   { tr("Game Files"), Page::GameFiles },
                                   { tr("DLC"), Page::Dlc } };
        if (update_required()) list.push_back({ tr("Mods"), Page::Mods });   /* mods are for the 1.01 build */
        if (need_keys() || page == Page::Keys) list.push_back({ tr("Keys"), Page::Keys });
        list.push_back({ trc("heading", "Install"), Page::Confirm });
        const Page shown = locked() ? Page::Confirm : page;
        int at = 0;
        for (size_t i = 0; i < list.size(); i++)
            if (list[i].page == shown) at = (int)i;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float x = px(72), step = px(58), r = px(12);
        float y = px(236);
        for (int i = 0; i < (int)list.size(); i++) {
            const bool cur = i == at, past = i < at || page == Page::Done;
            const bool can = i < at && !locked();   /* back to an earlier step */
            ImGui::SetCursorScreenPos(ImVec2(x - px(10), y - px(8)));
            ImGui::PushID(i);
            ImGui::BeginDisabled(!can);
            const bool pressed = hit("##step", ImVec2(px(300), px(50))) && cooldown == 0;
            const bool hot = can && hot_item();
            ImGui::EndDisabled();
            ImGui::PopID();
            float& a = anim_of(ImGui::GetItemID());
            approach(a, hot ? 1.0f : 0.0f, 16.0f);
            if (pressed && can) go(list[i].page);

            const ImVec2 c(x + r, y + r);
            if (i + 1 < (int)list.size())   /* the line down to the next step */
                dl->AddLine(ImVec2(c.x, c.y + r + px(5)), ImVec2(c.x, c.y + step - r - px(5)),
                            past && i + 1 <= at ? alpha(kOk, 0.35f) : kLine, px(1.5f));
            char num[8];
            snprintf(num, sizeof num, "%d", i + 1);
            const ImVec2 ns = text_size(F.semibold, 12, num);
            if (cur && page != Page::Done) {
                dl->AddCircleFilled(c, r, page == Page::Failed ? kErr : kAccent);
                dl->AddText(F.semibold, px(12), ImVec2(c.x - ns.x / 2, c.y - ns.y / 2), kText, num);
            } else if (past) {
                dl->AddCircleFilled(c, r, alpha(kOk, 0.14f + 0.10f * a));
                tick(dl, c, kOk);
            } else {
                dl->AddCircle(c, r, alpha(kText, 0.22f), 0, px(1.4f));
                dl->AddText(F.semibold, px(12), ImVec2(c.x - ns.x / 2, c.y - ns.y / 2), kFaint, num);
            }
            const ImU32 col = cur ? kText : past ? mix(alpha(kText, 0.72f), kText, a) : alpha(kText, 0.42f);
            const float lx = x + r * 2 + px(16) + px(6) * a;
            dl->AddText(F.medium, px(19), ImVec2(lx, y + px(1)), col, list[i].label);
            const std::string note = step_note(list[i].page);
            if (!note.empty())
                dl->AddText(F.regular, px(13), ImVec2(lx, y + px(25)), cur ? kDim : alpha(kDim, 0.7f), note.c_str());
            y += step;
        }
        const std::string v = trf("Version {1}  \xC2\xB7  Setup", DOD3_VERSION_STRING);
        dl->AddText(F.regular, px(13), ImVec2(px(72), H - px(52)), alpha(kDim, 0.8f), v.c_str());
    }

    /* ---- the foot: what a page adds on the left, the way on at the right ---- */

    /* the buttons at the foot's right, in order; the one pressed, or -1 */
    int nav(float y, float right, const std::vector<Btn>& bs)
    {
        const float kPrimaryW = 136, kOtherW = 104;   /* the narrowest they are drawn */
        const ImGuiIO& io = ImGui::GetIO();
        const float gap = px(10);
        std::vector<float> w;
        float span = -gap;
        for (const Btn& b : bs) {
            w.push_back(std::max(b.primary ? kPrimaryW : kOtherW, button_width(b.label) / s));
            span += px(w.back()) + gap;
        }
        float x = right - span;
        int pressed = -1, primary = -1, back = -1;
        for (size_t i = 0; i < bs.size(); i++) {
            const Btn& b = bs[i];
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            if (b.primary) {
                primary = (int)i;
                if (page_new && io.NavVisible) ImGui::SetKeyboardFocusHere();
            }
            if (b.back) back = (int)i;
            if (button(b.label, w[i], b.primary ? ButtonKind::Main : ButtonKind::Quiet, b.enabled)) pressed = (int)i;
            x += px(w[i]) + gap;
        }
        if (pressed < 0 && primary >= 0 && bs[primary].enabled) {
            if (tests.auto_left > 0 && page_frames >= std::max(45, tests.grab_at + 5)) {   /* after the page's grab */
                tests.auto_left--;
                pressed = primary;
            } else if (!io.NavVisible && cooldown == 0 && !checks.pending() && confirm_pressed())
                pressed = primary;
        }
        if (pressed < 0 && back >= 0 && bs[back].enabled && cooldown == 0 && !io.WantTextInput && back_pressed())
            pressed = back;
        return pressed;
    }

    /* kGoOn, or run_installer's result */
    int foot(ImVec2 body, ImVec2 size)
    {
        const float y = body.y + size.y - px(44), right = body.x + size.x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddLine(ImVec2(body.x, y - px(16)), ImVec2(right, y - px(16)), kLine);
        ImGui::SetCursorScreenPos(ImVec2(body.x, y));
        const bool busy = checks.pending() > 0;
        /* adding, on the pages that take files */
        if (page == Page::GameFiles || page == Page::Mods) {
            if (button(tr("Add Files..."), 0,
                       page == Page::GameFiles && !base_ready() ? ButtonKind::Main : ButtonKind::Quiet))
                add_files(false);
            ImGui::SameLine(0, px(10));
            if (button(tr("Add Folder..."), 0)) add_folder();
        } else if (page == Page::Dlc) {
            if (button(tr("Add DLC Packages..."), 0)) add_files(true);
        } else if (page == Page::Done) {
            if (button(tr("Add More"), 0)) go(Page::Dlc);
        }
        /* from the launcher (an install there already), closing goes back to it */
        const char* close = have.ready() ? tr("Close") : tr("Quit");
        int result = kGoOn;
        switch (page) {
        case Page::Welcome:
            if (have.ready()) {
                switch (nav(
                    y, right,
                    { { .label = tr("Close") }, { .label = tr("Play") }, { .label = tr("Next"), .primary = true } })) {
                case 0: result = kQuit; break;
                case 1: result = kPlay; break;
                case 2: go(Page::GameFiles); break;
                }
            } else {
                switch (nav(y, right, { { .label = tr("Quit") }, { .label = tr("Next"), .primary = true } })) {
                case 0: result = kQuit; break;
                case 1: go(Page::GameFiles); break;
                }
            }
            break;
        case Page::GameFiles:
            switch (nav(y, right, { { .label = tr("Back"), .back = true },
                                    { .label = tr("Next"), .primary = true, .enabled = base_ready() && !busy } })) {
            case 0: go(Page::Welcome); break;
            case 1: go(Page::Dlc); break;
            }
            break;
        case Page::Dlc:
            switch (
                nav(y, right,
                    { { .label = tr("Back"), .back = true },
                      { .label = plan.dlc.empty() ? tr("Skip") : tr("Next"), .primary = true, .enabled = !busy } })) {
            case 0: go(Page::GameFiles); break;
            case 1: go(update_required() ? Page::Mods : need_keys() ? Page::Keys : Page::Confirm); break;
            }
            break;
        case Page::Mods:
            switch (nav(y, right, { { .label = tr("Back"), .back = true },
                                    { .label = plan.mods.empty() ? tr("Skip") : tr("Next"),
                                      .primary = true,
                                      .enabled = !busy && !needs_voice_pack(plan, have) } })) {
            case 0: go(Page::Dlc); break;
            case 1: go(need_keys() ? Page::Keys : Page::Confirm); break;
            }
            break;
        case Page::Keys:
            switch (nav(y, right,
                        { { .label = tr("Back"), .back = true },
                          { .label = tr("Next"), .primary = true, .enabled = keys_ok() } })) {
            case 0: go(update_required() ? Page::Mods : Page::Dlc); break;
            case 1: go(Page::Confirm); break;
            }
            break;
        case Page::Confirm: {
            const Page prev = need_keys() ? Page::Keys : update_required() ? Page::Mods : Page::Dlc;
            if (!anything() && !make_elf_needed()) {
                switch (nav(y, right, { { .label = tr("Back"), .back = true },
                                        { .label = have.ready() ? tr("Play") : tr("Game Files"), .primary = true } })) {
                case 0: go(prev); break;
                case 1:
                    if (have.ready())
                        result = kPlay;
                    else
                        go(Page::GameFiles);
                    break;
                }
            } else {
                const uint64_t need = need_bytes(), space = free_bytes(base);
                switch (nav(
                    y, right,
                    { { .label = tr("Back"), .back = true },
                      { .label = tr("Install"), .primary = true, .enabled = !busy && !(space && need > space) } })) {
                case 0: go(prev); break;
                case 1: start_install(); break;
                }
            }
            break;
        }
        case Page::Installing:
            if (nav(y, right,
                    { { .label = job.cancel ? tr("Cancelling...") : tr("Cancel"), .enabled = !job.cancel } }) == 0)
                job.cancel = true;
            break;
        case Page::Done:
            switch (nav(y, right, { { .label = close }, { .label = tr("Play"), .primary = true } })) {
            case 0: result = kQuit; break;
            case 1: result = kPlay; break;
            }
            break;
        case Page::Failed:
            switch (nav(y, right, { { .label = close }, { .label = tr("Back"), .primary = true } })) {
            case 0: result = kQuit; break;
            case 1: go(Page::Confirm); break;
            }
            break;
        }
        return result;
    }

    /* ---- the frame ---- */

    std::pair<const char*, const char*> heading() const   /* the panel's heading, and the line under it */
    {
        switch (page) {
        case Page::Welcome:
            return have.ready()
                       ? std::make_pair(
                             tr("Game Files"),
                             tr("Drakengard 3 is installed. Add DLC here, or replace any of the game's files."))
                       : std::make_pair(tr("Welcome"),
                                        tr("Drakengard 3 Recompiled installs the game from your own copy of it."));
        case Page::GameFiles:
            return { tr("Game Files"), update_required() ? tr("The game and its 1.01 update.") : tr("The game disc.") };
        case Page::Dlc:
            return { tr("DLC"), tr("Optional: the packs you own. More can be added later, from the launcher.") };
        case Page::Mods:
            return { tr("Mods"), tr("Optional: fan-made additions, installed beside your own game files.") };
        case Page::Keys: return { tr("Keys"), tr("The keys that open the game's executable, EBOOT.BIN.") };
        case Page::Confirm:
            return { trc("heading", "Install"),
                     anything() || make_elf_needed() ? tr("Ready to install.") : tr("Nothing new to install.") };
        case Page::Installing:
            return { tr("Installing"), tr("The game's files are copied beside the game; this takes a few minutes.") };
        case Page::Done: return { tr("Installed"), tr("Drakengard 3 is ready to play.") };
        case Page::Failed:
            return job.cancel ? std::make_pair(tr("Cancelled"), tr("Nothing more was installed."))
                              : std::make_pair(tr("Installation Failed"), tr("Something went wrong while installing."));
        }
        return { "", nullptr };
    }

    int frame()
    {
        const Page before = page;
        begin_frame();
        page_frames++;
        grab_step(tests.grab, page_frames, tests.grab_at - 20, tests.grab_at, tests.grabs);
        if (!tests.grab.empty() && page_frames == tests.grab_at) tests.grabs++;
        poll_checks();
        poll_keys();
        poll_art();
        if (job.collect()) {
            have = installed(base);
            abouts.clear();
            if (job.ok) {
                plan = Plan();
                go(Page::Done);
            } else
                go(Page::Failed);
        }
        backdrop(art, art ? ease_out((float)(now - art_at) / 1.2f) : 0.0f, 1.0f);
        begin_window("##setup");
        title();
        steps();

        const auto [h, sub] = heading();
        ImVec2 body, size;
        panel_frame(h, sub, ease_out((float)now / 0.3f), false, &body, &size);
        ImGui::SetCursorScreenPos(body);
        if (page_new) ImGui::SetNextWindowScroll(ImVec2(0, 0));   /* a page opens at its top */
        ImGui::BeginChild("##page", ImVec2(size.x, size.y - px(64)), ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoBackground);
        switch (page) {
        case Page::Welcome: page_welcome(); break;
        case Page::GameFiles: page_game_files(); break;
        case Page::Dlc: page_dlc(); break;
        case Page::Mods: page_mods(); break;
        case Page::Keys: page_keys(); break;
        case Page::Confirm: page_confirm(); break;
        case Page::Installing: page_installing(); break;
        case Page::Done: page_done(); break;
        case Page::Failed: page_failed(); break;
        }
        ImGui::Dummy(ImVec2(0, px(4)));
        ImGui::EndChild();
        const int result = foot(body, size);
        if (page == before) page_new = false;   /* a page just opened focuses its primary button next frame */
        ImGui::End();
        toast_draw();
        return result;
    }
};

}  // namespace

int run_installer(const fs::path& base)
{
    if (!ui_init("Drakengard 3 Recompiled Setup", 1280, 720, "", 17.0f)) return -1;
    look::style();
    Wizard w;
    look::load_fonts(w.F);
    w.base = base;
    w.art = look::load_art(base);
    if (const char* a = getenv("DOD3_SETUP_AUTO")) w.tests.auto_left = atoi(a);
    if (const char* g = getenv("DOD3_SETUP_GRAB")) w.tests.grab = g;
    if (const char* g = getenv("DOD3_SETUP_GRAB_AT")) w.tests.grab_at = std::max(21, atoi(g));   /* the request goes 20 frames before */
    std::vector<fs::path> paths;
    for (const std::string& f : env_list("DOD3_SETUP_ADD", '|')) paths.push_back(fs::u8path(f));
    w.add(paths);
    w.have = installed(base);
    {   /* the keys this install was made with, if any */
        Keys k;
        std::string err;
        std::error_code ec;
        if (fs::exists(keys_file(base), ec) && keys_load(keys_file(base), &k, &err)) w.fill_boxes(k);
    }
    int result = kQuit;
    while (ui_frame_begin()) {
        const int r = w.frame();
        ui_frame_end();
        if (r != kGoOn) {
            result = r;
            break;
        }
    }
    w.job.stop();   /* the window was closed mid-install: the install stops first */
    w.checks.wait();
    if (w.running_gen >= 0) w.key_check.wait();
    if (w.art_loading) w.art_job.wait();
    ui_shutdown();   /* the backend lets go of the picture's texture */
    delete w.art;
    return result == kPlay && installed(base).ready() ? kPlay : kQuit;
}

}  // namespace dod3setup
