#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace efg {
namespace {

std::mutex g_logMutex;
FILE* g_logFile = nullptr;
LARGE_INTEGER g_qpcStart{};
LARGE_INTEGER g_qpcFreq{};

std::wstring ComputeGameDir() {
    wchar_t path[MAX_PATH * 2]{};
    DWORD len = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    std::wstring dir(path, len);
    size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L".\\" : dir.substr(0, slash + 1);
}

}  // namespace

const std::wstring& GameDir() {
    static const std::wstring dir = ComputeGameDir();
    return dir;
}

const std::wstring& ModDir() {
    static const std::wstring dir = [] {
        std::wstring d = GameDir() + L"EnshroudedFG\\";
        CreateDirectoryW(d.c_str(), nullptr);
        return d;
    }();
    return dir;
}

void LogInit() {
    std::lock_guard lock(g_logMutex);
    if (g_logFile) return;
    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpcStart);
    std::wstring path = ModDir() + L"log.txt";
    g_logFile = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
}

void Log(const char* fmt, ...) {
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double t = g_qpcFreq.QuadPart
                   ? double(now.QuadPart - g_qpcStart.QuadPart) / double(g_qpcFreq.QuadPart)
                   : 0.0;

    std::lock_guard lock(g_logMutex);
    if (!g_logFile) return;
    fprintf(g_logFile, "[%10.3f] [tid %5lu] %s\n", t, GetCurrentThreadId(), msg);
    fflush(g_logFile);
}

}  // namespace efg
