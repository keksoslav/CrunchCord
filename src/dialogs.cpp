#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#include "dialogs.h"
#include "compressor.h" // is_supported_media

namespace dlg {

static std::vector<std::wstring> run_dialog(HWND owner, bool folder, bool multi) {
    std::vector<std::wstring> result;

    IFileOpenDialog* d = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IFileOpenDialog, reinterpret_cast<void**>(&d));
    if (FAILED(hr) || !d) return result;

    DWORD opts = 0;
    d->GetOptions(&opts);
    if (folder) opts |= FOS_PICKFOLDERS;
    if (multi)  opts |= FOS_ALLOWMULTISELECT;
    opts |= FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST;
    d->SetOptions(opts);

    if (!folder) {
        COMDLG_FILTERSPEC filters[] = {
            { L"Media files",
              L"*.mp4;*.mov;*.mkv;*.avi;*.webm;*.m4v;*.flv;*.wmv;*.mpg;*.mpeg;*.ts;*.m2ts;"
              L"*.mts;*.3gp;*.ogv;*.jpg;*.jpeg;*.png;*.bmp;*.tif;*.tiff;*.gif;*.webp" },
            { L"All files", L"*.*" },
        };
        d->SetFileTypes(2, filters);
        d->SetTitle(L"Add files");
    } else {
        d->SetTitle(L"Choose an output / import folder");
    }

    hr = d->Show(owner);
    if (FAILED(hr)) { d->Release(); return result; } // cancelled

    auto push_item = [&](IShellItem* item) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
            result.emplace_back(path);
            CoTaskMemFree(path);
        }
    };

    if (multi) {
        IShellItemArray* arr = nullptr;
        if (SUCCEEDED(d->GetResults(&arr)) && arr) {
            DWORD count = 0;
            arr->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(arr->GetItemAt(i, &item)) && item) { push_item(item); item->Release(); }
            }
            arr->Release();
        }
    } else {
        IShellItem* item = nullptr;
        if (SUCCEEDED(d->GetResult(&item)) && item) { push_item(item); item->Release(); }
    }

    d->Release();
    return result;
}

std::vector<std::wstring> open_files(HWND owner)  { return run_dialog(owner, false, true); }

std::wstring open_folder(HWND owner) {
    auto r = run_dialog(owner, true, false);
    return r.empty() ? std::wstring() : r[0];
}

std::vector<std::wstring> enum_media(const std::wstring& folder, bool recurse) {
    std::vector<std::wstring> out;
    if (folder.empty()) return out;
    std::wstring base = folder;
    if (base.back() != L'\\' && base.back() != L'/') base += L'\\';

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;

    std::vector<std::wstring> subdirs;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring full = base + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (recurse) subdirs.push_back(full);
        } else if (is_supported_media(full)) {
            out.push_back(full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    for (auto& sd : subdirs) {
        auto sub = enum_media(sd, true);
        out.insert(out.end(), sub.begin(), sub.end());
    }
    return out;
}

} // namespace dlg
