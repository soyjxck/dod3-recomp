/* The look the launcher and the installer share (ui/look.h). */
#include "ui/look.h"
#include "util.h"
#include "launcher_fonts.h"   /* generated: kFont_* */

#include "imgui_internal.h"   /* RegisterUserTexture */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dod3setup {

bool png_rgba(const std::vector<uint8_t>& file, std::vector<uint8_t>* rgba, int* w, int* h);   /* ui/png.cpp */

namespace look {

namespace fs = std::filesystem;

ImU32 alpha(ImU32 c, float a)
{
    const int na = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.0f, 1.0f));
    return (c & ~IM_COL32_A_MASK) | ((ImU32)na << IM_COL32_A_SHIFT);
}

ImU32 mix(ImU32 a, ImU32 b, float t)
{
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImLerp(x, y, std::clamp(t, 0.0f, 1.0f)));
}

float ease_out(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

std::string size_text(uint64_t bytes)
{
    char s[32];
    if (bytes >= (1ull << 30))
        snprintf(s, sizeof s, "%.1f GB", bytes / 1073741824.0);
    else
        snprintf(s, sizeof s, "%.0f MB", bytes / 1048576.0);
    return s;
}

ImU32 grade_colour(char grade)
{
    switch (grade) {
    case 'P': return IM_COL32(184, 198, 219, 255);
    case 'G': return IM_COL32(227, 179, 65, 255);
    case 'S': return IM_COL32(196, 200, 204, 255);
    default:  return IM_COL32(205, 127, 50, 255);
    }
}

void trophy_cup(ImDrawList* dl, ImVec2 c, float size, ImU32 col)
{
    const float r = size * 0.30f, top = c.y - size * 0.45f;
    /* the bowl: the lower half of a circle under the rim */
    dl->PathArcTo(ImVec2(c.x, top), r, 0.0f, IM_PI, 16);
    dl->PathFillConvex(col);
    /* the handles */
    const float hr = size * 0.12f, th = size * 0.07f;
    dl->PathArcTo(ImVec2(c.x - r, top + size * 0.11f), hr, IM_PI * 0.5f, IM_PI * 1.5f, 8);
    dl->PathStroke(col, 0, th);
    dl->PathArcTo(ImVec2(c.x + r, top + size * 0.11f), hr, -IM_PI * 0.5f, IM_PI * 0.5f, 8);
    dl->PathStroke(col, 0, th);
    /* the stem and the foot */
    dl->AddRectFilled(ImVec2(c.x - size * 0.05f, top + r - 1.0f), ImVec2(c.x + size * 0.05f, c.y + size * 0.27f), col);
    dl->AddRectFilled(ImVec2(c.x - size * 0.24f, c.y + size * 0.27f), ImVec2(c.x + size * 0.24f, c.y + size * 0.45f), col,
                      size * 0.05f);
}

void load_fonts(Fonts& F)
{
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;   /* the arrays are the program's own */
    F.regular = io.Fonts->AddFontFromMemoryTTF((void*)kFont_Inter_Regular, (int)sizeof kFont_Inter_Regular, 17, &cfg);
    F.medium = io.Fonts->AddFontFromMemoryTTF((void*)kFont_Inter_Medium, (int)sizeof kFont_Inter_Medium, 17, &cfg);
    F.semibold = io.Fonts->AddFontFromMemoryTTF((void*)kFont_Inter_SemiBold, (int)sizeof kFont_Inter_SemiBold, 17, &cfg);
    F.display =
        io.Fonts->AddFontFromMemoryTTF((void*)kFont_InterDisplay_Bold, (int)sizeof kFont_InterDisplay_Bold, 17, &cfg);
    io.FontDefault = F.regular;
    ImGui::GetStyle().FontSizeBase = 17;
}

void style()
{
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowPadding = ImVec2(0, 0);
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.FrameBorderSize = 1;
    s.ScrollbarSize = 8;
    s.ScrollbarRounding = 8;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.035f, 0.043f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(kText);
    c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.05f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_TextSelectedBg] = ImGui::ColorConvertU32ToFloat4(alpha(kAccent, 0.45f));
    c[ImGuiCol_InputTextCursor] = ImGui::ColorConvertU32ToFloat4(kText);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.22f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(1, 1, 1, 0.30f);
    c[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);   /* the items show their own focus */
}

