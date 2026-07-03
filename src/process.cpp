#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "process.h"
#include <vector>
#include <deque>
#include <algorithm>

namespace proc {

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) return p.substr(0, slash + 1);
    return L"";
}

static std::wstring build_cmdline(const std::wstring& exe, const std::wstring& args) {
    return L"\"" + exe + L"\" " + args;
}

// Open a handle to the NUL device so children never block on stdin.
static HANDLE open_nul_input(SECURITY_ATTRIBUTES* sa) {
    return CreateFileW(L"NUL", GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, sa,
                       OPEN_EXISTING, 0, nullptr);
}

CaptureResult run_capture(const std::wstring& exe, const std::wstring& args) {
    CaptureResult res;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return res;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = open_nul_input(&sa);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = nul;

    PROCESS_INFORMATION pi{};
    std::wstring cl = build_cmdline(exe, args);
    std::vector<wchar_t> cmd(cl.begin(), cl.end());
    cmd.push_back(0);

    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return res; }

    res.launched = true;
    std::string out;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0)
        out.append(buf, n);
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    res.exit_code = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    res.output = std::move(out);
    return res;
}

int run_streaming(const std::wstring& exe,
                  const std::wstring& args,
                  const std::function<void(const std::string&)>& on_line,
                  std::atomic<bool>& cancel,
                  std::string* tail_out) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = open_nul_input(&sa);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = nul;

    PROCESS_INFORMATION pi{};
    std::wstring cl = build_cmdline(exe, args);
    std::vector<wchar_t> cmd(cl.begin(), cl.end());
    cmd.push_back(0);

    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return -1; }

    std::string pending;
    std::deque<std::string> tail;
    bool cancelled = false;

    auto emit_line = [&](std::string line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (tail_out) {
            tail.push_back(line);
            if (tail.size() > 40) tail.pop_front();
        }
        on_line(line);
    };

    auto process_chunk = [&](const char* data, size_t len) {
        pending.append(data, len);
        size_t pos;
        while ((pos = pending.find_first_of("\r\n")) != std::string::npos) {
            emit_line(pending.substr(0, pos));
            pending.erase(0, pos + 1);
        }
    };

    char buf[4096];
    for (;;) {
        if (cancel.load() && !cancelled) {
            TerminateProcess(pi.hProcess, 1);
            cancelled = true;
        }

        DWORD avail = 0;
        BOOL peek = PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr);
        if (peek && avail > 0) {
            DWORD n = 0;
            DWORD want = static_cast<DWORD>(std::min<size_t>(sizeof(buf), avail));
            if (ReadFile(rd, buf, want, &n, nullptr) && n > 0)
                process_chunk(buf, n);
        } else {
            DWORD w = WaitForSingleObject(pi.hProcess, 0);
            if (w == WAIT_OBJECT_0) {
                // Drain whatever is left in the pipe.
                while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                    DWORD n = 0;
                    DWORD want = static_cast<DWORD>(std::min<size_t>(sizeof(buf), avail));
                    if (!ReadFile(rd, buf, want, &n, nullptr) || n == 0) break;
                    process_chunk(buf, n);
                }
                break;
            }
            if (!peek) break; // pipe error
            Sleep(15);
        }
    }
    if (!pending.empty()) emit_line(pending);

    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (tail_out) {
        std::string t;
        for (auto& l : tail) { t += l; t += '\n'; }
        *tail_out = std::move(t);
    }
    if (cancelled) return -2;
    return static_cast<int>(code);
}

} // namespace proc
