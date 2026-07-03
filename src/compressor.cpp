#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "compressor.h"
#include "process.h"
#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cwctype>

// ---------------------------------------------------------------------------
// String / path helpers
// ---------------------------------------------------------------------------
std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

uint64_t file_size_of(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d))
        return (uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    return 0;
}

static bool file_exists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring dir_of(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : p.substr(0, s + 1);
}
static std::wstring stem_of(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    std::wstring name = s == std::wstring::npos ? p : p.substr(s + 1);
    size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}
static std::wstring ext_lower(const std::wstring& p) {
    size_t dot = p.find_last_of(L'.');
    if (dot == std::wstring::npos) return L"";
    std::wstring e = p.substr(dot + 1);
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e;
}

// Locate ffmpeg/ffprobe: prefer a copy next to our exe, else rely on PATH.
static std::wstring tool_path(const wchar_t* exe_name) {
    std::wstring local = proc::exe_dir() + exe_name;
    if (file_exists(local)) return local;
    return exe_name; // found via PATH
}
static std::wstring ffmpeg()  { return tool_path(L"ffmpeg.exe"); }
static std::wstring ffprobe() { return tool_path(L"ffprobe.exe"); }

static std::wstring quote(const std::wstring& s) { return L"\"" + s + L"\""; }

// Build a unique 2-pass log prefix in the temp directory.
static std::wstring make_passlog() {
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    std::wstring dir(tmp, n);
    static long counter = 0;
    long id = InterlockedIncrement(&counter);
    std::wstringstream ss;
    ss << dir << L"dcz_" << GetCurrentProcessId() << L"_" << id;
    return ss.str();
}

// ---------------------------------------------------------------------------
// Probing
// ---------------------------------------------------------------------------
static bool is_image_ext(const std::wstring& e) {
    return e == L"jpg" || e == L"jpeg" || e == L"png" || e == L"bmp" ||
           e == L"tif" || e == L"tiff";
}
static bool is_maybe_animated_ext(const std::wstring& e) {
    return e == L"gif" || e == L"webp" || e == L"apng";
}

static std::string get_val(const std::string& text, const std::string& key) {
    // Parse "key=value" lines from ffprobe default output.
    size_t pos = 0;
    while ((pos = text.find(key + "=", pos)) != std::string::npos) {
        // ensure it's at line start
        if (pos == 0 || text[pos - 1] == '\n' || text[pos - 1] == '\r') {
            size_t start = pos + key.size() + 1;
            size_t end = text.find_first_of("\r\n", start);
            return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        }
        pos += key.size() + 1;
    }
    return "";
}