/* The disc's picture (the PSN release has the same file) carries the Dolby
 * and DTS logos and their trademark lines, which have no place here: they
 * are painted out. The logos, on black: every grey or white pixel of their
 * boxes; the two lines, partly over the dragon's tail: their pure-white
 * pixels. Each grown by a few pixels (the soft edges, the lines' dark
 * outline), then filled from the pixels around, inward. */
static void clean_disc_art(std::vector<uint8_t>& px, int w, int h)
{
    struct Box {
        int x0, y0, x1, y1;
        bool white;
        int grow;
    };
    const Box boxes[] = { { 902, 716, 1172, 796, false, 1 },
                          { 902, 842, 1104, 923, false, 1 },
                          { 452, 962, 1332, 1030, true, 5 } };
    std::vector<uint8_t> mask((size_t)w * h);
    auto at = [&](int x, int y) { return &px[((size_t)y * w + x) * 4]; };
    for (const Box& b : boxes) {
        for (int y = b.y0; y < b.y1; y++)
            for (int x = b.x0; x < b.x1; x++) {
                const uint8_t* p = at(x, y);
                const int hi = std::max({ p[0], p[1], p[2] }), lo = std::min({ p[0], p[1], p[2] });
                if (b.white ? lo > 236 : (hi - lo < 48 && hi > 20)) mask[(size_t)y * w + x] = 1;
            }
        for (int g = 0; g < b.grow; g++) {
            std::vector<size_t> add;
            for (int y = std::max(1, b.y0 - b.grow); y < std::min(h - 1, b.y1 + b.grow); y++)
                for (int x = std::max(1, b.x0 - b.grow); x < std::min(w - 1, b.x1 + b.grow); x++) {
                    const size_t i = (size_t)y * w + x;
                    if (!mask[i] && (mask[i - 1] || mask[i + 1] || mask[i - w] || mask[i + w])) add.push_back(i);
                }
            for (size_t i : add) mask[i] = 1;
        }
    }
    std::vector<size_t> todo;
    for (size_t i = 0; i < mask.size(); i++)
        if (mask[i]) todo.push_back(i);
    while (!todo.empty()) {
        std::vector<size_t> next;
        std::vector<std::pair<size_t, uint32_t>> filled;
        for (size_t i : todo) {
            const int x = (int)(i % w), y = (int)(i / w);
            int acc[3] = { 0, 0, 0 }, n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    const int nx = x + dx, ny = y + dy;
                    if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= w || ny >= h || mask[(size_t)ny * w + nx]) continue;
                    const uint8_t* p = at(nx, ny);
                    acc[0] += p[0];
                    acc[1] += p[1];
                    acc[2] += p[2];
                    n++;
                }
            if (n >= 2)
                filled.push_back(
                    { i, (uint32_t)(acc[0] / n) | (uint32_t)(acc[1] / n) << 8 | (uint32_t)(acc[2] / n) << 16 });
            else
                next.push_back(i);
        }
        if (filled.empty()) break;
        for (const auto& f : filled) {
            uint8_t* p = &px[f.first * 4];
            p[0] = (uint8_t)f.second;
            p[1] = (uint8_t)(f.second >> 8);
            p[2] = (uint8_t)(f.second >> 16);
            mask[f.first] = 0;
        }
        todo.swap(next);
    }
}

bool art_pixels(const std::vector<uint8_t>& png, std::vector<uint8_t>* rgba, int* w, int* h)
{
    if (!png_rgba(png, rgba, w, h) || *w <= 0 || *h <= 0) return false;
    if (png.size() == 1181776 && *w == 1920 && *h == 1080) clean_disc_art(*rgba, *w, *h);   /* the disc's own */
    return true;
}

ImTextureData* art_texture(const std::vector<uint8_t>& rgba, int w, int h)
{
    ImTextureData* t = new ImTextureData();
    t->Create(ImTextureFormat_RGBA32, w, h);
    memcpy(t->Pixels, rgba.data(), rgba.size());
    t->UseColors = true;
    ImGui::RegisterUserTexture(t);
    return t;
}

ImTextureData* load_art(const fs::path& base)
{
    std::error_code ec;
    const fs::path p = base / "game/disc/PS3_GAME/PIC1.PNG";
    const uint64_t n = fs::file_size(p, ec);
    if (ec || n == 0 || n > (64u << 20)) return nullptr;
    std::vector<uint8_t> file((size_t)n);
    FILE* f = dod3::open_file(p, "rb");
    if (!f) return nullptr;
    const size_t got = fread(file.data(), 1, file.size(), f);
    fclose(f);
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (got != file.size() || !art_pixels(file, &rgba, &w, &h)) return nullptr;
    return art_texture(rgba, w, h);
}

