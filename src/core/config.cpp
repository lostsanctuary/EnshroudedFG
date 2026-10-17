#include "core/config.h"

#include <windows.h>

#include <atomic>
#include <cwchar>
#include <string>

#include "core/log.h"

namespace efg {
namespace {

const std::wstring& IniPath() {
    static const std::wstring path = GameDir() + L"EnshroudedFG.ini";
    return path;
}

float ReadFloat(const wchar_t* section, const wchar_t* key, float def) {
    wchar_t buf[64]{};
    GetPrivateProfileStringW(section, key, L"", buf, 64, IniPath().c_str());
    if (!buf[0]) return def;
    wchar_t* end = nullptr;
    float v = wcstof(buf, &end);
    return end != buf ? v : def;
}

void WriteInt(const wchar_t* section, const wchar_t* key, int v) {
    wchar_t buf[32];
    swprintf_s(buf, L"%d", v);
    WritePrivateProfileStringW(section, key, buf, IniPath().c_str());
}

void WriteFloat(const wchar_t* section, const wchar_t* key, float v, const wchar_t* fmt) {
    wchar_t buf[64];
    swprintf_s(buf, fmt, v);
    WritePrivateProfileStringW(section, key, buf, IniPath().c_str());
}

void WriteDefaults(const Config& c) {
    WriteInt(L"FrameGen", L"Multiplier", c.multiplier);
    WriteInt(L"Overlay", L"ToggleKey", c.overlayKey);
    WriteFloat(L"Camera", L"FovYDegrees", c.fovYDegrees, L"%.1f");
    WriteFloat(L"Camera", L"Near", c.nearPlane, L"%.2f");
    WriteFloat(L"Camera", L"Far", c.farPlane, L"%.0f");
}

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

Config& Mutable() {
    static Config cfg = [] {
        Config c;
        const std::wstring& path = IniPath();
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) WriteDefaults(c);
        c.multiplier = Clamp(static_cast<int>(GetPrivateProfileIntW(L"FrameGen", L"Multiplier", c.multiplier, path.c_str())), 1, 6);
        c.overlayKey = static_cast<int>(GetPrivateProfileIntW(L"Overlay", L"ToggleKey", c.overlayKey, path.c_str()));
        c.fovYDegrees = ReadFloat(L"Camera", L"FovYDegrees", c.fovYDegrees);
        c.nearPlane = ReadFloat(L"Camera", L"Near", c.nearPlane);
        c.farPlane = ReadFloat(L"Camera", L"Far", c.farPlane);
        EFG_LOG("config: multiplier %dx, overlay key 0x%02x, fovY %.1f, near %.2f, far %.0f", c.multiplier, c.overlayKey,
                c.fovYDegrees, c.nearPlane, c.farPlane);
        return c;
    }();
    return cfg;
}

}  // namespace

const Config& LoadConfig() { return Mutable(); }

void SetMultiplier(int multiplier) {
    Config& c = Mutable();
    multiplier = Clamp(multiplier, 1, 6);
    if (c.multiplier == multiplier) return;
    c.multiplier = multiplier;
    WriteInt(L"FrameGen", L"Multiplier", multiplier);
    EFG_LOG("config: multiplier set to %dx (saved)", multiplier);
}

}  // namespace efg
