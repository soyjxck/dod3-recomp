/* The launcher's Trophies panel (ui/trophies.h). */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ui/trophies.h"

#include "lang.h"          /* tr: the text in the player's language */
#include "util.h"          /* utf8 */
#include "trophy_pack.h"   /* ps3recomp: the disc's trophy list, the unlocks' file */

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>

namespace dod3setup {

bool png_rgba(const std::vector<uint8_t>& file, std::vector<uint8_t>* rgba, int* w, int* h);   /* ui/png.cpp */

namespace fs = std::filesystem;
using namespace look;
using dod3::tr;
using dod3::trf;

namespace {

struct Icon {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    ImTextureData* tex = nullptr;
};

const char kSep[] = " \xC2\xB7 ";

std::string date_text(uint64_t at)
{
    if (!at) return tr("Earned");
    const time_t t = (time_t)at;
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    static const char* const kMonths[12] = { TR_NOOP("Jan"), TR_NOOP("Feb"), TR_NOOP("Mar"), TR_NOOP("Apr"),
                                             TR_NOOP("May"), TR_NOOP("Jun"), TR_NOOP("Jul"), TR_NOOP("Aug"),
                                             TR_NOOP("Sep"), TR_NOOP("Oct"), TR_NOOP("Nov"), TR_NOOP("Dec") };
    /* TRANSLATORS: a date: {1} the day, {2} the month's short name, {3} the year ("4 Oct 2026") */
    return trf("{1} {2} {3}", tmv.tm_mday, tr(kMonths[tmv.tm_mon % 12]), tmv.tm_year + 1900);
}

/* a trophy's grade, in the player's language (trophy_grade_name's words) */
const char* grade_name(char grade)
{
    switch (grade) {
    case 'P': return tr("Platinum");
    case 'G': return tr("Gold");
    case 'S': return tr("Silver");
    case 'B': return tr("Bronze");
    default: return tr("Trophy");
    }
}

}  // namespace

struct TrophiesState {
    fs::path base;
    std::thread loader;
    std::atomic<bool> loaded{ false };
    TrophyPack* pack = nullptr;
    TrophyUnlocks u{};
    std::vector<Icon> icons;   /* by id */

    int filter = 0;            /* 0 all, 1 earned, 2 not earned */
    bool show_hidden = false;

    ~TrophiesState()
    {
        if (loader.joinable()) loader.join();
        trophy_pack_free(pack);
        for (Icon& i : icons) delete i.tex;
    }

    int earned() const
    {
        int n = 0;
        for (int i = 0; pack && i < pack->end; i++) n += pack->trophies[i].valid && u.unlocked[i];
        return n;
    }

