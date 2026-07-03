// Full windows.h (not the lean variant) so PROPID and COM base types exist for
// GDI+. shellapi.h provides HDROP / DROPFILES; objidl.h provides IStream.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>   // DROPFILES
#include <objidl.h>
#include <algorithm>
// gdiplus.h expects min/max in the Gdiplus namespace; NOMINMAX removed the macros.
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include "clipboard.h"
#include <string>

namespace clip {

static ULONG_PTR g_gdip_token = 0;

void init() {
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&g_gdip_token, &in, nullptr);
}
void shutdown() {
    if (g_gdip_token) { Gdiplus::GdiplusShutdown(g_gdip_token); g_gdip_token = 0; }
}

static int encoder_clsid(const wchar_t* mime, CLSID* clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (!size) return -1;
    auto* info = (Gdiplus::ImageCodecInfo*)malloc(size);
    if (!info) return -1;
    Gdiplus::GetImageEncoders(num, size, info);
    int idx = -1;
    for (UINT i = 0; i < num; ++i)
        if (wcscmp(info[i].MimeType, mime) == 0) { *clsid = info[i].Clsid; idx = (int)i; break; }
    free(info);
    return idx;
}

static std::wstring save_clip_image(HWND owner) {
    std::wstring result;
    if (!OpenClipboard(owner)) return result;
    HBITMAP hbmp = (HBITMAP)GetClipboardData(CF_BITMAP); // synthesized from CF_DIB if needed
    if (hbmp) {
        Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromHBITMAP(hbmp, nullptr);
        if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
            wchar_t tmp[MAX_PATH];
            DWORD n = GetTempPathW(MAX_PATH, tmp);
            std::wstring path = std::wstring(tmp, n) + L"crunchcord_paste_" +
                                std::to_wstring(GetTickCount()) + L".png";
            CLSID png;
            if (encoder_clsid(L"image/png", &png) >= 0 &&
                bmp->Save(path.c_str(), &png, nullptr) == Gdiplus::Ok)
                result = path;
        }
        delete bmp;
    }
    CloseClipboard();
    return result;
}

bool has_pasteable() {
    return IsClipboardFormatAvailable(CF_HDROP) ||
           IsClipboardFormatAvailable(CF_BITMAP) ||
           IsClipboardFormatAvailable(CF_DIB);
}

std::vector<std::wstring> paste(HWND owner) {
    std::vector<std::wstring> out;
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        if (OpenClipboard(owner)) {
            HDROP hd = (HDROP)GetClipboardData(CF_HDROP);
            if (hd) {
                UINT cnt = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < cnt; ++i) {
                    wchar_t p[MAX_PATH];
                    if (DragQueryFileW(hd, i, p, MAX_PATH)) out.emplace_back(p);
                }
            }
            CloseClipboard();
        }
    } else if (IsClipboardFormatAvailable(CF_BITMAP) || IsClipboardFormatAvailable(CF_DIB)) {
        std::wstring img = save_clip_image(owner);
        if (!img.empty()) out.push_back(img);
    }
    return out;
}

bool copy_files(HWND owner, const std::vector<std::wstring>& paths) {
    if (paths.empty()) return false;

    // CF_HDROP payload: DROPFILES header + double-null-terminated wide path list.
    size_t chars = 1; // trailing extra null
    for (auto& p : paths) chars += p.size() + 1;
    size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);

    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) return false;
    auto* df = (DROPFILES*)GlobalLock(h);
    ZeroMemory(df, sizeof(DROPFILES));
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* dst = (wchar_t*)((BYTE*)df + sizeof(DROPFILES));
    for (auto& p : paths) {
        memcpy(dst, p.c_str(), (p.size() + 1) * sizeof(wchar_t));
        dst += p.size() + 1;
    }
    *dst = 0;
    GlobalUnlock(h);

    if (!OpenClipboard(owner)) { GlobalFree(h); return false; }
    EmptyClipboard();
    if (!SetClipboardData(CF_HDROP, h)) { CloseClipboard(); GlobalFree(h); return false; }
    CloseClipboard(); // clipboard owns h now
    return true;
}

} // namespace clip
