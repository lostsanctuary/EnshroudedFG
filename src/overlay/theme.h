#pragma once

#include <imgui.h>

// Enshrouded-style look for the overlay: deep indigo panels, gold hairlines and diamond ornaments,
// a Palatino-like serif for text and spaced small caps for titles, "< value >" steppers like the game's
// settings rows, white keycap hints, and a compact counter like the game's own FPS display.
// Fonts are loaded at runtime from C:\Windows\Fonts (nothing is bundled); fallbacks keep it usable.
namespace efg::overlay::theme {

// Palette (sampled from the game's menus).
namespace col {
constexpr ImU32 kPanelTop = IM_COL32(46, 49, 87, 242);
constexpr ImU32 kPanelBottom = IM_COL32(30, 32, 56, 242);
constexpr ImU32 kRow = IM_COL32(52, 56, 94, 230);
constexpr ImU32 kRowAlt = IM_COL32(47, 51, 87, 230);
constexpr ImU32 kRowHover = IM_COL32(63, 68, 112, 240);
constexpr ImU32 kRowSeparator = IM_COL32(26, 28, 48, 255);
constexpr ImU32 kGold = IM_COL32(226, 194, 124, 255);
constexpr ImU32 kGoldDim = IM_COL32(168, 138, 78, 255);
constexpr ImU32 kGoldGlow = IM_COL32(242, 196, 109, 60);
constexpr ImU32 kParchment = IM_COL32(217, 200, 162, 255);
constexpr ImU32 kText = IM_COL32(236, 231, 218, 255);
constexpr ImU32 kTextDim = IM_COL32(168, 164, 190, 255);
constexpr ImU32 kTextMuted = IM_COL32(126, 123, 152, 255);
constexpr ImU32 kAccent = IM_COL32(242, 176, 74, 255);
constexpr ImU32 kRed = IM_COL32(232, 87, 63, 255);
constexpr ImU32 kGreen = IM_COL32(158, 220, 134, 255);
constexpr ImU32 kKeycap = IM_COL32(241, 240, 236, 255);
constexpr ImU32 kKeycapText = IM_COL32(29, 30, 44, 255);
constexpr ImU32 kCounterBg = IM_COL32(18, 19, 34, 150);
constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 170);
}  // namespace col

struct Fonts {
    ImFont* serif = nullptr;      // body text
    ImFont* serifBold = nullptr;  // values, section headers, titles
    ImFont* serifItalic = nullptr;
    ImFont* counter = nullptr;    // compact counter (condensed sans)
};
const Fonts& GetFonts();

// Base font sizes (at 1080p; scaled with the screen height).
constexpr float kBody = 19.0f;
constexpr float kSmall = 15.0f;
constexpr float kSection = 25.0f;
constexpr float kTitle = 18.0f;
constexpr float kStat = 32.0f;
constexpr float kCounter = 16.0f;

// Creates the fonts (call once after ImGui::CreateContext) and applies colours / metrics.
void Init();
// Re-applies metrics when the output height changes (call every frame before NewFrame).
void UpdateScale(float displayHeight);
float Scale();
inline float S(float px) { return px * Scale(); }

// ---- drawing helpers ---------------------------------------------------------------------------

void Diamond(ImDrawList* dl, ImVec2 center, float radius, ImU32 color);
// Thin gold line fading out at both ends, optional diamond in the middle.
void GoldRule(ImDrawList* dl, float x0, float x1, float y, bool centerDiamond);
// Panel background with gradient, gold frame and corner ornaments.
void PanelChrome(ImDrawList* dl, ImVec2 min, ImVec2 max);
// Text with an extra gap between glyphs (the game's spaced capitals).
void SpacedText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 color, const char* text, float spacing);
float SpacedTextWidth(ImFont* font, float size, const char* text, float spacing);

// ---- widgets (inside a themed window) ----------------------------------------------------------

// Header like the game's tab bar: spaced capitals, gold rule, diamond under the active title.
void TitleBar(const char* title, const char* rightText);
// Serif section header ("Output", "Quality" in the game).
void SectionHeader(const char* text);
// Settings row with "< value >" stepper on the right. Returns -1 / +1 when an arrow (or the value,
// which steps forward) is clicked, 0 otherwise. `rowIndex` alternates the row shade.
int StepperRow(const char* id, const char* label, const char* value, bool canPrev, bool canNext, int rowIndex,
               const char* tooltip, bool enabled = true);
// Settings row that opens a dropdown, like the game's "Resolution  1920 x 1080  v" rows. Returns true
// while its list is open: then add items with DropdownCheck() and close with EndDropdown().
bool DropdownRow(const char* id, const char* label, const char* preview, int rowIndex, const char* tooltip,
                 bool enabled = true);
// A check item in an open dropdown: gold diamond box + label. Returns true when toggled.
bool DropdownCheck(const char* label, bool* checked, bool canToggle = true);
void EndDropdown();
// Big number with a small spaced caption above it (the game's character stats).
void Stat(const char* caption, const char* value, ImU32 valueColor, float width);
// Status line led by a coloured diamond.
void StatusLine(ImU32 color, const char* text);
// White keycap with dark letters followed by a label ("ESC Close"); returns true when clicked.
bool KeyHint(const char* key, const char* label);

}  // namespace efg::overlay::theme
