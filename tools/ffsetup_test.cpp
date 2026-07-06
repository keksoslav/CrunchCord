// Console harness for the FFmpeg auto-setup module (real download).
#include "ffmpeg_setup.h"
#include <cstdio>
#include <atomic>

int wmain() {
    printf("present (before): %d\n", ffsetup::present());
    std::atomic<bool> cancel{false};
    std::string err = ffsetup::install([](const ffsetup::Progress& p) {
        printf("\r%3.0f%%  %-42s", p.fraction * 100.0, p.stage.c_str());
        fflush(stdout);
    }, cancel);
    printf("\n");
    if (err.empty()) printf("install: OK\n");
    else             printf("install ERROR: %s\n", err.c_str());
    printf("present (after): %d\n", ffsetup::present());
    return 0;
}
