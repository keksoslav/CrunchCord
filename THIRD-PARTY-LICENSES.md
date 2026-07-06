# Third-party licenses

## Dear ImGui

CrunchCord's interface is built with Dear ImGui, which is compiled into
`CrunchCord.exe`.

Copyright (c) 2014-2024 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## FFmpeg

CrunchCord does **not** bundle FFmpeg. It uses an FFmpeg installation found on
your system, or, if you use the in-app download, it fetches an official build
from gyan.dev at runtime. Those builds are licensed under the GNU GPL (they
include x264 and x265). CrunchCord invokes FFmpeg as a separate program and is
not a derivative work of it.

FFmpeg source and build details:
- https://www.gyan.dev/ffmpeg/builds/
- https://ffmpeg.org/
