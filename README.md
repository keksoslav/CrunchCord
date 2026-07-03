# CrunchCord

A small Windows GUI tool that compresses images and videos to fit under
Discord's upload limit. It spends the whole size budget on quality instead of
overshooting and making you guess export settings by hand.

![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-blue)
![Language](https://img.shields.io/badge/C%2B%2B-17-00599C)

## What it does

- Add files by **drag & drop**, an **Add files** picker, **Add folder** to
  import a whole folder (optionally including subfolders), or **paste** (Ctrl+V)
  a screenshot or copied files straight in.
- Pick a **target size**: Discord Free 10 MB, Nitro Basic 50 MB, Nitro 500 MB,
  or a custom value.
- Choose where results go: next to each source file, or a folder you pick.
- It works out the exact bitrate that fills the budget, encodes to land just
  under the limit, checks the real output size, and retries at a lower bitrate
  if it overshot. The result is reliably under the cap.
- An automatic **resolution ladder** steps the video down (1080p, 720p, 480p and
  so on) when the budget is too small for the source resolution, so the bits
  that remain look clean instead of blocky.
- Images are re-encoded to **WebP** with a quality search, downscaling only if
  it has to.
- Output is named `name_discord.mp4` or `name_discord.webp`.
- When a job finishes, the file can go **straight to your clipboard** (either
  automatically, or with the Copy button) so you paste it into Discord with
  Ctrl+V, no hunting for the file.

## Speed

Encoding uses your **NVIDIA GPU (NVENC)** automatically when it is available,
which is far faster than encoding on the CPU. If no GPU encoder is available it
falls back to the CPU, and the default "Balanced" mode uses a single fast pass
rather than two passes. There are three speed settings in the Advanced panel:
Fastest, Balanced, and Best quality.

GPU encoding needs a reasonably recent NVIDIA driver (FFmpeg 8.x requires driver
version 570 or newer). If the Advanced panel shows "no GPU encoder", update your
driver and it will switch to the GPU on the next run.

## Requirements

- Windows 10/11 (x64)
- **FFmpeg**: `ffmpeg.exe` and `ffprobe.exe` on your `PATH`, or copied next to
  `CrunchCord.exe`. Install with `winget install Gyan.FFmpeg`.
- Optional: an NVIDIA GPU with driver 570+ for hardware encoding.

## Build

```powershell
cmake -S . -B build
cmake --build build
```

The executable lands at `build/CrunchCord.exe`. Dear ImGui is fetched
automatically, and the GUI uses the Win32 + DirectX 11 backend, so nothing is
required beyond the Windows SDK. The MinGW build is statically linked, so the
resulting `.exe` runs standalone.

## How it works

| Stage  | What happens |
|--------|--------------|
| Probe  | `ffprobe` reads duration, resolution, fps, and audio |
| Plan   | video bitrate = target size in bits / duration, minus audio and a 2% muxing margin |
| Ladder | pick the largest resolution whose bits-per-pixel clears the quality bar |
| Encode | `hevc_nvenc` / `av1_nvenc` / `h264_nvenc` on the GPU, or `libx265` / `libsvtav1` / `libx264` on the CPU |
| Verify | measure the real output; if it is over, lower the bitrate and re-run |

## Notes

- H.265 and AV1 make the smallest files, but a few Discord clients will not show
  an inline preview for them (you click to download instead). Switch to H.264 in
  the Advanced panel for maximum compatibility.
- Size targets use a decimal MB (1,000,000 bytes) with a small safety margin, so
  uploads stay under the limit no matter how Discord rounds.

## License

MIT, see [LICENSE](LICENSE).
