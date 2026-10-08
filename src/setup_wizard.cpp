/* The installer's pages, shared by every platform (setup_ui.h has the window;
 * setup_install.h does the work). In the spirit of Unleashed Recompiled's
 * installer: the player adds files -- any mix of the disc, the update and
 * DLC packages, on any page -- each is recognised on its own, and a light
 * per item shows what is still needed. The keys for the game's executable
 * are checked as they are entered, by decrypting it.
 *
 *   Welcome -> Game files -> DLC (optional) -> Keys (while the executable is
 *   still to be made) -> Install (space check) -> Installing -> Done / Failed
 *
 * Every size is in units of the font size and every position is relative to
 * the window, so the pages are the same on a 1x, 1.5x or Retina display. */
#include "setup_ui.h"
#include "setup_install.h"
#include "setup_crypto.h"
#include "dod3_eboot.h"
#include "dod3_util.h"
#include "app_version.h"   /* generated: the port's version label */

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>

namespace dod3setup {

namespace {

enum class Page { Welcome, GameFiles, Dlc, Keys, Confirm, Installing, Done, Failed };

const ImU32 kOk = IM_COL32(92, 184, 92, 255);
const ImU32 kWarn = IM_COL32(217, 164, 65, 255);
const ImU32 kOff = IM_COL32(110, 110, 120, 255);
const ImVec4 kErrText = ImVec4(0.90f, 0.33f, 0.35f, 1.0f);
const ImVec4 kDimText = ImVec4(0.58f, 0.58f, 0.63f, 1.0f);

using dod3::utf8;

std::string gb(uint64_t b)
{
    char s[32];
    if (b >= (1ull << 30))
        snprintf(s, sizeof s, "%.1f GB", b / 1073741824.0);
    else
        snprintf(s, sizeof s, "%.0f MB", b / 1048576.0 + 0.5);
    return s;
}

void style()
{
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowPadding = ImVec2(0, 0);
    s.FramePadding = ImVec2(14, 7);
    s.ItemSpacing = ImVec2(10, 8);
    s.FrameRounding = 4;
    s.ChildRounding = 0;
    s.GrabRounding = 4;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.ScrollbarSize = 12;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.059f, 0.059f, 0.071f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Text] = ImVec4(0.91f, 0.90f, 0.89f, 1);
    c[ImGuiCol_TextDisabled] = kDimText;
    c[ImGuiCol_Button] = ImVec4(0.17f, 0.17f, 0.20f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.25f, 0.29f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.31f, 0.31f, 0.36f, 1);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.64f, 0.09f, 0.12f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.13f, 0.15f, 1);
    c[ImGuiCol_Separator] = ImVec4(0.20f, 0.20f, 0.23f, 1);
    c[ImGuiCol_NavCursor] = ImVec4(0.85f, 0.25f, 0.28f, 1);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
}

/* A path in dim text, on one line: if it does not fit, its middle gives way
 * to "..." (the drive and the file name are what tell paths apart). */
