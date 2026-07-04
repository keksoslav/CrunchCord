#pragma once
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <cstdint>

enum class Codec { H265, AV1, H264 };

// 0 = fastest, 1 = balanced, 2 = best quality.
enum class Speed { Fastest, Balanced, Best };

// Which NVENC encoders the current GPU + driver actually support at runtime.
struct HwCaps {
    bool checked = false;
    bool hevc = false;
    bool h264 = false;
    bool av1  = false;
};

struct EncodeOptions {
    Codec        codec        = Codec::H265;
    Speed        speed        = Speed::Balanced;
    bool         use_hardware = true; // use NVENC when available for the codec
    int          max_height   = 0;    // 0 = auto ladder, else cap (e.g. 720)
    int          fps_cap      = 0;    // 0 = keep source
    int          audio_kbps   = 0;    // 0 = auto
    std::wstring out_dir;             // empty = same folder as the input
};

struct MediaInfo {
    bool        ok        = false;
    bool        is_image  = false;
    double      duration  = 0.0; // seconds (0 for still images)
    int         width     = 0;
    int         height    = 0;
    double      fps       = 0.0;
    bool        has_audio = false;
    std::string vcodec;
    std::string err;
};

struct ProgressInfo {
    float       fraction = 0.f; // 0..1 overall
    std::string stage;          // human-readable current step
};

struct CompressResult {
    bool         ok            = false;
    std::wstring out_path;
    uint64_t     out_size      = 0;
    bool         used_hardware = false; // true if a GPU (NVENC) encoder was used
    std::string  message; // summary or error text
};

// Detect NVENC support once (runs quick test encodes) and cache the result.
HwCaps detect_hw_caps();
bool   hw_available_for(Codec c);

MediaInfo probe_media(const std::wstring& path);

// Compress `in_path` to land under `target_bytes`. Reports progress through
// `on_progress`; abort by setting `cancel`.
CompressResult compress_file(const std::wstring& in_path,
                             const MediaInfo& info,
                             uint64_t target_bytes,
                             const EncodeOptions& opts,
                             const std::function<void(const ProgressInfo&)>& on_progress,
                             std::atomic<bool>& cancel);

// Small helpers shared with the GUI.
uint64_t file_size_of(const std::wstring& path);
std::string wide_to_utf8(const std::wstring& w);
std::wstring utf8_to_wide(const std::string& s);
bool is_supported_media(const std::wstring& path); // recognised by extension
std::vector<std::wstring> supported_extensions();  // e.g. "mp4", "png" (no dot)
