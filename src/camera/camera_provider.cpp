#include "camera/camera_provider.h"

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/log.h"

namespace efg::camera {
namespace {

// ---- signatures (each matches exactly once in build 25988036; see PLAN.md) --------------------

// Call to the per-update camera-copy setter: setter = match+9+rel32(+5); global = setter+10+disp32(setter+6).
const char* kSigCameraCopyCall = "48 8D 4D 10 E8 ?? ?? ?? ?? EB ?? 41 0F 10 85 ?? ?? ?? ?? F2 41 0F 10 8D ?? ?? ?? ??";
// Camera store into GameClient: disp32(+17) = camera offset, disp32(+60) = vertical FOV offset.
const char* kSigCameraStore =
    "0F 10 45 10 49 8D 44 3D 00 0F 10 4D 20 41 0F 11 85 ?? ?? ?? ?? 0F 10 45 30 41 0F 11 8D ?? ?? ?? ?? F2 0F 10 4D 40 "
    "41 0F 11 85 ?? ?? ?? ?? F2 41 0F 11 8D ?? ?? ?? ?? F3 41 0F 11 B5 ?? ?? ?? ??";
// Static GameModule object: module = match+7+disp32(+3).
const char* kSigModule =
    "48 8D 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 48 8D 81 ?? ?? ?? ?? 33 C9 38 88 98 00 00 00 48 0F 44 C1 C3";
// Module -> state machine pointer offset: byte at +8.
const char* kSigStateMachine = "E8 ?? ?? ?? ?? 48 89 47 10 48 85 C0 0F 85";
// Play state -> GameClient pointer offset: disp32 at +15.
const char* kSigClient = "48 8B C8 E8 ?? ?? ?? ?? 49 8B 4F 50 49 89 87 ?? ?? ?? ?? 49 8B 47 40";

constexpr uintptr_t kStatesOffset = 0x38;  // state machine -> state array
constexpr int kPlayStateId = 3;
constexpr uintptr_t kStateIdOffset = 0x10;
constexpr double kFixedToMetres = 1.0 / 4294967296.0;  // 32.32 fixed point

struct Locations {
    bool scanned = false;
    uintptr_t cameraCopy = 0;   // static Transform copy (position + rotation)
    uintptr_t module = 0;       // GameModule object
    uint32_t smOffset = 0, clientOffset = 0, cameraOffset = 0, fovOffset = 0;
};
Locations g_loc;

bool ParsePattern(const char* text, std::vector<int>& out) {
    out.clear();
    for (const char* p = text; *p;) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?') {
            out.push_back(-1);
            p += (p[1] == '?') ? 2 : 1;
        } else {
            char hex[3] = {p[0], p[1], 0};
            out.push_back(static_cast<int>(strtoul(hex, nullptr, 16)));
            p += 2;
        }
    }
    return !out.empty();
}

// Returns the unique match address in the exe's .text section, or 0 (none or ambiguous).
uintptr_t FindUnique(const char* sig, const char* name) {
    std::vector<int> pat;
    ParsePattern(sig, pat);
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t found = 0;
    int count = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint8_t* start = reinterpret_cast<const uint8_t*>(base + sec->VirtualAddress);
        const size_t size = sec->Misc.VirtualSize;
        const uint8_t first = static_cast<uint8_t>(pat[0]);
        for (size_t off = 0; off + pat.size() <= size; ++off) {
            if (start[off] != first) continue;
            size_t k = 1;
            for (; k < pat.size(); ++k) {
                if (pat[k] >= 0 && start[off + k] != pat[k]) break;
            }
            if (k == pat.size()) {
                if (++count == 1) found = reinterpret_cast<uintptr_t>(start + off);
            }
        }
    }
    if (count != 1) {
        EFG_LOG("camera: signature %s matched %d times - unusable", name, count);
        return 0;
    }
    EFG_LOG("camera: signature %s at exe+0x%llx", name, (unsigned long long)(found - base));
    return found;
}

int32_t Rel32(uintptr_t at) {
    int32_t v;
    memcpy(&v, reinterpret_cast<const void*>(at), 4);
    return v;
}

