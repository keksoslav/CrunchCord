#pragma once
#include <string>

// Explorer right-click ("Compress for Discord") integration, per-user (HKCU),
// so no administrator rights are needed.
namespace shellreg {

bool is_installed();
bool install(const std::wstring& exe_path);
bool uninstall();

} // namespace shellreg
