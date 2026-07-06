#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include "ffmpeg_setup.h"
#include "process.h"
#include <string>
#include <vector>
#include <cctype>

namespace ffsetup {

// The official gyan.dev "full" build has every encoder CrunchCord uses
// (x264, x265, SVT-AV1, WebP, AAC, NVENC/AMF/QSV). The rolling URL always
// exists; the matching checksum is fetched at download time so it never rots.
static const wchar_t* kZipUrl = L"https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-full.7z";
static const wchar_t* kShaUrl = L"https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-full.7z.sha256";

static bool file_exists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool runnable(const wchar_t* name) {
    std::wstring local = proc::exe_dir() + name;
    std::wstring exe = file_exists(local) ? local : std::wstring(name);
    auto r = proc::run_capture(exe, L"-version");
    return r.launched && r.exit_code == 0;
}

bool present() {
    return runnable(L"ffmpeg.exe") && runnable(L"ffprobe.exe");
}

// First whitespace-delimited 64-hex token (lowercased), or "".
static std::string first_sha256(const std::string& text) {
    std::string tok;
    auto is_hex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    for (size_t i = 0; i <= text.size(); ++i) {
        char c = (i < text.size()) ? text[i] : ' ';
        if (is_hex(c)) {
            tok += (char)tolower((unsigned char)c);
        } else {
            if (tok.size() == 64) return tok;
            tok.clear();
        }
    }
    return "";
}

static std::string sha256_file(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    DWORD objLen = 0, cb = 0;
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &cb, 0);
    std::vector<UCHAR> obj(objLen);
    BCryptCreateHash(alg, &h, obj.data(), objLen, nullptr, 0, 0);
    std::vector<char> buf(1 << 20);
    DWORD n = 0;
    while (ReadFile(f, buf.data(), (DWORD)buf.size(), &n, nullptr) && n > 0)
        BCryptHashData(h, (PUCHAR)buf.data(), n, 0);
    CloseHandle(f);
    UCHAR digest[32];
    BCryptFinishHash(h, digest, 32, 0);
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    static const char* hx = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 32; ++i) { out += hx[digest[i] >> 4]; out += hx[digest[i] & 0xf]; }
    return out;
}

// Pull a percentage out of a curl --progress-bar line.
static bool parse_percent(const std::string& line, float& out) {
    size_t pct = line.find('%');
    if (pct == std::string::npos) return false;
    size_t i = pct;
    while (i > 0 && (isdigit((unsigned char)line[i - 1]) || line[i - 1] == '.')) i--;
    if (i == pct) return false;
    try { out = std::stof(line.substr(i, pct - i)); return true; } catch (...) { return false; }
}

std::string install(const std::function<void(const Progress&)>& on_progress,
                    std::atomic<bool>& cancel) {
    std::wstring dest = proc::exe_dir();
    wchar_t tmpBuf[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpBuf);
    std::wstring tmp = std::wstring(tmpBuf) + L"crunchcord_ffdl";
    CreateDirectoryW(tmp.c_str(), nullptr);
    std::wstring arc = tmp + L"\\ffmpeg.7z";

    auto report = [&](float f, const char* s) { on_progress(Progress{f, s}); };
    std::string tail;

    // 1. Fetch the published checksum.
    report(0.02f, "Checking latest FFmpeg");
    auto shaRes = proc::run_capture(L"curl.exe", std::wstring(L"-fsSL ") + kShaUrl);
    if (!shaRes.launched) return "Could not run curl (requires Windows 10 1803 or newer).";
    if (shaRes.exit_code != 0) return "Could not reach the FFmpeg download server.";
    std::string published = first_sha256(shaRes.output);
    if (published.size() != 64) return "The download server returned an unexpected checksum.";

    // 2. Download the archive with progress.
    if (cancel.load()) return "Cancelled.";
    std::wstring dlArgs = L"-fL --progress-bar -o \"" + arc + L"\" " + kZipUrl;
    int rc = proc::run_streaming(L"curl.exe", dlArgs,
        [&](const std::string& ln) {
            float p; if (parse_percent(ln, p)) report(0.05f + (p / 100.f) * 0.85f, "Downloading FFmpeg (about 160 MB)");
        }, cancel, &tail);
    if (rc == -2) { DeleteFileW(arc.c_str()); return "Cancelled."; }
    if (rc != 0)  { DeleteFileW(arc.c_str()); return "Download failed. Check your internet connection.\n" + tail; }

    // 3. Verify the checksum.
    report(0.92f, "Verifying download");
    std::string actual = sha256_file(arc);
    if (actual != published) {
        DeleteFileW(arc.c_str());
        return "Checksum mismatch. The download may be corrupted; please try again.";
    }

    // 4. Extract ffmpeg.exe + ffprobe.exe (flattened) next to our exe.
    report(0.96f, "Installing");
    // Strip the trailing backslash: a quoted path ending in \" is misparsed as
    // an escaped quote by the command-line splitter.
    std::wstring destArg = dest;
    while (!destArg.empty() && (destArg.back() == L'\\' || destArg.back() == L'/'))
        destArg.pop_back();
    std::wstring exArgs = L"-xf \"" + arc + L"\" -C \"" + destArg +
                          L"\" --strip-components=2 \"*/bin/ffmpeg.exe\" \"*/bin/ffprobe.exe\"";
    auto exRes = proc::run_capture(L"tar.exe", exArgs);
    DeleteFileW(arc.c_str());
    RemoveDirectoryW(tmp.c_str());
    if (!exRes.launched || exRes.exit_code != 0)
        return "Could not extract the download.";

    if (!file_exists(dest + L"ffmpeg.exe") || !file_exists(dest + L"ffprobe.exe"))
        return "Extraction finished but ffmpeg was not found.";

    report(1.0f, "Done");
    return "";
}

} // namespace ffsetup
