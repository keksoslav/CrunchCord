#pragma once
#include <string>
#include <functional>
#include <atomic>

namespace proc {

struct CaptureResult {
    bool launched = false;
    int  exit_code = -1;
    std::string output; // merged stdout + stderr
};

// Run a program, block until exit, capture merged stdout+stderr.
CaptureResult run_capture(const std::wstring& exe, const std::wstring& args);

// Run a long process (ffmpeg). on_line is called for each output line.
// Polls `cancel`; if set, terminates the child. `tail_out` (optional) receives
// the last lines of output for diagnostics.
// Returns: exit code >=0, -2 if cancelled, -1 if the process failed to launch.
int run_streaming(const std::wstring& exe,
                  const std::wstring& args,
                  const std::function<void(const std::string&)>& on_line,
                  std::atomic<bool>& cancel,
                  std::string* tail_out);

// Directory of our own executable, with trailing backslash.
std::wstring exe_dir();

} // namespace proc
