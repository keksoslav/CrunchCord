#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "shellreg.h"
#include "compressor.h" // supported_extensions

namespace shellreg {

static const wchar_t* VERB  = L"CrunchCord";
static const wchar_t* LABEL = L"Compress for Discord";

// Per-extension verb under the current user, e.g.
// HKCU\Software\Classes\SystemFileAssociations\.mp4\shell\CrunchCord
static std::wstring verb_key(const std::wstring& ext) {
    return L"Software\\Classes\\SystemFileAssociations\\." + ext + L"\\shell\\" + VERB;
}

static bool set_str(HKEY root, const std::wstring& sub, const wchar_t* name,
                    const std::wstring& data) {
    HKEY k;
    if (RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    LSTATUS s = RegSetValueExW(k, name, 0, REG_SZ,
                    (const BYTE*)data.c_str(), (DWORD)((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return s == ERROR_SUCCESS;
}

bool install(const std::wstring& exe) {
    std::wstring cmd  = L"\"" + exe + L"\" \"%1\"";
    std::wstring icon = exe + L",0";
    bool ok = true;
    for (auto& e : supported_extensions()) {
        std::wstring base = verb_key(e);
        ok &= set_str(HKEY_CURRENT_USER, base, nullptr, LABEL);
        set_str(HKEY_CURRENT_USER, base, L"Icon", icon);
        ok &= set_str(HKEY_CURRENT_USER, base + L"\\command", nullptr, cmd);
    }
    return ok;
}

bool uninstall() {
    for (auto& e : supported_extensions())
        RegDeleteTreeW(HKEY_CURRENT_USER, verb_key(e).c_str());
    return true;
}

bool is_installed() {
    HKEY k;
    std::wstring sub = verb_key(L"mp4") + L"\\command";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_READ, &k) == ERROR_SUCCESS) {
        RegCloseKey(k);
        return true;
    }
    return false;
}

} // namespace shellreg
