#include "overlay/theme.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <string>

#include "core/log.h"

namespace efg::overlay::theme {
namespace {

Fonts g_fonts;
float g_scale = 1.0f;
ImGuiStyle g_baseStyle;  // unscaled style, re-scaled when the output height changes

std::string FontsDir() {
    char dir[MAX_PATH]{};
    UINT n = GetWindowsDirectoryA(dir, MAX_PATH);
    return std::string(dir, n) + "\\Fonts\\";
}

ImFont* LoadFirst(std::initializer_list<const char*> files, const char* role) {
    ImGuiIO& io = ImGui::GetIO();
    const std::string dir = FontsDir();
    for (const char* f : files) {
        std::string path = dir + f;
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), kBody, &cfg)) {
            EFG_LOG("overlay theme: %s font %s", role, f);
            return font;
        }
    }
    EFG_LOG("overlay theme: no %s font found, using the default font", role);
    return nullptr;
}

ImVec4 V(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

void ApplyColors(ImGuiStyle& s) {
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = V(col::kText);
    c[ImGuiCol_TextDisabled] = V(col::kTextMuted);
    c[ImGuiCol_WindowBg] = V(col::kPanelBottom);
    c[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.12f, 0.21f, 0.97f);
    c[ImGuiCol_Border] = V(col::kGoldDim);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = V(col::kRow);
    c[ImGuiCol_FrameBgHovered] = V(col::kRowHover);
    c[ImGuiCol_FrameBgActive] = V(col::kRowHover);
    c[ImGuiCol_Button] = V(col::kRow);
    c[ImGuiCol_ButtonHovered] = V(col::kRowHover);
    c[ImGuiCol_ButtonActive] = V(col::kGoldDim);
    c[ImGuiCol_CheckMark] = V(col::kGold);
    c[ImGuiCol_SliderGrab] = V(col::kText);
    c[ImGuiCol_SliderGrabActive] = V(col::kGold);
    c[ImGuiCol_Header] = V(col::kRow);
    c[ImGuiCol_HeaderHovered] = V(col::kRowHover);
    c[ImGuiCol_HeaderActive] = V(col::kRowHover);
    c[ImGuiCol_Separator] = V(col::kGoldDim);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = V(col::kGoldDim);
    c[ImGuiCol_ScrollbarGrabHovered] = V(col::kGold);
    c[ImGuiCol_ScrollbarGrabActive] = V(col::kGold);
    c[ImGuiCol_NavCursor] = V(col::kGold);
}

}  // namespace

const Fonts& GetFonts() { return g_fonts; }
float Scale() { return g_scale; }

void Init() {
    // Palatino Linotype is the closest Windows serif to the game's typeface.
    g_fonts.serif = LoadFirst({"pala.ttf", "georgia.ttf", "constan.ttf"}, "serif");
    g_fonts.serifBold = LoadFirst({"palab.ttf", "georgiab.ttf", "constanb.ttf"}, "serif bold");
    g_fonts.serifItalic = LoadFirst({"palai.ttf", "georgiai.ttf", "constani.ttf"}, "serif italic");
    g_fonts.counter = LoadFirst({"bahnschrift.ttf", "seguisb.ttf", "segoeui.ttf"}, "counter");
    if (!g_fonts.serifBold) g_fonts.serifBold = g_fonts.serif;
    if (!g_fonts.serifItalic) g_fonts.serifItalic = g_fonts.serif;
    if (!g_fonts.counter) g_fonts.counter = g_fonts.serifBold;
    if (g_fonts.serif) ImGui::GetIO().FontDefault = g_fonts.serif;

    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.FontSizeBase = kBody;
    s.WindowPadding = ImVec2(0, 0);
    s.WindowRounding = 0.0f;
    s.WindowBorderSize = 0.0f;
    s.PopupRounding = 2.0f;
    s.PopupBorderSize = 1.0f;
    s.FramePadding = ImVec2(10, 6);
    s.FrameRounding = 2.0f;
    s.ItemSpacing = ImVec2(10, 8);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.ScrollbarSize = 8.0f;
    s.ScrollbarRounding = 0.0f;
    s.GrabRounding = 0.0f;
    ApplyColors(s);
    g_baseStyle = s;
    g_scale = 1.0f;
}