    void load()
    {
        char trp[1024];
        if (trophy_pack_find(dod3::utf8(base / "game" / "disc").c_str(), nullptr, trp, sizeof trp) != 0) return;
        TrophyPack* pk = trophy_pack_load(trp, TROPHY_LANG_ENGLISH_US);
        if (!pk) return;
        fs::path dir = base / "gamedata" / "trophies";
        if (const char* g = getenv("PS3_GAMEDATA_ROOT"))
            if (*g) dir = fs::u8path(g) / "trophies";
        char json[1024];
        trophy_unlocks_path(dod3::utf8(dir).c_str(), pk->np_comm_id, json, sizeof json);
        trophy_unlocks_load(json, &u);
        icons.resize(TROPHY_PACK_MAX);
        for (int id = 0; id < pk->end; id++) {
            if (!pk->trophies[id].valid) continue;
            uint8_t* png = nullptr;
            size_t n = 0;
            if (trophy_pack_icon(pk, id, &png, &n) != 0) continue;
            const std::vector<uint8_t> file(png, png + n);
            free(png);
            Icon& ic = icons[(size_t)id];
            if (!png_rgba(file, &ic.rgba, &ic.w, &ic.h)) continue;
            if (ic.w >= 160 && ic.h >= 160) {   /* half size: the rows show them at 48 design units */
                const int w = ic.w / 2, h = ic.h / 2;
                std::vector<uint8_t> o((size_t)w * h * 4);
                const size_t row = (size_t)ic.w * 4;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        for (int c = 0; c < 4; c++) {
                            const uint8_t* p = &ic.rgba[((size_t)(2 * y) * ic.w + 2 * x) * 4 + c];
                            o[((size_t)y * w + x) * 4 + c] = (uint8_t)((p[0] + p[4] + p[row] + p[row + 4] + 2) / 4);
                        }
                ic.rgba.swap(o);
                ic.w = w;
                ic.h = h;
            }
        }
        pack = pk;
    }
};

TrophiesPanel::TrophiesPanel() : st(new TrophiesState) {}
TrophiesPanel::~TrophiesPanel() = default;

void TrophiesPanel::start(const fs::path& base)
{
    st->base = base;
    TrophiesState* s = st.get();
    st->loader = std::thread([s] {
        s->load();
        s->loaded = true;
    });
}

std::string TrophiesPanel::summary() const
{
    if (!st->loaded) return "";
    if (!st->pack) return tr("No trophy list in this install");
    const int got = st->earned(), all = st->pack->count;
    return trf("{1} of {2} earned", got, all) + kSep + std::to_string(all ? (got * 100 + all / 2) / all : 0) + "%";
}

bool TrophiesPanel::typing() const { return ImGui::GetIO().WantTextInput; }

namespace {

/* the four grades' counts: a cup, earned / all */
void grade_counts(Kit& k, const TrophiesState& s)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char grades[4] = { 'P', 'G', 'S', 'B' };
    float x = ImGui::GetCursorScreenPos().x;
    const float y = ImGui::GetCursorScreenPos().y;
    for (char g : grades) {
        int all = 0, got = 0;
        for (int i = 0; i < s.pack->end; i++) {
            const TrophyPackTrophy& t = s.pack->trophies[i];
            if (!t.valid || t.grade != g) continue;
            all++;
            got += s.u.unlocked[i];
        }
        trophy_cup(dl, ImVec2(x + k.px(8), y + k.px(10)), k.px(16), grade_colour(g));
        char t[24];
        snprintf(t, sizeof t, "%d / %d", got, all);
        dl->AddText(k.F.semibold, k.px(15), ImVec2(x + k.px(22), y + k.px(1)), kText, t);
        x += k.px(22) + k.text_size(k.F.semibold, 15, t).x + k.px(26);
    }
    ImGui::Dummy(ImVec2(0, k.px(22)));
}

/* one trophy's line */
void trophy_row(Kit& k, TrophiesState& s, int id)
{
    const TrophyPackTrophy& t = s.pack->trophies[id];
    const bool got = s.u.unlocked[id] != 0;
    const bool secret = t.hidden && !got && !s.show_hidden;
    Icon& ic = s.icons[(size_t)id];
    if (!ic.tex && !ic.rgba.empty()) {
        ic.tex = art_texture(ic.rgba, ic.w, ic.h);
        ic.rgba.clear();
        ic.rgba.shrink_to_fit();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x, h = k.px(68);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    Kit::hit("##t", ImVec2(w, h));
    const bool hot = Kit::hot_item();
    ImGui::PopID();
    dl->AddRectFilled(p, p + ImVec2(w, h), IM_COL32(255, 255, 255, hot ? 16 : got ? 8 : 3), k.px(12));

    /* the icon, dimmed until earned; a hidden one's is a question mark */
    const float is = k.px(48);
    const ImVec2 i0(p.x + k.px(10), p.y + (h - is) / 2), i1 = i0 + ImVec2(is, is);
    if (secret || !ic.tex) {
        dl->AddRectFilled(i0, i1, IM_COL32(255, 255, 255, 12), k.px(8));
        if (secret) {
            const ImVec2 qs = k.text_size(k.F.display, 24, "?");
            dl->AddText(k.F.display, k.px(24), (i0 + i1) * 0.5f - qs * 0.5f, kFaint, "?");
        } else {
            trophy_cup(dl, (i0 + i1) * 0.5f, is * 0.5f, alpha(grade_colour(t.grade), got ? 1.0f : 0.45f));
        }
    } else {
        dl->AddImageRounded(ic.tex->GetTexRef(), i0, i1, ImVec2(0, 0), ImVec2(1, 1),
                            got ? IM_COL32_WHITE : IM_COL32(150, 150, 150, 120), k.px(8));
    }
    dl->AddRect(i0, i1, IM_COL32(255, 255, 255, 20), k.px(8));

    /* the grade and when, at the right */
    const float rw = k.px(128), rx = p.x + w - rw - k.px(12);
    const ImU32 gc = got ? grade_colour(t.grade) : alpha(grade_colour(t.grade), 0.5f);
    trophy_cup(dl, ImVec2(rx + k.px(7), p.y + k.px(22)), k.px(13), gc);
    dl->AddText(k.F.semibold, k.px(12.5f), ImVec2(rx + k.px(19), p.y + k.px(14)), gc, grade_name(t.grade));
    const std::string when = got ? date_text(s.u.at[id]) : tr("Not earned");
    dl->AddText(k.F.regular, k.px(12.5f), ImVec2(rx + k.px(1), p.y + k.px(36)), got ? kDim : kFaint, when.c_str());

    /* the name and what it is for */
    const float tx = i1.x + k.px(14), tw = rx - tx - k.px(12);
    const std::string name = fit_line(k.F.semibold, k.px(15.5f), secret ? tr("Hidden Trophy") : t.name, tw);
    const std::string detail =
        fit_line(k.F.regular, k.px(13), secret ? tr("Earn it to see what it is.") : t.detail, tw);
    dl->AddText(k.F.semibold, k.px(15.5f), ImVec2(tx, p.y + k.px(15)), got ? kText : alpha(kText, 0.72f), name.c_str());
    dl->AddText(k.F.regular, k.px(13), ImVec2(tx, p.y + k.px(37)), got ? kDim : alpha(kDim, 0.75f), detail.c_str());
    ImGui::Dummy(ImVec2(0, k.px(4)));
}

}  // namespace

void TrophiesPanel::draw(Kit& k, ImVec2 body, ImVec2 size, bool focus)
{
    TrophiesState& s = *st;
    ImGui::SetCursorScreenPos(body);
    {   /* tests: DOD3_TROPHIES_SCROLL=<design units> starts the list scrolled */
        static int frames;   /* a few: the first frames' scroll is clamped to a list not yet measured */
        const char* e = getenv("DOD3_TROPHIES_SCROLL");
        if (e && s.loaded && frames < 10) {
            ImGui::SetNextWindowScroll(ImVec2(0, k.px((float)atof(e))));
            frames++;
        }
    }
    ImGui::BeginChild("##trophies", size, ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
    if (!s.loaded) {
        k.para(k.F.regular, 14, kDim, tr("Reading the trophy list..."));
        ImGui::EndChild();
        return;
    }
    if (!s.pack) {
        k.label(k.F.semibold, 19, kText, tr("No trophy list"));
        ImGui::Dummy(ImVec2(0, k.px(6)));
        k.para(k.F.regular, 14, kDim,
               tr("This install has no PS3_GAME/TROPDIR. Game Files, at the bottom, installs the disc again with it."));
        ImGui::EndChild();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();

    /* how far along: the percentage, the count, the bar, the grades */
    const int got = s.earned(), all = s.pack->count;
    const int pct = all ? (got * 100 + all / 2) / all : 0;
    char big[16];
    snprintf(big, sizeof big, "%d%%", pct);
    const std::string line = trf("{1} of {2} trophies earned", got, all);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddText(k.F.display, k.px(38), p, kText, big);
    const float bx = p.x + k.text_size(k.F.display, 38, big).x + k.px(16);
    dl->AddText(k.F.semibold, k.px(15), ImVec2(bx, p.y + k.px(6)), kText, line.c_str());
    dl->AddText(k.F.regular, k.px(13), ImVec2(bx, p.y + k.px(26)), kDim, s.pack->title);
    ImGui::Dummy(ImVec2(0, k.px(50)));
    k.progress(all ? (float)got / (float)all : 0.0f);
    ImGui::Dummy(ImVec2(0, k.px(10)));
    grade_counts(k, s);
    ImGui::Dummy(ImVec2(0, k.px(4)));

    /* All / Earned / Not Earned, and Show Hidden */
    static const char* const kFilters[3] = { TR_NOOP("All"), TR_NOOP("Earned"), TR_NOOP("Not Earned") };
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1)) s.filter = (s.filter + 2) % 3;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1)) s.filter = (s.filter + 1) % 3;
    const float x0 = ImGui::GetCursorScreenPos().x, right = x0 + ImGui::GetContentRegionAvail().x;
    float x = x0;
    const float y = ImGui::GetCursorScreenPos().y;
    for (int f = 0; f < 3; f++) {
        const char* name = tr(kFilters[f]);
        const ImVec2 ts = k.text_size(k.F.semibold, 14, name);
        const ImVec2 tp(x, y), tsz(ts.x + k.px(32), k.px(36));
        ImGui::SetCursorScreenPos(tp);
        ImGui::PushID(f);
        if (f == 0 && focus) ImGui::SetKeyboardFocusHere();
        if (Kit::hit("##filter", tsz) && k.cooldown == 0) s.filter = f;
        const bool hot = Kit::hot_item();
        ImGui::PopID();
        if (f == s.filter)
            dl->AddRectFilled(tp, tp + tsz, IM_COL32(255, 255, 255, 30), k.px(18));
        else if (hot)
            dl->AddRectFilled(tp, tp + tsz, IM_COL32(255, 255, 255, 12), k.px(18));
        dl->AddText(k.F.semibold, k.px(14), ImVec2(tp.x + k.px(16), tp.y + (tsz.y - ts.y) / 2), f == s.filter ? kText : kDim,
                    name);
        x += tsz.x + k.px(6);
    }
    {   /* Show Hidden, at the right of the tabs */
        const ImVec2 ts = k.toggle_size();
        const char* lt = tr("Show Hidden");
        const ImVec2 ls = k.text_size(k.F.semibold, 13, lt);
        ImGui::SetCursorScreenPos(ImVec2(right - ts.x - k.px(4), y + (k.px(36) - ts.y) / 2));
        if (k.toggle("##hidden", s.show_hidden)) s.show_hidden = !s.show_hidden;
        dl->AddText(k.F.semibold, k.px(13), ImVec2(right - ts.x - k.px(14) - ls.x, y + (k.px(36) - ls.y) / 2), kDim, lt);
    }
    ImGui::SetCursorScreenPos(ImVec2(x0, y + k.px(46)));

    /* the base game's, then each group's (the DLC chapters) */
    auto wanted = [&](int id) {
        const TrophyPackTrophy& t = s.pack->trophies[id];
        if (!t.valid) return false;
        const bool g = s.u.unlocked[id] != 0;
        return s.filter == 0 || (s.filter == 1 && g) || (s.filter == 2 && !g);
    };
    auto section = [&](int group, const char* title) {
        int n = 0, have = 0, total = 0;
        for (int id = 0; id < s.pack->end; id++) {
            if (!s.pack->trophies[id].valid || s.pack->trophies[id].group != group) continue;
            total++;
            have += s.u.unlocked[id];
            n += wanted(id);
        }
        if (!n) return;
        ImGui::Dummy(ImVec2(0, k.px(6)));
        char h[200];
        snprintf(h, sizeof h, "%s%s%d / %d", title, kSep, have, total);
        k.label(k.F.semibold, 13, kDim, h);
        ImGui::Dummy(ImVec2(0, k.px(4)));
        for (int id = 0; id < s.pack->end; id++)
            if (s.pack->trophies[id].group == group && wanted(id)) trophy_row(k, s, id);
    };
    section(-1, s.pack->title);
    for (int g = 0; g < s.pack->group_count; g++) section(s.pack->groups[g].id, s.pack->groups[g].name);
    ImGui::Dummy(ImVec2(0, k.px(10)));
    ImGui::EndChild();
}

}  // namespace dod3setup