/* ---- the kit ------------------------------------------------------------------ */

void Kit::begin_frame()
{
    frames++;
    ImGuiIO& io = ImGui::GetIO();
    W = io.DisplaySize.x;
    H = io.DisplaySize.y;
    s = H / 720.0f;
    dt = io.DeltaTime > 0 ? io.DeltaTime : 1.0f / 60;
    now = ImGui::GetTime();
    ImGuiStyle& st = ImGui::GetStyle();
    /* ImGui's own text (PushFont sizes, as in para) is scaled here, so its
     * sizes are design units too; draw-list text takes px(). The display's
     * scale is divided out: the window's height in pixels has it already. */
    st.FontScaleMain = s / (st.FontScaleDpi > 0 ? st.FontScaleDpi : 1.0f);
    st.FrameRounding = px(10);
    st.FramePadding = ImVec2(px(14), px(10));
    st.ItemSpacing = ImVec2(px(8), px(6));
    if (cooldown > 0) cooldown--;
}

bool Kit::hit(const char* id, const ImVec2& size) { return ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav); }

bool Kit::hot_item() { return ImGui::IsItemHovered() || (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible); }

void Kit::approach(float& v, float target, float speed) const { v += (target - v) * (1.0f - std::exp(-speed * dt)); }

ImVec2 Kit::text_size(ImFont* f, float size, const char* t, float wrap) const
{
    return f->CalcTextSizeA(px(size), FLT_MAX, wrap, t);
}

float Kit::tracked_width(ImFont* f, float size, const char* t, float track) const
{
    float w = 0;
    for (const char* c = t; *c; c++) w += f->CalcTextSizeA(px(size), FLT_MAX, 0, c, c + 1).x + (c[1] ? px(track) : 0);
    return w;
}

void Kit::tracked(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 col, const char* t, float track) const
{
    for (const char* c = t; *c; c++) {
        dl->AddText(f, px(size), p, col, c, c + 1);
        p.x += f->CalcTextSizeA(px(size), FLT_MAX, 0, c, c + 1).x + px(track);
    }
}

