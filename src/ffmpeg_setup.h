#pragma once
#include <string>
#include <functional>
#include <atomic>

// First-run helper: if FFmpeg is not present, download an official build from
// gyan.dev (over HTTPS, checksum-verified) into the app's own folder. CrunchCord
// never redistributes FFmpeg; the user's machine fetches it from the official
// source, so the release stays license-clean.
namespace ffsetup {

// True if both ffmpeg and ffprobe are runnable (next to the exe, or on PATH).
bool present();

struct Progress {
    float       fraction; // 0..1
    std::string stage;
};

// Download, verify, and extract ffmpeg.exe + ffprobe.exe next to the exe.
// Returns an empty string on success, or a human-readable error.
std::string install(const std::function<void(const Progress&)>& on_progress,
                    std::atomic<bool>& cancel);

} // namespace ffsetup