void UpdateScale(float displayHeight) {
    float scale = displayHeight > 0 ? displayHeight / 1080.0f : 1.0f;
    if (scale < 0.75f) scale = 0.75f;
    if (scale > 3.0f) scale = 3.0f;
    if (std::fabs(scale - g_scale) < 0.01f) return;
    g_scale = scale;
    ImGuiStyle& s = ImGui::GetStyle();
    s = g_baseStyle;
    s.ScaleAllSizes(scale);
    s.FontScaleMain = scale;
}

// ---- drawing helpers ---------------------------------------------------------------------------

void Diamond(ImDrawList* dl, ImVec2 c, float r, ImU32 color) {
    dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), color);
}

void GoldRule(ImDrawList* dl, float x0, float x1, float y, bool centerDiamond) {
    const float fade = (x1 - x0) * 0.18f;
    const ImU32 clear = col::kGold & 0x00FFFFFF;
    const float t = S(1.0f);
    dl->AddRectFilledMultiColor(ImVec2(x0, y), ImVec2(x0 + fade, y + t), clear, col::kGold, col::kGold, clear);
    dl->AddRectFilled(ImVec2(x0 + fade, y), ImVec2(x1 - fade, y + t), col::kGold);
    dl->AddRectFilledMultiColor(ImVec2(x1 - fade, y), ImVec2(x1, y + t), col::kGold, clear, clear, col::kGold);
    if (centerDiamond) {
        const ImVec2 c((x0 + x1) * 0.5f, y + t * 0.5f);
        Diamond(dl, c, S(5.0f), col::kGold);
        Diamond(dl, c, S(2.5f), col::kPanelBottom | 0xFF000000);
    }
}

void PanelChrome(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    // Soft drop shadow so the panel separates from bright scenes, like the game's menus.
    for (int i = 1; i <= 6; ++i) {
        const float o = S(3.0f * i);
        dl->AddRectFilled(ImVec2(a.x - o, a.y - o + S(4)), ImVec2(b.x + o, b.y + o + S(4)),
                          IM_COL32(0, 0, 0, 34 - 5 * i), o);
    }
    dl->AddRectFilledMultiColor(a, b, col::kPanelTop, col::kPanelTop, col::kPanelBottom, col::kPanelBottom);
    // Outer hairline and an inset gold frame, like the game's item panels.
    dl->AddRect(a, b, IM_COL32(12, 13, 24, 200), 0.0f, 0, S(1.0f));
    const float in = S(6.0f);
    const ImVec2 ia(a.x + in, a.y + in), ib(b.x - in, b.y - in);
    dl->AddRect(ia, ib, col::kGoldDim, 0.0f, 0, S(1.0f));
    // Corner ornaments: a gold diamond with a short flourish along each edge.
    const ImVec2 corners[4] = {ia, ImVec2(ib.x, ia.y), ib, ImVec2(ia.x, ib.y)};
    for (int i = 0; i < 4; ++i) {
        const ImVec2 c = corners[i];
        const float sx = (i == 1 || i == 2) ? -1.0f : 1.0f;
        const float sy = (i >= 2) ? -1.0f : 1.0f;
        dl->AddLine(ImVec2(c.x, c.y + sy * S(1)), ImVec2(c.x + sx * S(28), c.y + sy * S(1)), col::kGold, S(1.5f));
        dl->AddLine(ImVec2(c.x + sx * S(1), c.y), ImVec2(c.x + sx * S(1), c.y + sy * S(28)), col::kGold, S(1.5f));
        Diamond(dl, c, S(6.0f), col::kGold);
        Diamond(dl, c, S(3.0f), col::kPanelTop | 0xFF000000);
    }
}

float SpacedTextWidth(ImFont* font, float size, const char* text, float spacing) {
    float w = 0;
    const size_t n = strlen(text);
    for (size_t i = 0; i < n; ++i) {
        w += font->CalcTextSizeA(size, FLT_MAX, 0, text + i, text + i + 1).x;
        if (i + 1 < n) w += spacing;
    }
    return w;
}

void SpacedText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 color, const char* text, float spacing) {
    for (const char* p = text; *p; ++p) {
        dl->AddText(font, size, pos, color, p, p + 1);
        pos.x += font->CalcTextSizeA(size, FLT_MAX, 0, p, p + 1).x + spacing;
    }
}

// ---- widgets ------------------------------------------------------------------------------------

