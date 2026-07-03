// Console harness that exercises the real compression engine (no GUI).
// Usage: cli_test <file> <target_MB> [codec: h265|av1|h264]
#include "compressor.h"
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { wprintf(L"usage: cli_test <file> <MB> [h265|av1|h264]\n"); return 1; }
    std::wstring path = argv[1];
    double mb = _wtof(argv[2]);
    uint64_t tb = (uint64_t)(mb * 1000.0 * 1000.0);

    EncodeOptions opts;
    if (argc >= 4) {
        std::wstring c = argv[3];
        if (c == L"av1") opts.codec = Codec::AV1;
        else if (c == L"h264") opts.codec = Codec::H264;
        else opts.codec = Codec::H265;
    }

    MediaInfo mi = probe_media(path);
    printf("probe: ok=%d image=%d %dx%d dur=%.2fs fps=%.2f audio=%d codec=%s%s%s\n",
           mi.ok, mi.is_image, mi.width, mi.height, mi.duration, mi.fps,
           mi.has_audio, mi.vcodec.c_str(),
           mi.err.empty() ? "" : " err=", mi.err.c_str());
    if (!mi.ok) return 2;

    uint64_t in_size = file_size_of(path);
    printf("input size: %.2f MB   target: %.2f MB\n", in_size / 1e6, mb);

    std::atomic<bool> cancel{false};
    auto r = compress_file(path, mi, tb, opts,
        [](const ProgressInfo& pi) {
            printf("\r  %3.0f%%  %-32s", pi.fraction * 100.0, pi.stage.c_str());
            fflush(stdout);
        }, cancel);
    printf("\n");

    printf("result: ok=%d  out=%.2f MB  under_target=%d\n",
           r.ok, r.out_size / 1e6, (r.out_size <= tb));
    printf("message: %s\n", r.message.c_str());
    wprintf(L"output: %ls\n", r.out_path.c_str());
    return r.ok ? 0 : 3;
}
