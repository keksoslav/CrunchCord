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
#include <mutex>

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

std::vector<std::wstring> supported_extensions() {
    return {
        L"jpg", L"jpeg", L"png", L"bmp", L"tif", L"tiff", L"gif", L"webp", L"apng",
        L"mp4", L"mov", L"mkv", L"avi", L"webm", L"m4v", L"flv", L"wmv", L"mpg",
        L"mpeg", L"ts", L"m2ts", L"mts", L"3gp", L"ogv", L"vob", L"m2v", L"divx"
    };
}
bool is_supported_media(const std::wstring& path) {
    std::wstring e = ext_lower(path);
    for (auto& x : supported_extensions()) if (e == x) return true;
    return false;
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

static std::wstring make_passlog() {
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    std::wstring dir(tmp, n);
    static long counter = 0;
    long id = InterlockedIncrement(&counter);
    std::wstringstream ss;
    ss << dir << L"ccz_" << GetCurrentProcessId() << L"_" << id;
    return ss.str();
}

// ---------------------------------------------------------------------------
// Hardware (NVENC) detection
// ---------------------------------------------------------------------------
static bool test_encoder(const std::wstring& enc) {
    std::wstring args = L"-hide_banner -loglevel error -f lavfi "
                        L"-i color=c=black:s=256x256:d=1 -c:v " + enc + L" -f null -";
    auto r = proc::run_capture(ffmpeg(), args);
    return r.launched && r.exit_code == 0;
}

static HwCaps       g_caps;
static std::once_flag g_caps_once;

HwCaps detect_hw_caps() {
    std::call_once(g_caps_once, [] {
        g_caps.hevc = test_encoder(L"hevc_nvenc");
        g_caps.h264 = test_encoder(L"h264_nvenc");
        g_caps.av1  = test_encoder(L"av1_nvenc");
        g_caps.checked = true;
    });
    return g_caps;
}

bool hw_available_for(Codec c) {
    HwCaps caps = detect_hw_caps();
    switch (c) {
        case Codec::H265: return caps.hevc;
        case Codec::H264: return caps.h264;
        case Codec::AV1:  return caps.av1;
    }
    return false;
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
    size_t pos = 0;
    while ((pos = text.find(key + "=", pos)) != std::string::npos) {
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

    std::wstring aargs = L"-v error -select_streams a -show_entries stream=codec_name "
                         L"-of default=noprint_wrappers=1:nokey=1 " + quote(path);
    auto ar = proc::run_capture(ffprobe(), aargs);
    mi.has_audio = ar.launched && ar.output.find_first_not_of(" \r\n\t") != std::string::npos;

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
        if (mi.duration <= 0.0) { mi.err = "Could not determine video duration"; return mi; }
        if (mi.width <= 0 || mi.height <= 0) { mi.err = "Could not read video dimensions"; return mi; }
    }
    mi.ok = true;
    return mi;
}

// ---------------------------------------------------------------------------
// Planning
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

    if (video_kbps < 100 && mi.has_audio && opts.audio_kbps == 0) {
        audio_kbps = 32;
        video_kbps = total_kbps - audio_kbps;
    }
    p.low_quality_warning = video_kbps < 150;
    if (video_kbps < 40) video_kbps = 40;
    p.audio_kbps = audio_kbps;
    p.video_kbps = (int)std::llround(video_kbps);

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
        for (int i = (int)(sizeof(ladder) / sizeof(ladder[0])) - 1; i >= 0; --i) {
            if (ladder[i] <= max_h) {
                chosen_h = ladder[i];
                chosen_w = even(double(mi.width) * chosen_h / mi.height);
                break;
            }
        }
        if (chosen_h == 0) { chosen_h = max_h; chosen_w = mi.width; }
    }

    if (chosen_h >= mi.height) { p.width = mi.width; p.height = mi.height; p.scale = false; }
    else                       { p.width = chosen_w; p.height = chosen_h; p.scale = true; }
    return p;
}

// ---------------------------------------------------------------------------
// Encoder command building
// ---------------------------------------------------------------------------
static std::wstring encoder_name(Codec c, bool hw) {
    if (hw) {
        switch (c) {
            case Codec::H265: return L"hevc_nvenc";
            case Codec::H264: return L"h264_nvenc";
            case Codec::AV1:  return L"av1_nvenc";
        }
    }
    switch (c) {
        case Codec::H265: return L"libx265";
        case Codec::AV1:  return L"libsvtav1";
        default:          return L"libx264";
    }
    return L"libx264";
}