void TitleBar(const char* title, const char* rightText) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth();
    ImFont* bold = g_fonts.serifBold ? g_fonts.serifBold : ImGui::GetFont();
    const float size = S(kTitle * 1.15f);
    const float spacing = S(2.2f);
    const float tw = SpacedTextWidth(bold, size, title, spacing);
    const float top = wp.y + S(26);
    SpacedText(dl, bold, size, ImVec2(wp.x + (w - tw) * 0.5f, top), col::kText, title, spacing);
    if (rightText) {
        ImFont* f = g_fonts.serif ? g_fonts.serif : ImGui::GetFont();
        const float rs = S(kSmall);
        const float rw = f->CalcTextSizeA(rs, FLT_MAX, 0, rightText).x;
        dl->AddText(f, rs, ImVec2(wp.x + w - S(30) - rw, top + (size - rs) * 0.5f), col::kTextMuted, rightText);
    }
    const float ruleY = top + size + S(12);
    GoldRule(dl, wp.x + S(18), wp.x + w - S(18), ruleY, false);
    // Active-tab marker under the title, like the game's gold diamond.
    Diamond(dl, ImVec2(wp.x + w * 0.5f, ruleY + S(0.5f)), S(5.5f), col::kGold);
    ImGui::SetCursorScreenPos(ImVec2(wp.x, ruleY + S(18)));
}

void SectionHeader(const char* text) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(p.x + S(28), p.y + S(6)));
    ImGui::PushFont(g_fonts.serif, kSection);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col::kText));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p.x, ImGui::GetCursorScreenPos().y + S(2)));
}

namespace {
void Chevron(ImDrawList* dl, ImVec2 c, float h, bool left, ImU32 color) {
    const float w = h * 0.55f;
    const float dir = left ? -1.0f : 1.0f;
    const ImVec2 pts[3] = {ImVec2(c.x - dir * w * 0.5f, c.y - h * 0.5f), ImVec2(c.x + dir * w * 0.5f, c.y),
                           ImVec2(c.x - dir * w * 0.5f, c.y + h * 0.5f)};
    dl->AddPolyline(pts, 3, color, 0, S(2.2f));
}
}  // namespace

int StepperRow(const char* id, const char* label, const char* value, bool canPrev, bool canNext, int rowIndex,
               const char* tooltip, bool enabled) {
    ImGui::PushID(id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x0 = ImGui::GetWindowPos().x + S(24);
    const float x1 = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - S(24);
    const float h = S(52);
    const ImVec2 a(x0, ImGui::GetCursorScreenPos().y);
    const ImVec2 b(x1, a.y + h);

    // Row hover is a plain rectangle test: an item covering the row would take the hover (and the
    // clicks) away from the arrow / value buttons placed on top of it.
    const bool rowHovered = enabled && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a, b);
    dl->AddRectFilled(a, b, rowHovered ? col::kRowHover : (rowIndex % 2 ? col::kRowAlt : col::kRow));
    dl->AddLine(ImVec2(a.x, b.y - S(1)), ImVec2(b.x, b.y - S(1)), col::kRowSeparator, S(2));
    if (rowHovered) dl->AddRectFilled(a, ImVec2(a.x + S(3), b.y), col::kGold);  // selection accent

    ImFont* serif = g_fonts.serif ? g_fonts.serif : ImGui::GetFont();
    ImFont* bold = g_fonts.serifBold ? g_fonts.serifBold : ImGui::GetFont();
    const float ts = S(kBody * 1.05f);
    dl->AddText(serif, ts, ImVec2(a.x + S(26), a.y + (h - ts) * 0.5f), enabled ? col::kText : col::kTextMuted, label);

    // Stepper: "<   value   >" in the right half, like the game's settings rows.
    const float sx0 = a.x + (x1 - x0) * 0.50f;
    const float sx1 = b.x - S(30);
    const float cy = a.y + h * 0.5f;
    int delta = 0;
    const ImVec2 hit(S(44), h);
    ImGui::SetCursorScreenPos(ImVec2(sx0 - S(10), a.y));
    ImGui::InvisibleButton("prev", hit);
    const bool prevHover = enabled && canPrev && ImGui::IsItemHovered();
    if (enabled && canPrev && ImGui::IsItemClicked()) delta = -1;
    ImGui::SetCursorScreenPos(ImVec2(sx1 - hit.x + S(10), a.y));
    ImGui::InvisibleButton("next", hit);
    const bool nextHover = enabled && canNext && ImGui::IsItemHovered();
    if (enabled && canNext && ImGui::IsItemClicked()) delta = +1;
    ImGui::SetCursorScreenPos(ImVec2(sx0 + hit.x - S(10), a.y));
    ImGui::InvisibleButton("value", ImVec2((sx1 - hit.x + S(10)) - (sx0 + hit.x - S(10)), h));
    const bool valueHover = enabled && ImGui::IsItemHovered();
    if (enabled && ImGui::IsItemClicked()) delta = canNext ? +1 : (canPrev ? -1 : 0);

    auto arrowColor = [&](bool can, bool hover) {
        if (!enabled || !can) return IM_COL32(126, 123, 152, 90);
        return hover ? col::kGold : col::kText;
    };
    Chevron(dl, ImVec2(sx0 + S(12), cy), S(16), true, arrowColor(canPrev, prevHover));
    Chevron(dl, ImVec2(sx1 - S(12), cy), S(16), false, arrowColor(canNext, nextHover));
    const float vs = S(kBody * 1.08f);
    const float vw = bold->CalcTextSizeA(vs, FLT_MAX, 0, value).x;
    const ImU32 vc = !enabled ? col::kTextMuted : (valueHover ? col::kGold : col::kText);
    dl->AddText(bold, vs, ImVec2((sx0 + sx1 - vw) * 0.5f, cy - vs * 0.5f), vc, value);

    if (tooltip && (rowHovered || prevHover || nextHover || valueHover)) ImGui::SetTooltip("%s", tooltip);
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x, b.y));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
    return delta;
}

