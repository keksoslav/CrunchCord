#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace clip {

void init();      // start GDI+ (for decoding pasted images)
void shutdown();

// Put files on the clipboard as CF_HDROP, so they paste straight into Discord.
bool copy_files(HWND owner, const std::vector<std::wstring>& paths);

// Pull content off the clipboard. Returns file paths; a pasted image is saved
// to a temp PNG and its path returned.
std::vector<std::wstring> paste(HWND owner);

// Is there anything we can paste right now (files or an image)?
bool has_pasteable();

} // namespace clip