MediaInfo probe_media(const std::wstring& path) {
    MediaInfo mi;
    if (!file_exists(path)) { mi.err = "File not found"; return mi; }

    std::wstring args =
        L"-v error -hide_banner -select_streams v:0 "
        L"-show_entries stream=width,height,r_frame_rate,codec_name,nb_frames "
        L"-show_entries format=duration -of default=noprint_wrappers=1 " + quote(path);
    auto r = proc::run_capture(ffprobe(), args);
    if (!r.launched) { mi.err = "Could not launch ffprobe (is FFmpeg installed / on PATH?)"; return mi; }

    const std::string& t = r.output;
    mi.vcodec = get_val(t, "codec_name");
    try { mi.width  = std::stoi(get_val(t, "width")); }  catch (...) {}
    try { mi.height = std::stoi(get_val(t, "height")); } catch (...) {}

    std::string rfr = get_val(t, "r_frame_rate");
    if (!rfr.empty()) {
        size_t slash = rfr.find('/');
        if (slash != std::string::npos) {
            try {
                double num = std::stod(rfr.substr(0, slash));
                double den = std::stod(rfr.substr(slash + 1));
                if (den != 0) mi.fps = num / den;
            } catch (...) {}
        }
    }
    std::string dur = get_val(t, "duration");
    if (!dur.empty() && dur != "N/A") { try { mi.duration = std::stod(dur); } catch (...) {} }

    // Audio present?
    std::wstring aargs = L"-v error -select_streams a -show_entries stream=codec_name "
                         L"-of default=noprint_wrappers=1:nokey=1 " + quote(path);
    auto ar = proc::run_capture(ffprobe(), aargs);
    mi.has_audio = ar.launched && ar.output.find_first_not_of(" \r\n\t") != std::string::npos;

    // Classify image vs video.
    std::wstring e = ext_lower(path);
    long nb_frames = 0;
    try { nb_frames = std::stol(get_val(t, "nb_frames")); } catch (...) {}

    if (is_image_ext(e)) {
        mi.is_image = true;
    } else if (is_maybe_animated_ext(e)) {
        mi.is_image = !(mi.duration > 0.05 || nb_frames > 1);
    } else {
        mi.is_image = false;
    }

    if (!mi.is_image) {
        if (mi.duration <= 0.0 && mi.fps > 0 && nb_frames > 1)
            mi.duration = nb_frames / mi.fps;
        if (mi.duration <= 0.0) {
            mi.err = "Could not determine video duration";
            return mi;
        }
        if (mi.width <= 0 || mi.height <= 0) {
            mi.err = "Could not read video dimensions";
            return mi;
        }
    }
    mi.ok = true;
    return mi;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------
static int even(double v) {
    int i = (int)std::llround(v);
    if (i < 2) i = 2;
    return i - (i % 2);
}

struct Plan {
    int    width, height;
    bool   scale;
    int    video_kbps;
    int    audio_kbps;
    double fps_used;
    bool   low_quality_warning;
};

static Plan make_plan(const MediaInfo& mi, uint64_t target_bytes, const EncodeOptions& opts) {
    Plan p{};
    const double usable_bits = double(target_bytes) * 0.98 * 8.0; // 2% muxing headroom
    const double dur = mi.duration;

    double fps = mi.fps > 0 ? mi.fps : 30.0;
    if (opts.fps_cap > 0 && fps > opts.fps_cap) fps = opts.fps_cap;
    p.fps_used = fps;

    // Audio budget: cap around 18% of the total, clamp to sane range.
    int audio_kbps = 0;
    if (mi.has_audio) {
        if (opts.audio_kbps > 0) {
            audio_kbps = opts.audio_kbps;
        } else {
            double a = std::min(128.0, usable_bits * 0.18 / dur / 1000.0);
            audio_kbps = (int)std::clamp(a, 32.0, 160.0);
        }
    }

    double total_kbps = usable_bits / dur / 1000.0;
    double video_kbps = total_kbps - audio_kbps;

    // If starved, trim audio before giving up on video quality.
    if (video_kbps < 100 && mi.has_audio && opts.audio_kbps == 0) {
        audio_kbps = 32;
        video_kbps = total_kbps - audio_kbps;
    }
    p.low_quality_warning = video_kbps < 150;
    if (video_kbps < 40) video_kbps = 40; // hard floor; verify loop guards size
    p.audio_kbps = audio_kbps;
    p.video_kbps = (int)std::llround(video_kbps);

    // Resolution ladder: keep the largest height whose bits-per-pixel clears
    // the quality bar for the chosen codec.
    double bpp_target = opts.codec == Codec::H265 ? 0.028
                      : opts.codec == Codec::AV1  ? 0.024
                                                  : 0.050;
    int max_h = mi.height;
    if (opts.max_height > 0) max_h = std::min(max_h, opts.max_height);

    const int ladder[] = {4320, 2160, 1440, 1080, 900, 720, 600, 480, 360, 270, 180};
    int chosen_h = 0, chosen_w = 0;
    double video_bps = video_kbps * 1000.0;
    for (int h : ladder) {
        if (h > max_h) continue;
        int w = even(double(mi.width) * h / mi.height);
        double bpp = video_bps / (double(w) * h * fps);
        if (bpp >= bpp_target) { chosen_h = h; chosen_w = w; break; }
    }
    if (chosen_h == 0) {
        // Nothing clears the bar: use the smallest sane height allowed.
        for (int i = (int)(sizeof(ladder) / sizeof(ladder[0])) - 1; i >= 0; --i) {
            if (ladder[i] <= max_h) {
                chosen_h = ladder[i];
                chosen_w = even(double(mi.width) * chosen_h / mi.height);
                break;
            }
        }
        if (chosen_h == 0) { chosen_h = max_h; chosen_w = mi.width; }
    }

    if (chosen_h >= mi.height) { // never upscale
        p.width = mi.width; p.height = mi.height; p.scale = false;
    } else {
        p.width = chosen_w; p.height = chosen_h; p.scale = true;
    }
    return p;
}

static std::wstring codec_lib(Codec c) {
    switch (c) {
        case Codec::H265: return L"libx265";
        case Codec::AV1:  return L"libsvtav1";
        default:          return L"libx264";
    }
}

// Assemble one ffmpeg pass command.
static std::wstring build_pass(const std::wstring& in, const std::wstring& out,
                               const Plan& p, const EncodeOptions& opts,
                               bool has_audio, int pass, const std::wstring& passlog) {
    std::wstringstream a;
    a << L"-y -hide_banner -nostdin -progress pipe:1 -nostats -i " << quote(in) << L" ";
    if (p.scale)
        a << L"-vf scale=" << p.width << L":" << p.height << L":flags=lanczos ";
    a << L"-c:v " << codec_lib(opts.codec) << L" -b:v " << p.video_kbps << L"k ";
    if (opts.codec == Codec::AV1)
        a << L"-preset " << opts.preset_svt << L" ";
    else
        a << L"-preset " << std::wstring(opts.preset_x.begin(), opts.preset_x.end()) << L" ";
    a << L"-pass " << pass << L" -passlogfile " << quote(passlog) << L" ";
    if (opts.fps_cap > 0) a << L"-r " << opts.fps_cap << L" ";

    if (pass == 1) {
        a << L"-an -f null NUL";
    } else {
        if (has_audio)
            a << L"-c:a aac -b:a " << p.audio_kbps << L"k -ac 2 ";
        else
            a << L"-an ";
        if (opts.codec == Codec::H265) a << L"-tag:v hvc1 ";
        a << L"-movflags +faststart " << quote(out);
    }
    return a.str();
}

// Parse "out_time=HH:MM:SS.microseconds" -> seconds. Returns -1 if absent.
static double parse_out_time(const std::string& line) {
    const std::string key = "out_time=";
    if (line.compare(0, key.size(), key) != 0) return -1;
    std::string v = line.substr(key.size());
    if (v.find("N/A") != std::string::npos) return 0;
    int hh = 0, mm = 0; double ss = 0;
    if (sscanf(v.c_str(), "%d:%d:%lf", &hh, &mm, &ss) == 3)
        return hh * 3600.0 + mm * 60.0 + ss;
    return -1;
}

static std::wstring unique_out(const std::wstring& dir, const std::wstring& stem,
                               const std::wstring& ext) {
    std::wstring base = dir + stem + L"_discord";
    std::wstring cand = base + ext;
    int i = 1;
    while (file_exists(cand)) cand = base + L"_" + std::to_wstring(i++) + ext;
    return cand;
}

static void cleanup_passlogs(const std::wstring& passlog) {
    // ffmpeg/x265 create a handful of files based on this prefix.
    const wchar_t* suffixes[] = {L"-0.log", L"-0.log.mbtree", L"", L".cutree",
                                 L".log", L".log.cutree", L".stats", L".stats.cutree"};
    for (auto s : suffixes) DeleteFileW((passlog + s).c_str());
}

// -------- Video --------
static CompressResult compress_video(const std::wstring& in, const MediaInfo& mi,
                                     uint64_t target_bytes, const EncodeOptions& opts,
                                     const std::function<void(const ProgressInfo&)>& on_progress,
                                     std::atomic<bool>& cancel) {
    CompressResult res;
    uint64_t in_size = file_size_of(in);

    std::wstring outdir = opts.out_dir.empty() ? dir_of(in) : opts.out_dir;
    if (!outdir.empty() && outdir.back() != L'\\' && outdir.back() != L'/') outdir += L'\\';
    CreateDirectoryW(outdir.c_str(), nullptr);

    // Already fits: copy as-is, preserving original quality.
    if (in_size > 0 && in_size <= target_bytes) {
        std::wstring ext = L"." + ext_lower(in);
        std::wstring out = unique_out(outdir, stem_of(in), ext);
        if (CopyFileW(in.c_str(), out.c_str(), FALSE)) {
            res.ok = true; res.out_path = out; res.out_size = file_size_of(out);
            res.message = "Already under the limit - copied without re-encoding.";
            return res;
        }
    }

    Plan plan = make_plan(mi, target_bytes, opts);
    std::wstring out = unique_out(outdir, stem_of(in), L".mp4");
    std::wstring passlog = make_passlog();

    auto report = [&](int pass, double sec, const char* stage) {
        double frac_pass = mi.duration > 0 ? std::clamp(sec / mi.duration, 0.0, 1.0) : 0.0;
        ProgressInfo pi;
        pi.fraction = float((pass == 1 ? 0.0 : 0.5) + frac_pass * 0.5);
        pi.stage = stage;
        on_progress(pi);
    };

    const int MAX_TRIES = 3;
    std::string tail;
    for (int attempt = 0; attempt < MAX_TRIES; ++attempt) {
        // Pass 1 (only needed once; stats stay valid across bitrate changes).
        if (attempt == 0) {
            std::wstring c1 = build_pass(in, out, plan, opts, mi.has_audio, 1, passlog);
            int rc = proc::run_streaming(ffmpeg(), c1,
                [&](const std::string& ln) { double s = parse_out_time(ln);
                    if (s >= 0) report(1, s, "Pass 1 of 2  -  analyzing"); },
                cancel, &tail);
            if (rc == -2) { res.message = "Cancelled."; cleanup_passlogs(passlog); return res; }
            if (rc != 0) {
                res.message = "Encoding failed (pass 1).\n" + tail;
                cleanup_passlogs(passlog); return res;
            }
        }

        // Pass 2.
        std::wstring c2 = build_pass(in, out, plan, opts, mi.has_audio, 2, passlog);
        int rc = proc::run_streaming(ffmpeg(), c2,
            [&](const std::string& ln) { double s = parse_out_time(ln);
                if (s >= 0) report(2, s, "Pass 2 of 2  -  encoding"); },
            cancel, &tail);
        if (rc == -2) { res.message = "Cancelled."; DeleteFileW(out.c_str()); cleanup_passlogs(passlog); return res; }
        if (rc != 0) {
            res.message = "Encoding failed (pass 2).\n" + tail;
            DeleteFileW(out.c_str()); cleanup_passlogs(passlog); return res;
        }

        uint64_t sz = file_size_of(out);
        if (sz <= target_bytes || attempt == MAX_TRIES - 1) {
            cleanup_passlogs(passlog);
            res.ok = true; res.out_path = out; res.out_size = sz;
            double ratio = in_size ? (double)sz / in_size * 100.0 : 0.0;
            std::wstringstream m;
            m << plan.width << L"x" << plan.height;
            res.message = "Encoded at " + wide_to_utf8(m.str()) +
                          (plan.low_quality_warning ? " (tight budget - quality reduced)" : "") +
                          " - " + std::to_string((int)ratio) + "% of original.";
            return res;
        }

        // Overshot: scale the video bitrate down and re-run pass 2 only.
        double corr = double(target_bytes) * 0.97 / double(sz);
        plan.video_kbps = std::max(40, (int)std::llround(plan.video_kbps * corr));
    }

    cleanup_passlogs(passlog);
    res.message = "Could not reach target size.";
    return res;
}

// -------- Image --------
static bool encode_webp(const std::wstring& in, const std::wstring& out, int quality,
                        double scale, std::atomic<bool>& cancel) {
    std::wstringstream a;
    a << L"-y -hide_banner -nostdin -i " << quote(in) << L" ";
    if (scale < 0.999)
        a << L"-vf scale=iw*" << scale << L":-2 ";
    a << L"-c:v libwebp -quality " << quality << L" -compression_level 6 -map_metadata -1 "
      << quote(out);
    std::string tail;
    int rc = proc::run_streaming(ffmpeg(), a.str(), [](const std::string&) {}, cancel, &tail);
    return rc == 0;
}

static CompressResult compress_image(const std::wstring& in, uint64_t target_bytes,
                                     const EncodeOptions& opts,
                                     const std::function<void(const ProgressInfo&)>& on_progress,
                                     std::atomic<bool>& cancel) {
    CompressResult res;
    uint64_t in_size = file_size_of(in);
    std::wstring outdir = opts.out_dir.empty() ? dir_of(in) : opts.out_dir;
    if (!outdir.empty() && outdir.back() != L'\\' && outdir.back() != L'/') outdir += L'\\';
    CreateDirectoryW(outdir.c_str(), nullptr);

    std::wstring e = ext_lower(in);
    bool web_friendly = (e == L"jpg" || e == L"jpeg" || e == L"png" || e == L"webp");
    if (in_size > 0 && in_size <= target_bytes && web_friendly) {
        std::wstring out = unique_out(outdir, stem_of(in), L"." + e);
        if (CopyFileW(in.c_str(), out.c_str(), FALSE)) {
            res.ok = true; res.out_path = out; res.out_size = file_size_of(out);
            res.message = "Already under the limit - copied as-is.";
            return res;
        }
    }

    std::wstring out = unique_out(outdir, stem_of(in), L".webp");
    std::wstring tmp = out + L".tmp.webp";

    auto tick = [&](float f, const char* s) { on_progress(ProgressInfo{f, s}); };

    // Binary-search WebP quality at full resolution.
    int lo = 8, hi = 95, best_q = -1;
    for (int it = 0; it < 8 && lo <= hi; ++it) {
        if (cancel.load()) { res.message = "Cancelled."; DeleteFileW(tmp.c_str()); return res; }
        int q = (lo + hi) / 2;
        tick(0.1f + it * 0.1f, "Optimizing image");
        if (!encode_webp(in, tmp, q, 1.0, cancel)) { res.message = "Image encode failed."; DeleteFileW(tmp.c_str()); return res; }
        uint64_t sz = file_size_of(tmp);
        if (sz <= target_bytes) { best_q = q; lo = q + 1; }
        else hi = q - 1;
    }

    double scale = 1.0;
    if (best_q < 0) {
        // Even the lowest quality is too big: shrink until it fits.
        for (int i = 0; i < 6 && best_q < 0; ++i) {
            if (cancel.load()) { res.message = "Cancelled."; DeleteFileW(tmp.c_str()); return res; }
            scale *= 0.75;
            tick(0.8f, "Resizing image");
            if (!encode_webp(in, tmp, 80, scale, cancel)) break;
            if (file_size_of(tmp) <= target_bytes) best_q = 80;
        }
    }

    if (best_q < 0) { DeleteFileW(tmp.c_str()); res.message = "Could not fit image under target."; return res; }

    tick(0.95f, "Finalizing");
    if (!encode_webp(in, out, best_q, scale, cancel)) { res.message = "Image encode failed."; return res; }
    DeleteFileW(tmp.c_str());
    res.ok = true; res.out_path = out; res.out_size = file_size_of(out);
    res.message = "WebP quality " + std::to_string(best_q) +
                  (scale < 0.999 ? " (resized)" : "") + ".";
    return res;
}

CompressResult compress_file(const std::wstring& in_path, const MediaInfo& info,
                             uint64_t target_bytes, const EncodeOptions& opts,
                             const std::function<void(const ProgressInfo&)>& on_progress,
                             std::atomic<bool>& cancel) {
    if (info.is_image)
        return compress_image(in_path, target_bytes, opts, on_progress, cancel);
    return compress_video(in_path, info, target_bytes, opts, on_progress, cancel);
}