void Kit::para(ImFont* f, float size, ImU32 col, const char* t)
{
    ImGui::PushFont(f, size);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::PushTextWrapPos(wrap_right > 0 ? wrap_right - ImGui::GetWindowPos().x : 0.0f);
    ImGui::TextUnformatted(t);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Kit::path_line(ImFont* f, float size, ImU32 col, const char* prefix, const fs::path& p)
{
    const std::string s8 = dod3::utf8(p), pre = prefix;
    const float avail =
        (wrap_right > 0 ? wrap_right - ImGui::GetCursorScreenPos().x : ImGui::GetContentRegionAvail().x);
    auto width = [&](const std::string& t) { return text_size(f, size, t.c_str()).x; };
    std::string out = pre + s8;
    if (width(out) > avail) {
        auto lead = [&](size_t i) {
            while (i > 0 && i < s8.size() && ((unsigned char)s8[i] & 0xC0) == 0x80) i--;
            return i;
        };
        /* keep [0, head) and [tail, end), taking from the longer side */
        size_t head = s8.size() / 2, tail = s8.size() / 2;
        for (;;) {
            out = pre + s8.substr(0, lead(head)) + "..." + s8.substr(lead(tail));
            if (width(out) <= avail || (head == 0 && tail == s8.size())) break;
            if (head > 0 && head >= s8.size() - tail)
                head--;
            else
                tail++;
        }
    }
    ImGui::PushFont(f, size);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(out.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Kit::say(const std::string& t)
{
    toast = t;
    toast_at = now;
}

float Kit::button_width(const char* label, float w) const
{
    return w > 0 ? px(w) : text_size(F.semibold, 15, label).x + px(36);
}

bool Kit::button(const char* label, float w, ButtonKind kind, bool enabled)
{
    ImGui::PushID(label);
    const float h = px(40);
    const ImVec2 ts = text_size(F.semibold, 15, label);
    const ImVec2 sz(button_width(label, w), h);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool pressed = hit("##b", sz) && cooldown == 0;
    const bool hot = hot_item();
    const bool held = ImGui::IsItemActive();
    ImGui::EndDisabled();
    float& a = anim_of(ImGui::GetItemID());
    approach(a, hot && enabled ? 1.0f : 0.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float k = enabled ? 1.0f : 0.4f;
    ImU32 bg, fg = alpha(kText, k);
    if (kind == ButtonKind::Main)
        bg = alpha(mix(kAccent, kAccentHi, a + (held ? 0.4f : 0.0f)), k);
    else if (kind == ButtonKind::Danger)
        bg = alpha(kErr, 0.10f + 0.18f * a);
    else
        bg = IM_COL32(255, 255, 255, (int)((14 + 18 * a + (held ? 10 : 0)) * k));
    dl->AddRectFilled(p, p + sz, bg, px(10));
    if (kind == ButtonKind::Danger) {
        dl->AddRect(p, p + sz, alpha(kErr, 0.55f + 0.45f * a), px(10), 0, px(1.2f));
        fg = alpha(mix(kErr, kText, a), k);
    }
    dl->AddText(F.semibold, px(15), ImVec2(p.x + (sz.x - ts.x) / 2, p.y + (sz.y - ts.y) / 2), fg, label);
    ImGui::PopID();
    return pressed && enabled;
}

void Kit::dot(ImDrawList* dl, ImVec2 c, ImU32 col) const { dl->AddCircleFilled(c, px(4.5f), col); }

void Kit::ring(ImDrawList* dl, ImVec2 c, ImU32 col) const { dl->AddCircle(c, px(4.5f), col, 0, px(1.4f)); }

void Kit::tick(ImDrawList* dl, ImVec2 c, ImU32 col) const
{
    const float r = px(5.5f);
    const ImVec2 pts[3] = { ImVec2(c.x - r, c.y), ImVec2(c.x - r * 0.3f, c.y + r * 0.7f), ImVec2(c.x + r, c.y - r * 0.8f) };
    dl->AddPolyline(pts, 3, col, 0, px(2.0f));
}

void Kit::list_row(Mark m, ImU32 mark_col, const char* name, ImU32 name_col, const char* after, ImU32 after_col,
                   ImFont* font, float size)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c = ImGui::GetCursorScreenPos();
    ImGui::PushFont(font ? font : F.regular, size);
    const ImVec2 mc(c.x + px(6), c.y + ImGui::GetTextLineHeight() / 2);
    if (m == kTickMark)
        tick(dl, mc, mark_col);
    else if (m == kDotMark)
        dot(dl, mc, mark_col);
    else if (m == kRingMark)
        ring(dl, mc, mark_col);
    ImGui::SetCursorScreenPos(ImVec2(c.x + px(22), c.y));
    ImGui::PushStyleColor(ImGuiCol_Text, name_col);
    ImGui::TextUnformatted(name);
    ImGui::PopStyleColor();
    if (after && *after) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, after_col);
        ImGui::TextUnformatted(after);
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();
}

void Kit::label(ImFont* f, float size, ImU32 col, const char* t)
{
    ImGui::PushFont(f, size);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Kit::pill(ImDrawList* dl, ImVec2 right_top, const char* t, ImU32 col) const
{
    const ImVec2 ts = text_size(F.semibold, 12, t);
    const ImVec2 p0(right_top.x - ts.x - px(22), right_top.y), p1(right_top.x, right_top.y + ts.y + px(8));
    dl->AddRectFilled(p0, p1, alpha(col, 0.16f), px(20));
    dl->AddText(F.semibold, px(12), ImVec2(p0.x + px(11), p0.y + px(4)), col, t);
}

bool Kit::toggle(const char* id, bool on, bool enabled)
{
    const ImVec2 sz = toggle_size();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool pressed = hit(id, sz) && cooldown == 0;
    const bool hot = hot_item();
    ImGui::EndDisabled();
    const ImGuiID item = ImGui::GetItemID();
    float& a = anim_of(item);                 /* under the mouse or focused */
    float& k = anim_of(item ^ 0x5A5A5A5Au);   /* the knob: 0 off, 1 on */
    approach(a, hot && enabled ? 1.0f : 0.0f);
    approach(k, on ? 1.0f : 0.0f, 18.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fade = enabled ? 1.0f : 0.4f;
    const ImU32 track = mix(IM_COL32(255, 255, 255, (int)(30 + 18 * a)), mix(kAccent, kAccentHi, a), k);
    dl->AddRectFilled(p, p + sz, alpha(track, fade), sz.y / 2);
    if (hot && enabled) dl->AddRect(p, p + sz, IM_COL32(255, 255, 255, 40), sz.y / 2, 0, px(1.2f));
    const float r = px(9), x = p.x + sz.y / 2 + (sz.x - sz.y) * k;
    dl->AddCircleFilled(ImVec2(x, p.y + sz.y / 2), r, alpha(IM_COL32(255, 255, 255, 255), fade));
    return pressed && enabled;
}

void Kit::card_begin()
{
    card_x0 = ImGui::GetCursorScreenPos().x;
    card_x1 = card_x0 + ImGui::GetContentRegionAvail().x - px(4);
    wrap_right = card_x1 - px(18);
    ImGui::GetWindowDrawList()->ChannelsSplit(2);
    ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, px(14)));
    ImGui::Indent(px(18));
}

void Kit::card_end()
{
    ImGui::Unindent(px(18));
    ImGui::Dummy(ImVec2(0, px(10)));
    ImGui::EndGroup();
    wrap_right = 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(card_x0, ImGui::GetItemRectMin().y), b(card_x1, ImGui::GetItemRectMax().y);
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(a, b, kCard, px(14));
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 14), px(14));
    dl->ChannelsMerge();
    ImGui::Dummy(ImVec2(0, px(12)));
}

