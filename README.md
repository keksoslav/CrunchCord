# CrunchCord

A small Windows GUI tool that compresses images and videos to fit under
Discord's upload limit — spending the whole size budget on quality instead of
overshooting and forcing you to guess export settings by hand.

![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-blue)
![Language](https://img.shields.io/badge/C%2B%2B-17-00599C)

## What it does

- **Drag & drop** any number of images or videos onto the window.
- Pick a **target size** (Discord Free 10 MB, Nitro Basic 50 MB, Nitro 500 MB,
  or a custom value).
- It computes the exact bitrate that fills the budget and encodes to land
  **just under** the limit, verifying the real output size and retrying if it
  overshoots.
- Videos use **two-pass** encoding (H.265 by default; AV1 / H.264 available)
  with an automatic **resolution ladder** — if the budget is too small for the
  source resolution it steps down (1080p → 720p → 480p …) so the remaining bits
  look clean instead of blocky.
- Images are re-encoded to **WebP** with a quality search (and downscale only if
  strictly necessary).
- Output goes next to each source file as `name_discord.mp4` / `.webp`
  (or a folder you choose).

## Requirements

- Windows 10/11 (x64)
- **FFmpeg** — `ffmpeg.exe` and `ffprobe.exe` must be on your `PATH`, or copied
  next to `CrunchCord.exe`. (e.g. `winget install Gyan.FFmpeg`)

## Build

```powershell
cmake -S . -B build
cmake --build build
```

The executable lands at `build/CrunchCord.exe`. Dear ImGui is fetched
automatically; the GUI uses the Win32 + DirectX 11 backend, so no other
libraries are required beyond the Windows SDK. The MinGW build is statically
linked, so the resulting `.exe` is standalone.

## How it works

| Stage | What happens |
|-------|--------------|
| Probe | `ffprobe` reads duration, resolution, fps, audio |
| Plan  | target bitrate = `(size × 8) / duration` − audio − 2% muxing headroom |
| Ladder | pick the largest resolution whose bits-per-pixel clears the quality bar |
| Encode | two-pass `libx265` / `libsvtav1` / `libx264` |
| Verify | measure real output; if over, drop bitrate and re-run pass 2 |

## Notes

- H.265/AV1 make the smallest files but occasionally won't inline-preview on
  some Discord clients (you'd click to download). Switch to **H.264** in the
  Advanced panel for maximum compatibility.
- Size targets are computed against a decimal MB (1,000,000 bytes) with a small
  safety margin, so uploads stay under the limit regardless of how Discord
  rounds.

## License

MIT — see [LICENSE](LICENSE).