// Software preset for the chosen speed (numeric for SVT-AV1, named otherwise).
static std::wstring sw_preset(Codec c, Speed s) {
    if (c == Codec::AV1) {
        int p = s == Speed::Fastest ? 10 : s == Speed::Balanced ? 8 : 5;
        return std::to_wstring(p);
    }
    switch (s) {
        case Speed::Fastest:  return L"veryfast";
        case Speed::Balanced: return L"fast";
        default:              return L"slow";
    }
}
static std::wstring nv_preset(Speed s) {
    switch (s) { case Speed::Fastest: return L"p3"; case Speed::Balanced: return L"p5"; default: return L"p7"; }
}
static std::wstring nv_multipass(Speed s) {
    switch (s) { case Speed::Fastest: return L"disabled"; case Speed::Balanced: return L"qres"; default: return L"fullres"; }
}

static std::wstring build_input(const std::wstring& in, const Plan& p, const EncodeOptions& opts) {
    std::wstringstream a;
    a << L"-y -hide_banner -nostdin -progress pipe:1 -nostats -i " << quote(in) << L" ";
    if (p.scale)
        a << L"-vf scale=" << p.width << L":" << p.height << L":flags=lanczos ";
    if (opts.fps_cap > 0) a << L"-r " << opts.fps_cap << L" ";
    return a.str();
}
static std::wstring audio_args(const Plan& p, bool has_audio) {
    if (has_audio) return L"-c:a aac -b:a " + std::to_wstring(p.audio_kbps) + L"k -ac 2 ";
    return L"-an ";
}

// Software two-pass (best quality).
static std::wstring build_sw2(const std::wstring& in, const std::wstring& out, const Plan& p,
                              const EncodeOptions& opts, bool has_audio, int pass,
                              const std::wstring& passlog) {
    std::wstringstream a;
    a << build_input(in, p, opts);
    a << L"-c:v " << encoder_name(opts.codec, false) << L" -b:v " << p.video_kbps << L"k ";
    a << L"-preset " << sw_preset(opts.codec, opts.speed) << L" ";
    a << L"-pass " << pass << L" -passlogfile " << quote(passlog) << L" ";
    if (pass == 1) {
        a << L"-an -f null NUL";
    } else {
        a << audio_args(p, has_audio);
        if (opts.codec == Codec::H265) a << L"-tag:v hvc1 ";
        a << L"-movflags +faststart " << quote(out);
    }
    return a.str();
}

// Software single-pass (capped VBR) - roughly twice as fast as two-pass.
static std::wstring build_sw1(const std::wstring& in, const std::wstring& out, const Plan& p,
                              const EncodeOptions& opts, bool has_audio) {
    std::wstringstream a;
    a << build_input(in, p, opts);
    a << L"-c:v " << encoder_name(opts.codec, false) << L" -b:v " << p.video_kbps << L"k ";
    a << L"-preset " << sw_preset(opts.codec, opts.speed) << L" ";
    if (opts.codec != Codec::AV1) {
        int maxr = (int)(p.video_kbps * 1.3);
        int buf  = p.video_kbps * 2;
        a << L"-maxrate " << maxr << L"k -bufsize " << buf << L"k ";
    }
    a << audio_args(p, has_audio);
    if (opts.codec == Codec::H265) a << L"-tag:v hvc1 ";
    a << L"-movflags +faststart " << quote(out);
    return a.str();
}

// Hardware NVENC, single invocation (multipass handled on the GPU).
static std::wstring build_hw(const std::wstring& in, const std::wstring& out, const Plan& p,
                             const EncodeOptions& opts, bool has_audio) {
    std::wstringstream a;
    a << build_input(in, p, opts);
    int maxr = (int)(p.video_kbps * 1.35);
    int buf  = p.video_kbps * 2;
    a << L"-c:v " << encoder_name(opts.codec, true)
      << L" -preset " << nv_preset(opts.speed)
      << L" -tune hq -rc vbr -multipass " << nv_multipass(opts.speed)
      << L" -b:v " << p.video_kbps << L"k -maxrate " << maxr << L"k -bufsize " << buf << L"k ";
    a << audio_args(p, has_audio);
    if (opts.codec == Codec::H265) a << L"-tag:v hvc1 ";
    a << L"-movflags +faststart " << quote(out);
    return a.str();
}

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
    const wchar_t* suffixes[] = {L"-0.log", L"-0.log.mbtree", L"", L".cutree",
                                 L".log", L".log.cutree", L".stats", L".stats.cutree"};
    for (auto s : suffixes) DeleteFileW((passlog + s).c_str());
}