void Kit::progress(float f)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos(), sz(ImGui::GetContentRegionAvail().x, px(8));
    ImGui::Dummy(sz);
    dl->AddRectFilled(p, p + sz, IM_COL32(255, 255, 255, 18), px(4));
    dl->AddRectFilled(p, ImVec2(p.x + sz.x * std::clamp(f, 0.0f, 1.0f), p.y + sz.y), kAccentHi, px(4));
}

void Kit::install_progress(const std::string& what, uint64_t done, uint64_t total, const std::string& more)
{
    label(F.semibold, 19, kText, what.empty() ? "Starting..." : what.c_str());
    ImGui::Dummy(ImVec2(0, px(14)));
    progress(total ? (float)((double)done / (double)total) : 0.0f);
    ImGui::Dummy(ImVec2(0, px(8)));
    if (!what.empty()) para(F.regular, 13, kDim, (size_text(done) + " of " + size_text(total) + more).c_str());
}

void Kit::checking(size_t files)
{
    if (!files) return;
    static const char* const dots[] = { "", ".", "..", "..." };
    char t[64];
    snprintf(t, sizeof t, "Checking %zu file%s%s", files, files == 1 ? "" : "s", dots[(int)(now * 3) % 4]);
    para(F.regular, 14, kDim, t);
}

bool Kit::confirm_pressed()
{
    return ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
           ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
}

bool Kit::back_pressed()
{
    return ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
}

void Kit::begin_window(const char* id)
{
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoScrollWithMouse);
}

void Kit::grab_step(const std::string& prefix, int frame, int req_at, int save_at, int index)
{
    if (prefix.empty()) return;
    if (frame == req_at)
        if (FILE* f = fopen((prefix + ".req").c_str(), "wb")) fclose(f);
    if (frame == save_at) {
        const std::string to = prefix + "_" + std::to_string(index) + ".ppm";
        remove(to.c_str());
        rename((prefix + ".ppm").c_str(), to.c_str());
    }
}

void Kit::toast_draw()
{
    const float t = (float)(now - toast_at);
    if (toast.empty() || t > 2.2f) return;
    const float a = std::min(1.0f, t / 0.15f) * std::min(1.0f, (2.2f - t) / 0.4f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 ts = text_size(F.semibold, 14, toast.c_str());
    const ImVec2 c(W / 2, H - px(40) - px(6) * (1 - a));
    const ImVec2 p0(c.x - ts.x / 2 - px(18), c.y - ts.y / 2 - px(9)), p1(c.x + ts.x / 2 + px(18), c.y + ts.y / 2 + px(9));
    dl->AddRectFilled(p0, p1, IM_COL32(24, 24, 30, (int)(240 * a)), px(30));
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, (int)(26 * a)), px(30));
    dl->AddText(F.semibold, px(14), ImVec2(p0.x + px(18), p0.y + px(9)), alpha(kText, a), toast.c_str());
}