// Copies `n` bytes from possibly-invalid memory; false if it faults.
bool SafeCopy(void* dst, uintptr_t src, size_t n) {
    if (!src) return false;
    __try {
        memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
bool Read(uintptr_t addr, T& out) {
    return SafeCopy(&out, addr, sizeof(T));
}

struct RawTransform {
    int64_t position[3];
    float rotation[4];
    float scale[3];
};
static_assert(sizeof(RawTransform) == 0x38 && offsetof(RawTransform, scale) == 0x28, "transform layout");

bool PlausibleRotation(const float q[4]) {
    double n = double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2] + double(q[3]) * q[3];
    return std::isfinite(n) && std::fabs(n - 1.0) < 0.01;
}

// The GameClient (null outside a world), through the static module object.
uintptr_t GameClient() {
    if (!g_loc.module || !g_loc.smOffset || !g_loc.clientOffset) return 0;
    uintptr_t sm = 0, states = 0, play = 0, client = 0;
    int32_t id = -1;
    if (!Read(g_loc.module + g_loc.smOffset, sm) || !Read(sm + kStatesOffset, states) ||
        !Read(states + kPlayStateId * sizeof(uintptr_t), play) || !Read(play + kStateIdOffset, id) ||
        id != kPlayStateId || !Read(play + g_loc.clientOffset, client)) {
        return 0;
    }
    return client;
}

}  // namespace

bool Init() {
    if (g_loc.scanned) return g_loc.cameraCopy != 0 || g_loc.module != 0;
    g_loc.scanned = true;
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

    if (uintptr_t m = FindUnique(kSigCameraCopyCall, "camera-copy call")) {
        uintptr_t setter = m + 9 + Rel32(m + 5);
        g_loc.cameraCopy = setter + 10 + Rel32(setter + 6);
        EFG_LOG("camera: static camera transform at exe+0x%llx", (unsigned long long)(g_loc.cameraCopy - base));
    }
    if (uintptr_t m = FindUnique(kSigModule, "game module")) g_loc.module = m + 7 + Rel32(m + 3);
    if (uintptr_t m = FindUnique(kSigStateMachine, "state machine offset")) g_loc.smOffset = *reinterpret_cast<uint8_t*>(m + 8);
    if (uintptr_t m = FindUnique(kSigClient, "game client offset")) g_loc.clientOffset = static_cast<uint32_t>(Rel32(m + 15));
    if (uintptr_t m = FindUnique(kSigCameraStore, "camera store")) {
        g_loc.cameraOffset = static_cast<uint32_t>(Rel32(m + 17));
        g_loc.fovOffset = static_cast<uint32_t>(Rel32(m + 60));
    }
    EFG_LOG("camera: module exe+0x%llx, offsets sm 0x%x client 0x%x camera 0x%x fov 0x%x",
            (unsigned long long)(g_loc.module ? g_loc.module - base : 0), g_loc.smOffset, g_loc.clientOffset,
            g_loc.cameraOffset, g_loc.fovOffset);
    return g_loc.cameraCopy != 0 || g_loc.module != 0;
}

CameraSample Read() {
    CameraSample s;
    if (!Init()) return s;

    // Preferred: the GameClient's camera (also carries FOV / near / far).
    RawTransform t{};
    bool haveTransform = false;
    if (uintptr_t client = GameClient(); client && g_loc.cameraOffset) {
        float nf[2] = {}, fov = 0;
        if (Read(client + g_loc.cameraOffset, t) && PlausibleRotation(t.rotation)) {
            haveTransform = true;
            if (Read(client + g_loc.cameraOffset + 0x38, nf) && Read(client + g_loc.fovOffset, fov) &&
                fov > 0.1f && fov < 3.0f && nf[0] > 1e-5f && nf[0] < 10.0f && nf[1] > nf[0] * 10.0f) {
                s.hasProjection = true;
                s.fovY = fov;
                s.nearPlane = nf[0];
                s.farPlane = nf[1];
            }
        }
    }
    // Fallback: the static copy of the camera transform (position + rotation only).
    if (!haveTransform && g_loc.cameraCopy) {
        haveTransform = Read(g_loc.cameraCopy, t) && PlausibleRotation(t.rotation);
    }
    if (!haveTransform) return s;

    for (int i = 0; i < 3; ++i) s.position[i] = double(t.position[i]) * kFixedToMetres;
    memcpy(s.rotation, t.rotation, sizeof(s.rotation));
    s.valid = true;
    return s;
}

}  // namespace efg::camera
