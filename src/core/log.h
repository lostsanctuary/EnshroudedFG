#pragma once

#include <string>

namespace efg {

// Directory containing the host executable (the game folder), with trailing backslash.
const std::wstring& GameDir();

// <GameDir>\EnshroudedFG\ - mod data folder (log, plugins). Created on first use.
const std::wstring& ModDir();

void LogInit();
void Log(const char* fmt, ...);

}  // namespace efg

#define EFG_LOG(...) ::efg::Log(__VA_ARGS__)
