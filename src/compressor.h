#pragma once
#include <string>
#include <functional>
#include <atomic>
#include <cstdint>

enum class Codec { H265, AV1, H264 };

// Encoder tuning. Defaults chosen for "smallest file that still looks good".
struct EncodeOptions {
    Codec       codec       = Codec::H265;
    std::string preset_x    = "medium"; // libx264 / libx265 preset
    int         preset_svt  = 6;        // libsvtav1 preset (0 slow .. 13 fast)
    int         max_height  = 0;        // 0 = auto ladder, else cap (e.g. 720)
    int         fps_cap     = 0;        // 0 = keep source
    int         audio_kbps  = 0;        // 0 = auto
    std::wstring out_dir;               // empty = same folder as the input
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
    bool         ok       = false;
    std::wstring out_path;
    uint64_t     out_size = 0;
    std::string  message; // summary or error text
};

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