// ---------------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------------
static CompressResult compress_video(const std::wstring& in, const MediaInfo& mi,
                                     uint64_t target_bytes, const EncodeOptions& opts,
                                     const std::function<void(const ProgressInfo&)>& on_progress,
                                     std::atomic<bool>& cancel) {
    CompressResult res;
    uint64_t in_size = file_size_of(in);

    std::wstring outdir = opts.out_dir.empty() ? dir_of(in) : opts.out_dir;
    if (!outdir.empty() && outdir.back() != L'\\' && outdir.back() != L'/') outdir += L'\\';
    CreateDirectoryW(outdir.c_str(), nullptr);

    if (in_size > 0 && in_size <= target_bytes) {
        std::wstring ext = L"." + ext_lower(in);
        std::wstring out = unique_out(outdir, stem_of(in), ext);
        if (CopyFileW(in.c_str(), out.c_str(), FALSE)) {
            res.ok = true; res.out_path = out; res.out_size = file_size_of(out);
            res.message = "Already under the limit, copied without re-encoding.";
            return res;
        }
    }

    Plan plan = make_plan(mi, target_bytes, opts);
    std::wstring out = unique_out(outdir, stem_of(in), L".mp4");
    std::wstring passlog = make_passlog();

    bool hw = opts.use_hardware && hw_available_for(opts.codec);
    bool two_pass = (!hw && opts.speed == Speed::Best);
    int total_stages = two_pass ? 2 : 1;

    auto report = [&](int stage_idx, double sec, const char* stage) {
        double frac = mi.duration > 0 ? std::clamp(sec / mi.duration, 0.0, 1.0) : 0.0;
        ProgressInfo pi;
        pi.fraction = float((double)stage_idx / total_stages + frac / total_stages);
        pi.stage = stage;
        on_progress(pi);
    };

    std::string tail;
    bool pass1_done = false;
    const int MAX_TRIES = 3;

    for (int attempt = 0; attempt < MAX_TRIES; ++attempt) {
        if (two_pass) {
            if (!pass1_done) {
                std::wstring c1 = build_sw2(in, out, plan, opts, mi.has_audio, 1, passlog);
                int rc = proc::run_streaming(ffmpeg(), c1, [&](const std::string& ln) {
                    double s = parse_out_time(ln); if (s >= 0) report(0, s, "Analyzing (pass 1 of 2)"); },
                    cancel, &tail);
                if (rc == -2) { res.message = "Cancelled."; cleanup_passlogs(passlog); return res; }
                if (rc != 0) { res.message = "Encoding failed (pass 1).\n" + tail; cleanup_passlogs(passlog); return res; }
                pass1_done = true;
            }
            std::wstring c2 = build_sw2(in, out, plan, opts, mi.has_audio, 2, passlog);
            int rc = proc::run_streaming(ffmpeg(), c2, [&](const std::string& ln) {
                double s = parse_out_time(ln); if (s >= 0) report(1, s, "Encoding (pass 2 of 2)"); },
                cancel, &tail);
            if (rc == -2) { res.message = "Cancelled."; DeleteFileW(out.c_str()); cleanup_passlogs(passlog); return res; }
            if (rc != 0) { res.message = "Encoding failed (pass 2).\n" + tail; DeleteFileW(out.c_str()); cleanup_passlogs(passlog); return res; }
        } else {
            std::wstring cmd = hw ? build_hw(in, out, plan, opts, mi.has_audio)
                                  : build_sw1(in, out, plan, opts, mi.has_audio);
            const char* stage = hw ? "Encoding on GPU" : "Encoding";
            int rc = proc::run_streaming(ffmpeg(), cmd, [&](const std::string& ln) {
                double s = parse_out_time(ln); if (s >= 0) report(0, s, stage); },
                cancel, &tail);
            if (rc == -2) { res.message = "Cancelled."; DeleteFileW(out.c_str()); return res; }
            if (rc != 0) { res.message = std::string("Encoding failed.\n") + tail; DeleteFileW(out.c_str()); return res; }
        }

        uint64_t sz = file_size_of(out);
        if (sz <= target_bytes || attempt == MAX_TRIES - 1) {
            cleanup_passlogs(passlog);
            res.ok = true; res.out_path = out; res.out_size = sz; res.used_hardware = hw;
            double ratio = in_size ? (double)sz / in_size * 100.0 : 0.0;
            std::wstringstream m; m << plan.width << L"x" << plan.height;
            // Encoder (GPU/CPU) is shown as a badge in the UI, so keep this concise.
            res.message = wide_to_utf8(m.str()) +
                          (plan.low_quality_warning ? " (tight budget, quality reduced)" : "") +
                          ", " + std::to_string((int)ratio) + "% of original.";
            return res;
        }

        double corr = double(target_bytes) * 0.97 / double(sz);
        plan.video_kbps = std::max(40, (int)std::llround(plan.video_kbps * corr));
    }

    cleanup_passlogs(passlog);
    res.message = "Could not reach target size.";
    return res;
}

// ---------------------------------------------------------------------------
// Image
// ---------------------------------------------------------------------------
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
            res.message = "Already under the limit, copied as-is.";
            return res;
        }
    }

    std::wstring out = unique_out(outdir, stem_of(in), L".webp");
    std::wstring tmp = out + L".tmp.webp";
    auto tick = [&](float f, const char* s) { on_progress(ProgressInfo{f, s}); };

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
    res.message = "WebP quality " + std::to_string(best_q) + (scale < 0.999 ? " (resized)" : "") + ".";
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