bool DropdownRow(const char* id, const char* label, const char* preview, int rowIndex, const char* tooltip,
                 bool enabled) {
    ImGui::PushID(id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x0 = ImGui::GetWindowPos().x + S(24);
    const float x1 = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - S(24);
    const float h = S(52);
    const ImVec2 a(x0, ImGui::GetCursorScreenPos().y);
    const ImVec2 b(x1, a.y + h);
    const bool open = ImGui::IsPopupOpen("list");

    const bool rowHovered = enabled && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a, b);
    dl->AddRectFilled(a, b, rowHovered || open ? col::kRowHover : (rowIndex % 2 ? col::kRowAlt : col::kRow));
    dl->AddLine(ImVec2(a.x, b.y - S(1)), ImVec2(b.x, b.y - S(1)), col::kRowSeparator, S(2));
    if (rowHovered || open) dl->AddRectFilled(a, ImVec2(a.x + S(3), b.y), col::kGold);

    ImFont* serif = g_fonts.serif ? g_fonts.serif : ImGui::GetFont();
    ImFont* bold = g_fonts.serifBold ? g_fonts.serifBold : ImGui::GetFont();
    const float ts = S(kBody * 1.05f);
    dl->AddText(serif, ts, ImVec2(a.x + S(26), a.y + (h - ts) * 0.5f), enabled ? col::kText : col::kTextMuted, label);

    // Value preview centred in the right half, chevron at the right edge (up while open).
    const float sx0 = a.x + (x1 - x0) * 0.50f;
    const float sx1 = b.x - S(30);
    const float cy = a.y + h * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(sx0, a.y));
    ImGui::InvisibleButton("value", ImVec2(b.x - sx0, h));
    const bool valueHover = enabled && ImGui::IsItemHovered();
    if (enabled && ImGui::IsItemClicked()) ImGui::OpenPopup("list");
    const float vs = S(kBody * 1.0f);
    const float vw = bold->CalcTextSizeA(vs, FLT_MAX, 0, preview).x;
    const ImU32 vc = !enabled ? col::kTextMuted : (valueHover || open ? col::kGold : col::kText);
    dl->AddText(bold, vs, ImVec2((sx0 + sx1 - vw) * 0.5f, cy - vs * 0.5f), vc, preview);
    {
        const float cw = S(8), ch = S(4.5f);
        const float dir = open ? -1.0f : 1.0f;
        const ImVec2 pts[3] = {ImVec2(sx1 + S(10) - cw, cy - dir * ch), ImVec2(sx1 + S(10), cy + dir * ch),
                               ImVec2(sx1 + S(10) + cw, cy - dir * ch)};
        dl->AddPolyline(pts, 3, enabled ? (valueHover || open ? col::kGold : col::kText) : col::kTextMuted, 0, S(2.2f));
    }
    if (tooltip && rowHovered && !open) ImGui::SetTooltip("%s", tooltip);
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x, b.y));
    ImGui::Dummy(ImVec2(0, 0));

    // The list: an indigo plate with a gold frame, directly under the value.
    ImGui::SetNextWindowPos(ImVec2(sx0, b.y - S(2)));
    ImGui::SetNextWindowSize(ImVec2(b.x - sx0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, S(1));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(2)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(col::kPanelBottom | 0xFF000000));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(col::kGold));
    const bool listOpen = ImGui::BeginPopup("list", ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
    if (!listOpen) ImGui::PopID();
    return listOpen;
}

