#include "core/config.h"

#include <windows.h>

#include <cwchar>
#include <string>

#include "core/log.h"

namespace efg {
namespace {

float ReadFloat(const wchar_t* section, const wchar_t* key, float def, const std::wstring& path) {
    wchar_t buf[64]{};
    GetPrivateProfileStringW(section, key, L"", buf, 64, path.c_str());
    if (!buf[0]) return def;
    wchar_t* end = nullptr;
    float v = wcstof(buf, &end);
    return end != buf ? v : def;
}

void WriteDefaults(const std::wstring& path, const Config& c) {
    wchar_t buf[64];
    WritePrivateProfileStringW(L"FrameGen", L"; Multiplier", L"1 = off, 2..6 = DLSS frame generation 2x..6x", path.c_str());
    swprintf_s(buf, L"%d", c.multiplier);
    WritePrivateProfileStringW(L"FrameGen", L"Multiplier", buf, path.c_str());
    swprintf_s(buf, L"%.1f", c.fovYDegrees);
    WritePrivateProfileStringW(L"Camera", L"FovYDegrees", buf, path.c_str());
    swprintf_s(buf, L"%.2f", c.nearPlane);
    WritePrivateProfileStringW(L"Camera", L"Near", buf, path.c_str());
    swprintf_s(buf, L"%.0f", c.farPlane);
    WritePrivateProfileStringW(L"Camera", L"Far", buf, path.c_str());
}

}  // namespace

const Config& LoadConfig() {
    static const Config cfg = [] {
        Config c;
        std::wstring path = GameDir() + L"EnshroudedFG.ini";
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) WriteDefaults(path, c);
        c.multiplier = static_cast<int>(GetPrivateProfileIntW(L"FrameGen", L"Multiplier", c.multiplier, path.c_str()));
        if (c.multiplier < 1) c.multiplier = 1;
        if (c.multiplier > 6) c.multiplier = 6;
        c.fovYDegrees = ReadFloat(L"Camera", L"FovYDegrees", c.fovYDegrees, path);
        c.nearPlane = ReadFloat(L"Camera", L"Near", c.nearPlane, path);
        c.farPlane = ReadFloat(L"Camera", L"Far", c.farPlane, path);
        EFG_LOG("config: multiplier %dx, fovY %.1f, near %.2f, far %.0f", c.multiplier, c.fovYDegrees, c.nearPlane,
                c.farPlane);
        return c;
    }();
    return cfg;
}

}  // namespace efg
