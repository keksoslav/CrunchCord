#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace dlg {

// Modern Windows open dialogs. Return empty on cancel.
std::vector<std::wstring> open_files(HWND owner);   // multi-select
std::wstring              open_folder(HWND owner);  // single folder

// Recursively (or not) list supported media files inside a folder.
std::vector<std::wstring> enum_media(const std::wstring& folder, bool recurse);

} // namespace dlg