bool DropdownCheck(const char* label, bool* checked, bool canToggle) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = S(38);
    ImGui::InvisibleButton(label, ImVec2(w, h));
    const bool hover = ImGui::IsItemHovered();
    bool changed = false;
    if (canToggle && ImGui::IsItemClicked()) {
        *checked = !*checked;
        changed = true;
    }
    if (hover) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col::kRowHover);
    // Diamond check box: gold outline, filled when checked.
    const ImVec2 c(p.x + S(20), p.y + h * 0.5f);
    const float r = S(7.5f);
    const ImU32 boxCol = canToggle ? col::kGold : col::kGoldDim;
    dl->AddQuad(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), boxCol, S(1.5f));
    if (*checked) Diamond(dl, c, r * 0.55f, boxCol);
    ImFont* serif = g_fonts.serif ? g_fonts.serif : ImGui::GetFont();
    const float ts = S(kBody);
    const ImU32 tc = !canToggle ? col::kTextDim : (hover ? col::kGold : (*checked ? col::kText : col::kTextDim));
    dl->AddText(serif, ts, ImVec2(p.x + S(40), p.y + (h - ts) * 0.5f), tc, label);
    if (!canToggle && hover) ImGui::SetTooltip("At least one item stays selected.");
    return changed;
}

void EndDropdown() {
    ImGui::EndPopup();
    ImGui::PopID();
}

void Stat(const char* caption, const char* value, ImU32 valueColor, float width) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImFont* serif = g_fonts.serif ? g_fonts.serif : ImGui::GetFont();
    ImFont* bold = g_fonts.serifBold ? g_fonts.serifBold : ImGui::GetFont();
    const float cs = S(kSmall * 0.95f);
    const float spacing = S(1.6f);
    const float cw = SpacedTextWidth(serif, cs, caption, spacing);
    SpacedText(dl, serif, cs, ImVec2(p.x + (width - cw) * 0.5f, p.y), col::kTextDim, caption, spacing);
    const float vs = S(kStat);
    const float vw = bold->CalcTextSizeA(vs, FLT_MAX, 0, value).x;
    dl->AddText(bold, vs, ImVec2(p.x + (width - vw) * 0.5f, p.y + cs + S(4)), valueColor, value);
    ImGui::Dummy(ImVec2(width, cs + S(4) + vs));
}

void StatusLine(ImU32 color, const char* text) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float fs = ImGui::GetFontSize();
    Diamond(dl, ImVec2(p.x + S(34), p.y + fs * 0.55f), S(5.0f), color);
    ImGui::SetCursorScreenPos(ImVec2(p.x + S(48), p.y));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));
    ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - S(34));  // window-local x
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

bool KeyHint(const char* key, const char* label) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImFont* bold = g_fonts.counter ? g_fonts.counter : ImGui::GetFont();
    ImFont* serif = g_fonts.serif ? g_fonts.serif : ImGui::GetFont();
    const float ks = S(kSmall * 0.95f);
    const ImVec2 kt = bold->CalcTextSizeA(ks, FLT_MAX, 0, key);
    const float kh = S(24);
    const float kw = kt.x + S(12) < kh ? kh : kt.x + S(12);
    const float ls = S(kBody * 1.05f);
    const float lw = serif->CalcTextSizeA(ls, FLT_MAX, 0, label).x;
    ImGui::InvisibleButton(key, ImVec2(kw + S(8) + lw, kh));
    const bool hover = ImGui::IsItemHovered();
    dl->AddRectFilled(p, ImVec2(p.x + kw, p.y + kh), col::kKeycap, S(3));
    dl->AddText(bold, ks, ImVec2(p.x + (kw - kt.x) * 0.5f, p.y + (kh - kt.y) * 0.5f), col::kKeycapText, key);
    dl->AddText(serif, ls, ImVec2(p.x + kw + S(8), p.y + (kh - ls) * 0.5f), hover ? col::kGold : col::kText, label);
    return ImGui::IsItemClicked();
}

}  // namespace efg::overlay::theme
