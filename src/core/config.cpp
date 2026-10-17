#include "core/config.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <string>

#include "core/log.h"

namespace efg {
namespace {

const std::wstring& IniPath() {
    static const std::wstring path = GameDir() + L"EnshroudedFG.ini";
    return path;
}

std::wstring ReadString(const wchar_t* section, const wchar_t* key) {
    wchar_t buf[128]{};
    GetPrivateProfileStringW(section, key, L"", buf, 128, IniPath().c_str());
    return buf;
}

void Write(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(section, key, value.c_str(), IniPath().c_str());
}

// Writes `def` if the key is missing, so every option shows up in the ini.
std::wstring ReadOrDefault(const wchar_t* section, const wchar_t* key, const wchar_t* def) {
    std::wstring v = ReadString(section, key);
    if (v.empty()) {
        Write(section, key, def);
        v = def;
    }
    return v;
}

int ToInt(const std::wstring& s, int def) {
    wchar_t* end = nullptr;
    long v = wcstol(s.c_str(), &end, 0);
    return end != s.c_str() ? static_cast<int>(v) : def;
}

float ToFloat(const std::wstring& s, float def) {
    wchar_t* end = nullptr;
    float v = wcstof(s.c_str(), &end);
    return end != s.c_str() ? v : def;
}

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

std::wstring CapToString(int cap) { return cap < 0 ? L"auto" : (cap == 0 ? L"off" : std::to_wstring(cap)); }

int ParseCap(const std::wstring& s) {
    if (_wcsicmp(s.c_str(), L"auto") == 0) return -1;
    if (_wcsicmp(s.c_str(), L"off") == 0) return 0;
    int v = ToInt(s, -1);
    return v <= 0 ? 0 : Clamp(v, 30, 2000);
}

float ClampKeep(float k) { return k < 0.85f ? 0.85f : (k > 1.0f ? 1.0f : k); }

std::wstring KeepToString(float k) {
    wchar_t buf[16];
    swprintf_s(buf, L"%.3f", k);
    return buf;
}

std::wstring MultiplierToString(int m) { return m == kMultiplierAuto ? L"auto" : std::to_wstring(m); }

int ParseMultiplier(const std::wstring& s) {
    if (_wcsicmp(s.c_str(), L"auto") == 0) return kMultiplierAuto;
    return Clamp(ToInt(s, 2), 1, 6);
}

int ParsePresentMode(const std::wstring& s) {
    if (_wcsicmp(s.c_str(), L"immediate") == 0) return 1;
    if (_wcsicmp(s.c_str(), L"mailbox") == 0) return 2;
    if (_wcsicmp(s.c_str(), L"fifo") == 0 || _wcsicmp(s.c_str(), L"vsync") == 0) return 3;
    return 0;  // "game"
}

// v0.4 wrote a "; Multiplier=..." entry. The profile API treats ';' lines as comments and cannot remove
// them, so filter them out of the file text.
void RemoveStrayLines() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, IniPath().c_str(), L"rb") != 0 || !f) return;
    std::string text;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
    fclose(f);
    std::string out;
    bool removed = false;
    for (size_t pos = 0; pos < text.size();) {
        size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos + 1);
        pos = end == std::string::npos ? text.size() : end + 1;
        if (line.rfind("; Multiplier", 0) == 0) {
            removed = true;
            continue;
        }
        out += line;
    }
    if (!removed || _wfopen_s(&f, IniPath().c_str(), L"wb") != 0 || !f) return;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
}

Config& Mutable() {
    static Config cfg = [] {
        Config c;
        RemoveStrayLines();

        c.multiplier = ParseMultiplier(ReadOrDefault(L"FrameGen", L"Multiplier", L"2"));
        c.autoKeep = ClampKeep(ToFloat(ReadOrDefault(L"FrameGen", L"AutoKeep", L"0.95"), 0.95f));
        c.outputCap = ParseCap(ReadOrDefault(L"FrameGen", L"OutputCap", L"off"));
        c.presentModeOverride = ParsePresentMode(ReadOrDefault(L"Display", L"PresentMode", L"game"));
        c.overlayKey = ToInt(ReadOrDefault(L"Overlay", L"ToggleKey", L"0x2D"), 0x2D);
        c.useGameCamera = _wcsicmp(ReadOrDefault(L"Camera", L"Source", L"game").c_str(), L"approx") != 0;
        c.fovYDegrees = ToFloat(ReadOrDefault(L"Camera", L"FovYDegrees", L"60"), 60.0f);
        c.nearPlane = ToFloat(ReadOrDefault(L"Camera", L"Near", L"0.1"), 0.1f);
        c.farPlane = ToFloat(ReadOrDefault(L"Camera", L"Far", L"10000"), 10000.0f);
        c.verboseLog = ToInt(ReadOrDefault(L"Debug", L"VerboseLog", L"0"), 0) != 0;
        ReadOrDefault(L"Debug", L"FrameTrace", L"0");

        static const char* kModes[] = {"game", "immediate", "mailbox", "fifo"};
        EFG_LOG("config: multiplier %ls (auto keep %.3f), output cap %ls, present mode %s, overlay key 0x%02x, camera %s, verbose %d",
                c.multiplier == kMultiplierAuto ? L"auto" : (std::to_wstring(c.multiplier) + L"x").c_str(), c.autoKeep,
                CapToString(c.outputCap).c_str(), kModes[c.presentModeOverride], c.overlayKey,
                c.useGameCamera ? "game" : "approx", c.verboseLog);
        return c;
    }();
    return cfg;
}

}  // namespace

const Config& LoadConfig() { return Mutable(); }

void SetMultiplier(int multiplier) {
    Config& c = Mutable();
    if (multiplier != kMultiplierAuto) multiplier = Clamp(multiplier, 1, 6);
    if (c.multiplier == multiplier) return;
    c.multiplier = multiplier;
    Write(L"FrameGen", L"Multiplier", MultiplierToString(multiplier));
    EFG_LOG("config: multiplier set to %ls (saved)", MultiplierToString(multiplier).c_str());
}

void SetAutoKeep(float keep) {
    Config& c = Mutable();
    keep = ClampKeep(keep);
    const std::wstring old = KeepToString(c.autoKeep), now = KeepToString(keep);
    c.autoKeep = keep;
    if (now == old) return;
    Write(L"FrameGen", L"AutoKeep", now);
    EFG_LOG("config: auto keep set to %ls (saved)", now.c_str());
}

void SetUseGameCamera(bool use) {
    Config& c = Mutable();
    if (c.useGameCamera == use) return;
    c.useGameCamera = use;
    Write(L"Camera", L"Source", use ? L"game" : L"approx");
    EFG_LOG("config: camera source set to %s (saved)", use ? "game" : "approx");
}

void SetOutputCap(int cap) {
    Config& c = Mutable();
    if (c.outputCap == cap) return;
    c.outputCap = cap;
    Write(L"FrameGen", L"OutputCap", CapToString(cap));
    EFG_LOG("config: output cap set to %ls (saved)", CapToString(cap).c_str());
}

}  // namespace efg