void Kit::backdrop(ImTextureData* art, float art_a, float shade)
{
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(9, 9, 11, 255));
    if (!art || art_a < 1) {
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), IM_COL32(28, 10, 14, 255), IM_COL32(12, 10, 12, 255),
                                    IM_COL32(9, 9, 11, 255), IM_COL32(16, 9, 11, 255));
        /* an ember's glow, drifting */
        const ImVec2 c(W * (0.74f + 0.02f * (float)std::sin(now * 0.07)), H * (0.32f + 0.02f * (float)std::cos(now * 0.05)));
        for (int i = 0; i < 18; i++)
            dl->AddCircleFilled(c, H * (0.95f - 0.045f * (float)i), IM_COL32(150, 24, 34, 4), 96);
    }
    if (art && art_a > 0) {
        /* cover the window, drifting slowly */
        const float aw = (float)art->Width, ah = (float)art->Height;
        const float k = std::max(W / aw, H / ah) * (1.05f + 0.02f * (float)std::sin(now * 0.05));
        const float dw = aw * k, dh = ah * k;
        const float ox = (W - dw) / 2 + (float)std::sin(now * 0.031) * px(10), oy = (H - dh) / 2;
        dl->AddImage(art->GetTexRef(), ImVec2(ox, oy), ImVec2(ox + dw, oy + dh), ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(255, 255, 255, (int)(255 * std::clamp(art_a, 0.0f, 1.0f))));
    }
    /* the left darkened for the menu, the bottom for the footer */
    const ImU32 a = IM_COL32(8, 8, 10, 236), z = IM_COL32(8, 8, 10, 0);
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W * 0.62f, H), a, z, z, a);
    dl->AddRectFilledMultiColor(ImVec2(0, H * 0.62f), ImVec2(W, H), z, z, IM_COL32(8, 8, 10, 210),
                                IM_COL32(8, 8, 10, 210));
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H * 0.18f), IM_COL32(8, 8, 10, 120), IM_COL32(8, 8, 10, 120), z,
                                z);
    if (shade > 0) dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(6, 6, 8, (int)(110 * shade)));
}

void Kit::title()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p(px(72), px(70));
    tracked(dl, F.display, 50, p, kText, "DRAKENGARD", 2.5f);
    const float w = tracked_width(F.display, 50, "DRAKENGARD", 2.5f);
    tracked(dl, F.display, 50, ImVec2(p.x + w + px(16), p.y), kAccentHi, "3", 0);
    const float y = p.y + px(66);
    dl->AddRectFilled(ImVec2(p.x, y + px(7)), ImVec2(p.x + px(34), y + px(9)), kAccent);
    tracked(dl, F.semibold, 13, ImVec2(p.x + px(48), y), kDim, "RECOMPILED", 4.0f);
}

bool Kit::panel_frame(const char* heading, const char* sub, float e, bool closable, ImVec2* body, ImVec2* body_size)
{
    const float x0 = W * 0.385f + px(36) * (1 - e), x1 = W - px(56) + px(36) * (1 - e);
    const float y0 = px(56), y1 = H - px(56);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x0 + px(2), y0 + px(10)), ImVec2(x1 + px(2), y1 + px(14)), IM_COL32(0, 0, 0, (int)(90 * e)),
                      px(20));
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), alpha(kPanel, e), px(18));
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), alpha(kLine, e), px(18));
    dl->AddText(F.display, px(30), ImVec2(x0 + px(36), y0 + px(28)), alpha(kText, e), heading);
    if (sub) dl->AddText(F.regular, px(14), ImVec2(x0 + px(36), y0 + px(68)), alpha(kDim, e), sub);
    bool close = false;
    if (closable) {
        const float cs = px(34);
        const ImVec2 cp(x1 - px(24) - cs, y0 + px(26));
        ImGui::SetCursorScreenPos(cp);
        close = hit("##close", ImVec2(cs, cs)) && cooldown == 0;
        float& a = anim_of(ImGui::GetItemID());
        approach(a, hot_item() ? 1.0f : 0.0f);
        dl->AddCircleFilled(cp + ImVec2(cs / 2, cs / 2), cs / 2, IM_COL32(255, 255, 255, (int)(10 + 22 * a)));
        const float r = px(6);
        const ImVec2 c = cp + ImVec2(cs / 2, cs / 2);
        dl->AddLine(c - ImVec2(r, r), c + ImVec2(r, r), alpha(kText, 0.6f + 0.4f * a), px(1.6f));
        dl->AddLine(c + ImVec2(-r, r), c + ImVec2(r, -r), alpha(kText, 0.6f + 0.4f * a), px(1.6f));
    }
    *body = ImVec2(x0 + px(36), y0 + (sub ? px(104) : px(84)));
    *body_size = ImVec2(x1 - x0 - px(72), y1 - body->y - px(28));
    return close;
}

}  // namespace look
}  // namespace dod3setup