void path_line(const char* prefix, const fs::path& p)
{
    const std::string s = utf8(p), pre = prefix;
    const float avail = ImGui::GetContentRegionAvail().x;
    auto width = [](const std::string& t) { return ImGui::CalcTextSize(t.c_str()).x; };
    std::string out = pre + s;
    if (width(out) > avail) {
        auto lead = [&](size_t i) {
            while (i > 0 && i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
            return i;
        };
        /* keep [0, head) and [tail, end), taking from the longer side */
        size_t head = s.size() / 2, tail = s.size() / 2;
        for (;;) {
            out = pre + s.substr(0, lead(head)) + "..." + s.substr(lead(tail));
            if (width(out) <= avail || (head == 0 && tail == s.size())) break;
            if (head > 0 && head >= s.size() - tail)
                head--;
            else
                tail++;
        }
    }
    ImGui::TextDisabled("%s", out.c_str());
}

/* a bullet whose text wraps under itself */
void bullet(const char* t)
{
    ImGui::Bullet();
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(t);
    ImGui::PopTextWrapPos();
}

/* a status light at the start of a line of text */
void light(ImU32 col, bool filled)
{
    const float h = ImGui::GetTextLineHeight(), r = h * 0.28f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 c(p.x + h * 0.5f, p.y + h * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (filled)
        dl->AddCircleFilled(c, r, col);
    else
        dl->AddCircle(c, r, col, 0, h * 0.09f);
    ImGui::Dummy(ImVec2(h, h));
    ImGui::SameLine();
}

struct Item {             /* the disc or the update */
    const char* name;
    const char* need;     /* what to add, when it is missing */
    Source* src;
    bool installed;
};

struct Wizard {
    fs::path base;
    Page page = Page::Welcome;
    bool page_new = true;
    Status have;
    Plan plan;
    std::vector<std::string> notes;           /* what the last files added came to */
    struct Check {
        fs::path path;
        std::future<std::pair<Source, std::string>> fut;
    };
    std::vector<Check> checks;                /* files being recognised */

    /* the install, on its own thread */
    std::thread worker;
    std::atomic<bool> cancel{ false }, finished{ false };
    std::atomic<uint64_t> done{ 0 }, total{ 1 };
    std::mutex what_mu;
    std::string what, error;
    bool ok = false;
    std::chrono::steady_clock::time_point started;

    /* the Keys page: a field per key; a check (decrypting the executable)
     * runs whenever all of them are well-formed and have changed since */
    struct KeyBox {
        char text[160] = "";
    };
    std::vector<KeyBox> boxes = std::vector<KeyBox>(needed_keys().size());
    int key_gen = 0, checked_gen = -1, running_gen = -1;
    std::future<std::pair<int, std::string>> key_check;   /* 0 the keys work, 1 wrong keys, 2 another problem */
    int key_result = -1;
    std::string key_msg, key_note;

    /* tests: DOD3_SETUP_AUTO=<n> presses the primary button of the next n
     * pages; with DOD3_SETUP_GRAB=<prefix>, every page is saved as
     * <prefix>_<n>.ppm (the platform's grab: <prefix>.req -> <prefix>.ppm) */
    int page_frames = 0, auto_left = 0, grabs = 0;
    std::string grab;

    float u() const { return ImGui::GetFontSize(); }
    void go(Page p)
    {
        page = p;
        page_new = true;
        page_frames = 0;
    }

    /* the primary button of a page, in the accent colour */
    bool primary(const char* label, float w, bool enabled = true)
    {
        ImGui::BeginDisabled(!enabled);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.64f, 0.09f, 0.12f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.76f, 0.12f, 0.16f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.84f, 0.16f, 0.20f, 1));
        const bool r = ImGui::Button(label, ImVec2(w, 0));
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        if (enabled && auto_left > 0 && page_frames >= 45) {
            auto_left--;
            return true;
        }
        return r;
    }

    void test_grab()
    {
        if (grab.empty()) return;
        if (page_frames == 20) {
            if (FILE* f = fopen((grab + ".req").c_str(), "wb")) fclose(f);
        }
        if (page_frames == 40) {
            char n[16];
            snprintf(n, sizeof n, "_%d.ppm", grabs++);
            remove((grab + n).c_str());
            rename((grab + ".ppm").c_str(), (grab + n).c_str());
        }
    }

    void add(const std::vector<fs::path>& paths)
    {
        if (paths.empty()) return;
        notes.clear();
        for (const fs::path& p : paths)
            checks.push_back({ p, std::async(std::launch::async, [p] {
                                   Source s;
                                   std::string err;
                                   if (!identify(p, &s, &err)) s = Source();
                                   return std::make_pair(s, err);
                               }) });
    }

    void poll_checks()
    {
        for (size_t i = 0; i < checks.size();) {
            if (checks[i].fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                i++;
                continue;
            }
            auto [s, err] = checks[i].fut.get();
            const std::string name =
                utf8(checks[i].path.filename().empty() ? checks[i].path : checks[i].path.filename());
            switch (s.kind) {
            case SourceKind::Disc: plan.disc = s; break;
            case SourceKind::Update: plan.update = s; break;
            case SourceKind::Eboot: plan.eboot = s; break;
            case SourceKind::Dlc: {
                auto& d = plan.dlc;
                d.erase(std::remove_if(d.begin(), d.end(), [&](const Source& o) { return o.id == s.id; }), d.end());
                d.push_back(s);
                break;
            }
            default: notes.push_back(name + ": " + err); break;
            }
            checks.erase(checks.begin() + (long)i);
        }
    }

    bool base_ready() const
    { return (plan.disc.set() || have.disc) && (!update_required() || plan.update.set() || have.update); }
    /* the executable is still to be made; the player is asked for its keys
     * only when the setup has none built in */
    bool make_elf_needed() const { return !plan.eboot.set() && !have.eboot; }
    bool need_keys() const { return make_elf_needed() && !builtin_keys().complete(); }
    bool keys_ok() const { return key_result == 0 && checked_gen == key_gen; }
    bool eboot_source() const { return (update_required() ? plan.update.set() : plan.disc.set()) || have.eboot_bin; }

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
            key_result = -1;
            return;
        }
        running_gen = key_gen;
        const Plan p = plan;
        const fs::path b = base;
        key_check = std::async(std::launch::async, [b, p, k] {
            std::vector<uint8_t> self, elf;
            std::string err;
            bool wrong = false;
            if (!read_eboot_bin(b, p, &self, &err)) return std::make_pair(2, err);
            if (!make_elf(self, k, &elf, &wrong, &err)) return std::make_pair(wrong ? 1 : 2, err);
            return std::make_pair(0, std::string());
        });
    }
    bool anything() const { return plan.disc.set() || plan.update.set() || plan.eboot.set() || !plan.dlc.empty(); }

    void add_buttons(bool folder)
    {
        const float bw = u() * 8;
        if (ImGui::Button("Add Files...", ImVec2(bw, 0)))
            add(ui_pick_files("Add Drakengard 3 files",
                              { { "Drakengard 3 files (.iso, .pkg)", "*.iso;*.pkg" }, { "All files", "*.*" } }));
        if (folder) {
            ImGui::SameLine();
            if (ImGui::Button("Add Folder...", ImVec2(bw, 0))) {
                const fs::path p = ui_pick_folder("The folder that holds PS3_GAME");
                if (!p.empty()) add({ p });
            }
        }
        if (!checks.empty()) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            static const char* dots[] = { "", ".", "..", "..." };
            ImGui::TextDisabled("Checking %zu file%s%s", checks.size(), checks.size() == 1 ? "" : "s",
                                dots[(int)(ImGui::GetTime() * 3) % 4]);
        }
        for (const std::string& n : notes) {
            ImGui::PushStyleColor(ImGuiCol_Text, kErrText);
            ImGui::TextWrapped("%s", n.c_str());
            ImGui::PopStyleColor();
        }
    }

    void item_row(const Item& it)
    {
        const bool added = it.src->set();
        light(added || it.installed ? kOk : kWarn, added || it.installed);
        ImGui::TextUnformatted(it.name);
        ImGui::Indent(ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.x);
        if (added)
            path_line("", it.src->path);
        else if (it.installed)
            ImGui::TextDisabled("Installed");
        else {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", it.need);
            ImGui::PopTextWrapPos();
        }
        ImGui::Unindent(ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.x);
        ImGui::Spacing();
    }

    void heading(const char* t)
    {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.45f);
        ImGui::TextUnformatted(t);
        ImGui::PopFont();
        ImGui::Spacing();
    }

    void para(const char* t)
    {
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(t);
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
    }

    /* ---- the pages ------------------------------------------------------ */

    void page_welcome()
    {
        heading("Welcome");
        if (have.ready()) {
            para("Drakengard 3 is installed. You can add DLC here, or replace any of the game's files.");
        } else {
            para("This copies Drakengard 3 from your own copy of the game. You will need:");
            ImGui::Indent(u());
            bullet("the game disc: a decrypted .iso, or the folder holding PS3_GAME");
            if (update_required()) bullet("the 1.01 update package (.pkg)");
            bullet("any DLC packages (.pkg) you own (optional)");
            if (need_keys())
                bullet(update_required() ? "the keys that open the update's EBOOT.BIN, the game's executable (asked "
                                           "for on the Keys page)"
                                         : "the keys that open the disc's EBOOT.BIN, the game's executable (asked for "
                                           "on the Keys page)");
            ImGui::Unindent(u());
            ImGui::Spacing();
            para("Nothing is changed in the files you add. Licences are not needed for the DLC.");
        }
        path_line(have.ready() ? "Installed in " : "Installing to ", base);
    }

    void page_game_files()
    {
        heading("Game files");
        para("Add the files below. Any file can be added on any page; each one is recognised by itself.");
        const Item items[] = {
            { "Game disc (BLUS31197)",
              "Add a decrypted .iso with Add Files, or the folder holding PS3_GAME with Add Folder.", &plan.disc,
              have.disc },
            { "Update 1.01", "Add the update package, UP0082-BLUS31197_00-DOD3PATCH0000000 (.pkg).", &plan.update,
              have.update },
        };
        for (const Item& it : items) {
            if (it.src == &plan.update && !update_required()) continue;
            item_row(it);
        }
        ImGui::Spacing();
        add_buttons(true);
    }

    void page_dlc()
    {
        heading("DLC");
        para("Optional. Add the DLC packages (.pkg) you own; you can also come back for them later.");
        uint64_t adding = 0;
        for (const Source& x : plan.dlc) adding += x.bytes;
        const auto& all = known_dlc();
        const size_t rows = (all.size() + 1) / 2;
        if (ImGui::BeginTable("dlc", 2)) {
            for (size_t r = 0; r < rows; r++) {
                ImGui::TableNextRow();
                for (size_t c = 0; c < 2; c++) {
                    ImGui::TableNextColumn();
                    if (c * rows + r >= all.size()) continue;
                    const DlcInfo& d = all[c * rows + r];
                    bool added = false;
                    for (const Source& x : plan.dlc) added |= x.id == d.id;
                    const bool inst = have.dlc.count(d.id) != 0;
                    light(added || inst ? kOk : kOff, added || inst);
                    ImGui::TextUnformatted(d.name);
                    if (added || inst) {
                        ImGui::SameLine();
                        ImGui::TextDisabled(added ? "added" : "installed");
                    }
                }
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (adding) {
            ImGui::TextDisabled("Adding %s", gb(adding).c_str());
            ImGui::Spacing();
        }
        add_buttons(false);
    }

    void page_keys()
    {
        heading("Keys");
        para(update_required()
                 ? "The game's executable is made from the update's EBOOT.BIN, which is encrypted. Enter the keys that "
                   "open it, as hex. They are checked by decrypting it, and kept in this install for next time."
                 : "The game's executable is made from the disc's EBOOT.BIN, which is encrypted. Enter the keys that "
                   "open it, as hex. They are checked by decrypting it, and kept in this install for next time.");
        const auto& f = needed_keys();
        for (size_t i = 0; i < f.size(); i++) {
            std::vector<uint8_t> b;
            const bool empty = boxes[i].text[0] == 0;
            const std::string problem = empty ? "" : key_problem(f[i], boxes[i].text, &b);
            light(empty ? kOff : problem.empty() ? kOk : kWarn, !empty);
            ImGui::TextUnformatted(f[i].label);
            ImGui::SameLine();
            ImGui::TextDisabled("%zu bytes", f[i].bytes);
            char id[16];
            snprintf(id, sizeof id, "##key%zu", i);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText(id, boxes[i].text, sizeof boxes[i].text)) key_gen++;
            if (!problem.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, kErrText);
                ImGui::TextUnformatted(problem.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::Spacing();
        }
        if (ImGui::Button("Load Key File...", ImVec2(u() * 9, 0))) {
            const auto picked = ui_pick_files("A key file (name=hex lines)",
                                              { { "Key files (.txt)", "*.txt" }, { "All files", "*.*" } });
            if (!picked.empty()) {
                Keys k;
                std::string err;
                if (keys_load(picked[0], &k, &err)) {
                    fill_boxes(k);
                    key_note = std::to_string(k.v.size()) + " of " + std::to_string(f.size()) + " keys found in " +
                               utf8(picked[0].filename());
                } else {
                    key_note = utf8(picked[0].filename()) + ": " + err;
                }
            }
        }
        if (!key_note.empty()) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", key_note.c_str());
        }
        ImGui::Spacing();
        if (!eboot_source()) {
            para(update_required() ? "Add the update package on the Game files page: the executable is made from it."
                                   : "Add the disc on the Game files page: the executable is made from it.");
        } else if (running_gen >= 0) {
            ImGui::TextDisabled("Checking the keys...");
        } else if (keys_ok()) {
            light(kOk, true);
            ImGui::TextUnformatted("These keys open the game's executable.");
        } else if (key_result > 0 && checked_gen == key_gen) {
            ImGui::PushStyleColor(ImGuiCol_Text, kErrText);
            ImGui::PushTextWrapPos(0);
            if (key_result == 1)
                ImGui::TextUnformatted(
                    "These keys do not open EBOOT.BIN. Check each one against your key set (key revision 0x1C).");
            else
                ImGui::TextUnformatted(key_msg.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
    }

    void page_confirm()
    {
        heading("Install");
        const uint64_t need = plan.bytes(), space = free_bytes(base);
        if (!anything() && !make_elf_needed()) {
            para("Nothing new to install.");
            return;
        }
        para("Ready to install:");
        if (ImGui::BeginTable("plan", 2, ImGuiTableFlags_SizingStretchProp)) {
            auto row = [](const std::string& a, uint64_t b) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(a.c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", gb(b).c_str());
            };
            if (plan.disc.set()) row("Game disc", plan.disc.bytes);
            if (plan.update.set()) row("Update 1.01", plan.update.bytes);
            if (plan.eboot.set())
                row("EBOOT.ELF", plan.eboot.bytes);
            else if (make_elf_needed())
                row(need_keys()         ? "The game's executable (made with your keys)"
                    : update_required() ? "The game's executable (made from the update)"
                                        : "The game's executable (made from the disc)",
                    update_required() ? 26872424u : 26872552u);
            for (const Source& d : plan.dlc) row(d.name, d.bytes);
            ImGui::EndTable();
        }
        ImGui::Separator();
        ImGui::Text("Needs %s; %s free on the drive.", gb(need).c_str(), gb(space).c_str());
        path_line("To ", base);
        if (space && need > space) {
            ImGui::PushStyleColor(ImGuiCol_Text, kErrText);
            ImGui::TextWrapped("Not enough space. Free %s, or install fewer DLC packs.", gb(need - space).c_str());
            ImGui::PopStyleColor();
        }
    }

    void page_installing()
    {
        heading("Installing");
        std::string w;
        {
            std::lock_guard<std::mutex> g(what_mu);
            w = what;
        }
        ImGui::TextUnformatted(w.empty() ? "Starting..." : w.c_str());
        ImGui::Spacing();
        const uint64_t d = done, t = total ? total.load() : 1;
        const float f = (float)((double)d / (double)t);
        char ov[64] = "";
        if (!w.empty()) snprintf(ov, sizeof ov, "%s of %s", gb(d).c_str(), gb(t).c_str());
        ImGui::ProgressBar(f, ImVec2(-FLT_MIN, u() * 1.6f), ov);
        ImGui::Spacing();
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (secs > 3 && d > 0 && d < t) {
            const double rate = d / secs, left = (t - d) / rate;
            ImGui::TextDisabled("%.0f MB/s, about %d min %02d s left", rate / 1048576.0, (int)left / 60,
                                (int)left % 60);
        }
        if (cancel) ImGui::TextDisabled("Cancelling...");
    }

    void page_done()
    {
        heading("Installed");
        para("Drakengard 3 is ready to play.");
        if (!have.dlc.empty())
            ImGui::TextDisabled("%zu DLC pack%s installed.", have.dlc.size(), have.dlc.size() == 1 ? "" : "s");
    }

    void page_failed()
    {
        heading(cancel ? "Cancelled" : "Installation failed");
        if (!cancel) {
            ImGui::PushStyleColor(ImGuiCol_Text, kErrText);
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(error.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }
        para("The part that was being installed has been removed; the parts before it are kept.");
    }

    void start_install()
    {
        cancel = false;
        finished = false;
        done = 0;
        total = 1;
        {
            std::lock_guard<std::mutex> g(what_mu);
            what.clear();
        }
        started = std::chrono::steady_clock::now();
        plan.keys = builtin_keys().complete() ? builtin_keys() : keys_from_boxes();
        const Plan p = plan;
        worker = std::thread([this, p] {
            std::string err;
            const bool r = install(
                base, p,
                [this](uint64_t d, uint64_t t, const std::string& w) {
                    done = d;
                    total = t ? t : 1;
                    {
                        std::lock_guard<std::mutex> g(what_mu);
                        what = w;
                    }
                    return !cancel.load();
                },
                &err);
            ok = r;
            error = err;
            finished = true;
        });
        go(Page::Installing);
    }

    /* ---- the frame ---------------------------------------------------- */

    /* returns -1 to go on, else run_installer's result */
    int frame()
    {
        page_frames++;
        test_grab();
        poll_checks();
        poll_keys();
        if (page == Page::Installing && finished) {
            worker.join();
            have = installed(base);
            if (ok) {
                plan = Plan();
                go(Page::Done);
            } else
                go(Page::Failed);
        }
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##setup", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        const float U = u(), pad = U * 1.6f, footer = U * 3.4f, side = U * 12;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(side, io.DisplaySize.y), IM_COL32(22, 22, 27, 255));

        /* the side: title and steps */
        ImGui::SetCursorPos(ImVec2(pad, pad));
        ImGui::BeginGroup();
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.35f);
        ImGui::TextUnformatted("DRAKENGARD 3");
        ImGui::PopFont();
        ImGui::TextDisabled("Recompiled  -  Setup");
        ImGui::Dummy(ImVec2(0, U * 1.5f));
        const char* steps[] = { "Welcome", "Game files", "DLC", "Keys", "Install" };
        const int at = page == Page::Welcome     ? 0
                       : page == Page::GameFiles ? 1
                       : page == Page::Dlc       ? 2
                       : page == Page::Keys      ? 3
                                                 : 4;
        for (int i = 0; i < 5; i++) {
            if (i == 3 && !need_keys() && at != 3) continue;   /* the Keys step only when the player is asked */
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (i == at)
                dl->AddRectFilled(ImVec2(p.x - pad, p.y - 2),
                                  ImVec2(p.x - pad + U * 0.22f, p.y + ImGui::GetTextLineHeight() + 2),
                                  IM_COL32(163, 23, 31, 255));
            if (i == at)
                ImGui::TextUnformatted(steps[i]);
            else
                ImGui::TextDisabled("%s", steps[i]);
            ImGui::Dummy(ImVec2(0, U * 0.3f));
        }
        ImGui::EndGroup();
        ImGui::SetCursorPos(ImVec2(pad, io.DisplaySize.y - pad - ImGui::GetTextLineHeight()));
        ImGui::TextDisabled("Version %s", DOD3_VERSION_STRING);

        /* the page */
        ImGui::SetCursorPos(ImVec2(side + pad, pad));
        ImGui::BeginChild("page", ImVec2(io.DisplaySize.x - side - pad * 2, io.DisplaySize.y - pad - footer),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        switch (page) {
        case Page::Welcome: page_welcome(); break;
        case Page::GameFiles: page_game_files(); break;
        case Page::Dlc: page_dlc(); break;
        case Page::Keys: page_keys(); break;
        case Page::Confirm: page_confirm(); break;
        case Page::Installing: page_installing(); break;
        case Page::Done: page_done(); break;
        case Page::Failed: page_failed(); break;
        }
        ImGui::EndChild();

        /* the buttons, right-aligned */
        const float bw = U * 7.5f, gap = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPos(ImVec2(side + pad, io.DisplaySize.y - footer + U * 0.4f));
        dl->AddLine(ImVec2(side, io.DisplaySize.y - footer), ImVec2(io.DisplaySize.x, io.DisplaySize.y - footer),
                    IM_COL32(40, 40, 46, 255));
        auto at_right = [&](int n) { ImGui::SetCursorPosX(io.DisplaySize.x - pad - n * bw - (n - 1) * gap); };
        const bool focus = page_new && io.NavVisible;
        page_new = false;
        int result = -1;
        const bool busy = !checks.empty();
        switch (page) {
        case Page::Welcome:
            at_right(have.ready() ? 3 : 2);
            if (ImGui::Button("Quit", ImVec2(bw, 0))) result = 1;
            if (have.ready()) {
                ImGui::SameLine();
                if (ImGui::Button("Start Game", ImVec2(bw, 0))) result = 0;
            }
            ImGui::SameLine();
            if (focus) ImGui::SetKeyboardFocusHere();
            if (primary("Next", bw)) go(Page::GameFiles);
            break;
        case Page::GameFiles:
            at_right(2);
            if (ImGui::Button("Back", ImVec2(bw, 0))) go(Page::Welcome);
            ImGui::SameLine();
            if (focus) ImGui::SetKeyboardFocusHere();
            if (primary("Next", bw, base_ready() && !busy)) go(Page::Dlc);
            break;
        case Page::Dlc:
            at_right(2);
            if (ImGui::Button("Back", ImVec2(bw, 0))) go(Page::GameFiles);
            ImGui::SameLine();
            if (focus) ImGui::SetKeyboardFocusHere();
            if (primary(plan.dlc.empty() ? "Skip" : "Next", bw, !busy)) go(need_keys() ? Page::Keys : Page::Confirm);
            break;
        case Page::Keys:
            at_right(2);
            if (ImGui::Button("Back", ImVec2(bw, 0))) go(Page::Dlc);
            ImGui::SameLine();
            if (focus) ImGui::SetKeyboardFocusHere();
            if (primary("Next", bw, keys_ok())) go(Page::Confirm);
            break;
        case Page::Confirm: {
            const uint64_t need = plan.bytes(), space = free_bytes(base);
            at_right(2);
            if (ImGui::Button("Back", ImVec2(bw, 0))) go(need_keys() ? Page::Keys : Page::Dlc);
            ImGui::SameLine();
            if (focus) ImGui::SetKeyboardFocusHere();
            if (!anything() && !make_elf_needed()) {
                if (primary(have.ready() ? "Start Game" : "Back", bw)) {
                    if (have.ready())
                        result = 0;
                    else
                        go(Page::GameFiles);
                }
            } else {
                if (primary("Install", bw, !(space && need > space))) start_install();
            }
            break;
        }
        case Page::Installing:
            at_right(1);
            ImGui::BeginDisabled(cancel.load());
            if (ImGui::Button("Cancel", ImVec2(bw, 0))) cancel = true;
            ImGui::EndDisabled();
            break;
        case Page::Done:
            at_right(3);
            if (ImGui::Button("Add DLC", ImVec2(bw, 0))) go(Page::Dlc);
            ImGui::SameLine();
            if (ImGui::Button("Quit", ImVec2(bw, 0))) result = 1;
            ImGui::SameLine();
            if (focus) ImGui::SetKeyboardFocusHere();
            if (primary("Start Game", bw)) result = 0;
            break;
        case Page::Failed:
            at_right(2);
            if (ImGui::Button("Quit", ImVec2(bw, 0))) result = 1;
            ImGui::SameLine();
            if (focus) ImGui::SetKeyboardFocusHere();
            if (primary("Back", bw)) {
                cancel = false;
                go(Page::Confirm);
            }
            break;
        }
        ImGui::End();
        return result;
    }
};

}  // namespace

int run_installer(const fs::path& base)
{
    if (!ui_init("Drakengard 3 Recompiled Setup", 900, 560, ui_default_font(), 18.0f)) return -1;
    style();
    Wizard w;
    w.base = base;
    if (const char* a = getenv("DOD3_SETUP_AUTO")) w.auto_left = atoi(a);
    if (const char* g = getenv("DOD3_SETUP_GRAB")) w.grab = g;
    if (const char* f = getenv("DOD3_SETUP_ADD")) {   /* '|'-separated */
        std::vector<fs::path> paths;
        for (const char* s = f; *s;) {
            const char* e = strchr(s, '|');
            const std::string one = e ? std::string(s, e) : std::string(s);
            if (!one.empty()) paths.push_back(fs::u8path(one));
            if (!e) break;
            s = e + 1;
        }
        w.add(paths);
    }
    w.have = installed(base);
    {   /* the keys this install was made with, if any */
        Keys k;
        std::string err;
        std::error_code ec;
        if (fs::exists(keys_file(base), ec) && keys_load(keys_file(base), &k, &err)) w.fill_boxes(k);
    }
    int result = 1;
    while (ui_frame_begin()) {
        const int r = w.frame();
        ui_frame_end();
        if (r >= 0) {
            result = r;
            break;
        }
    }
    /* the window was closed mid-install: stop the install first */
    if (w.worker.joinable()) {
        w.cancel = true;
        w.worker.join();
    }
    for (auto& c : w.checks) c.fut.wait();
    if (w.running_gen >= 0) w.key_check.wait();
    ui_shutdown();
    return result == 0 && installed(base).ready() ? 0 : 1;
}

}  // namespace dod3setup
