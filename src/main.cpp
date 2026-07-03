// CrunchCord - Dear ImGui (Win32 + DirectX 11) front end.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <d3d11.h>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include <algorithm>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "compressor.h"
#include "process.h"
#include "dialogs.h"
#include "clipboard.h"
#include "shellreg.h"

// ---------------------------------------------------------------------------
// Shared application state
// ---------------------------------------------------------------------------
enum class JobState { Queued, Probing, Running, Done, Failed, Cancelled };

struct Job {
    std::wstring          path;
    std::string           name;
    uint64_t              in_size = 0;
    MediaInfo             info;
    std::atomic<float>    progress{0.f};
    std::atomic<JobState> state{JobState::Probing};
    std::string           stage;       // guarded by g_ui_mtx
    std::string           result_msg;  // guarded by g_ui_mtx
    std::wstring          out_path;
    uint64_t              out_size = 0;
    bool                  copied = false; // guarded by g_ui_mtx (auto-copy bookkeeping)
};

static std::vector<std::unique_ptr<Job>> g_jobs;
static std::mutex                        g_ui_mtx;

static std::vector<std::wstring> g_incoming;
static std::mutex                g_incoming_mtx;

static std::atomic<bool> g_cancel{false};
static std::atomic<bool> g_worker_running{false};
static std::atomic<bool> g_app_quit{false};

static HWND              g_hwnd = nullptr;
static std::atomic<bool> g_hw_ready{false};
static HwCaps            g_hw;

static void enqueue_paths(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    std::lock_guard<std::mutex> lk(g_incoming_mtx);
    for (auto& p : paths) g_incoming.push_back(p);
}

// Expand any folders to their media files, drop unsupported files, then enqueue.
static void add_paths_expanding(const std::vector<std::wstring>& in) {
    std::vector<std::wstring> out;
    for (auto& path : in) {
        DWORD attr = GetFileAttributesW(path.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            auto media = dlg::enum_media(path, true);
            out.insert(out.end(), media.begin(), media.end());
        } else if (is_supported_media(path)) {
            out.push_back(path);
        }
    }
    enqueue_paths(out);
}

static void set_stage(Job* j, const std::string& s)  { std::lock_guard<std::mutex> lk(g_ui_mtx); j->stage = s; }
static void set_result(Job* j, const std::string& s) { std::lock_guard<std::mutex> lk(g_ui_mtx); j->result_msg = s; }

// ---------------------------------------------------------------------------
// Small formatting helpers
// ---------------------------------------------------------------------------
static std::string human_size(uint64_t b) {
    char buf[32];
    double v = (double)b;
    const char* u[] = {"B", "KB", "MB", "GB"};
    int i = 0;
    while (v >= 1024.0 && i < 3) { v /= 1024.0; i++; }
    snprintf(buf, sizeof buf, i == 0 ? "%.0f %s" : "%.1f %s", v, u[i]);
    return buf;
}
static std::string mmss(int s) {
    char b[16];
    snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
    return b;
}
static std::wstring filename_of(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

// Build status text for a job (caller holds g_ui_mtx).
static std::string build_status(const Job& j, JobState s) {
    switch (s) {
        case JobState::Probing:  return "Reading...";
        case JobState::Queued:
            if (j.info.is_image) return "Image  -  queued";
            return std::to_string(j.info.width) + "x" + std::to_string(j.info.height) +
                   "  -  " + mmss((int)j.info.duration) + "  -  queued";
        case JobState::Running:   return j.stage.empty() ? "Working..." : j.stage;
        case JobState::Done:      return j.result_msg;
        case JobState::Failed:    return j.result_msg.empty() ? "Failed" : ("Failed: " + j.result_msg);
        case JobState::Cancelled: return "Cancelled";
    }
    return "";
}

// ---------------------------------------------------------------------------
// Background threads
// ---------------------------------------------------------------------------
static void intake_run() {
    while (!g_app_quit.load()) {
        std::vector<std::wstring> batch;
        { std::lock_guard<std::mutex> lk(g_incoming_mtx); batch.swap(g_incoming); }
        if (batch.empty()) { Sleep(40); continue; }

        for (auto& p : batch) {
            auto job = std::make_unique<Job>();
            job->path = p;
            job->name = wide_to_utf8(filename_of(p));
            job->in_size = file_size_of(p);
            job->state = JobState::Probing;
            Job* raw = job.get();
            { std::lock_guard<std::mutex> lk(g_ui_mtx); g_jobs.push_back(std::move(job)); }

            MediaInfo mi = probe_media(p);
            {
                std::lock_guard<std::mutex> lk(g_ui_mtx);
                raw->info = mi;
                if (mi.ok) { raw->state = JobState::Queued; }
                else { raw->result_msg = mi.err; raw->state = JobState::Failed; }
            }
        }
    }
}

static void worker_run(EncodeOptions opts, uint64_t target_bytes) {
    for (;;) {
        if (g_cancel.load()) break;
        Job* job = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_ui_mtx);
            for (auto& jp : g_jobs) {
                if (jp->state.load() == JobState::Queued) {
                    job = jp.get();
                    jp->state = JobState::Running;
                    break;
                }
            }
        }
        if (!job) break;

        set_stage(job, "Starting...");
        job->progress = 0.f;
        auto onp = [job](const ProgressInfo& pi) {
            job->progress = pi.fraction;
            set_stage(job, pi.stage);
        };
        CompressResult r = compress_file(job->path, job->info, target_bytes, opts, onp, g_cancel);

        if (r.ok) {
            {
                std::lock_guard<std::mutex> lk(g_ui_mtx);
                job->out_path = r.out_path;
                job->out_size = r.out_size;
                job->result_msg = r.message;
            }
            job->progress = 1.f;
            job->state = JobState::Done;
        } else {
            set_result(job, r.message);
            job->state = g_cancel.load() ? JobState::Cancelled : JobState::Failed;
        }
        if (g_cancel.load()) break;
    }
    g_worker_running = false;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
static void render_ui() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##main", nullptr, flags);

    static double copy_msg_until = 0.0;
    static int    copy_msg_n = 0;

    ImGui::TextUnformatted("CrunchCord");
    ImGui::SameLine();
    ImGui::TextDisabled("- shrink images & videos to fit Discord's upload limit");
    ImGui::Separator();

    // ---- target size ----
    static int target_idx = 0; static float custom_mb = 25.f;
    const char* targets[] = {"Discord Free  -  10 MB", "Nitro Basic  -  50 MB",
                             "Nitro  -  500 MB", "Custom..."};
    ImGui::SetNextItemWidth(260);
    ImGui::Combo("Target size", &target_idx, targets, IM_ARRAYSIZE(targets));
    if (target_idx == 3) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(110);
        ImGui::InputFloat("MB", &custom_mb, 0, 0, "%.0f");
        if (custom_mb < 1.f) custom_mb = 1.f;
    }

    // ---- add files / folder ----
    static bool recurse_folders = false;
    bool add_files_clicked = false, add_folder_clicked = false, browse_out_clicked = false;
    bool paste_clicked = false;
    if (ImGui::Button("Add files...")) add_files_clicked = true;
    ImGui::SameLine();
    if (ImGui::Button("Add folder...")) add_folder_clicked = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(!clip::has_pasteable());
    if (ImGui::Button("Paste")) paste_clicked = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Include subfolders", &recurse_folders);
    ImGui::SameLine();
    ImGui::TextDisabled("(drag & drop, or Ctrl+V to paste a screenshot or files)");

    // Ctrl+V pastes, unless a text field has focus.
    {
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
            paste_clicked = true;
    }

    // ---- output location ----
    static int out_mode = 0; // 0 = same folder as source, 1 = chosen folder
    static char out_dir_buf[512] = "";
    ImGui::TextUnformatted("Save to:");
    ImGui::SameLine();
    ImGui::RadioButton("Same folder as each file", &out_mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Choose a folder", &out_mode, 1);
    if (out_mode == 1) {
        ImGui::SetNextItemWidth(430);
        ImGui::InputTextWithHint("##outdir", "Pick or type an output folder",
                                 out_dir_buf, sizeof out_dir_buf);
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) browse_out_clicked = true;
    }

    // ---- advanced ----
    static int codec_idx = 0, speed_idx = 1, maxres_idx = 0, fps_idx = 0, audio_idx = 0;
    static bool use_gpu = true;
    bool gpu_checked  = g_hw_ready.load();
    bool gpu_possible = gpu_checked && (g_hw.hevc || g_hw.h264 || g_hw.av1);
    if (ImGui::CollapsingHeader("Advanced")) {
        const char* codecs[] = {"H.265 (HEVC)  -  smaller, previews on most clients",
                                "AV1  -  smallest, slower to encode",
                                "H.264  -  largest, plays everywhere"};
        ImGui::SetNextItemWidth(420); ImGui::Combo("Video codec", &codec_idx, codecs, IM_ARRAYSIZE(codecs));

        const char* speeds[] = {"Fastest", "Balanced", "Best quality (slower)"};
        ImGui::SetNextItemWidth(260); ImGui::Combo("Encode speed", &speed_idx, speeds, IM_ARRAYSIZE(speeds));

        ImGui::BeginDisabled(!gpu_possible);
        ImGui::Checkbox("Use GPU (NVENC) when possible", &use_gpu);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (!gpu_checked)      ImGui::TextDisabled("(checking GPU...)");
        else if (gpu_possible) ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.40f, 1), "GPU ready");
        else                   ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.30f, 1),
                                                  "no GPU encoder (update NVIDIA driver to 570+)");

        const char* res[] = {"Auto", "Max 1080p", "Max 720p", "Max 480p"};
        ImGui::SetNextItemWidth(150); ImGui::Combo("Resolution", &maxres_idx, res, IM_ARRAYSIZE(res));
        ImGui::SameLine(); const char* fpss[] = {"Keep FPS", "Cap 60", "Cap 30"};
        ImGui::SetNextItemWidth(150); ImGui::Combo("##fps", &fps_idx, fpss, IM_ARRAYSIZE(fpss));
        ImGui::SameLine(); const char* auds[] = {"Auto audio", "128 kbps", "96 kbps", "64 kbps"};
        ImGui::SetNextItemWidth(150); ImGui::Combo("##aud", &audio_idx, auds, IM_ARRAYSIZE(auds));

        static int reg_installed = -1;
        if (reg_installed < 0) reg_installed = shellreg::is_installed() ? 1 : 0;
        bool reg_on = (reg_installed == 1);
        if (ImGui::Checkbox("Add 'Compress for Discord' to the Explorer right-click menu", &reg_on)) {
            wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
            if (reg_on) shellreg::install(exe); else shellreg::uninstall();
            reg_installed = shellreg::is_installed() ? 1 : 0;
        }
    }
    ImGui::Separator();

    // ---- action buttons ----
    bool running = g_worker_running.load();
    int queued = 0;
    { std::lock_guard<std::mutex> lk(g_ui_mtx);
      for (auto& j : g_jobs) if (j->state.load() == JobState::Queued) queued++; }

    bool start_clicked = false, cancel_clicked = false, clear_done = false, clear_all = false;
    ImGui::BeginDisabled(running || queued == 0);
    if (ImGui::Button("Start", ImVec2(120, 0))) start_clicked = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Cancel", ImVec2(100, 0))) cancel_clicked = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear finished")) clear_done = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(running);
    if (ImGui::Button("Clear all")) clear_all = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool copy_all_clicked = false;
    if (ImGui::Button("Copy all outputs")) copy_all_clicked = true;
    ImGui::SameLine();
    static bool auto_copy = true;
    ImGui::Checkbox("Auto-copy when done", &auto_copy);

    // ---- snapshot jobs under lock ----
    struct Row { std::string name, status, inS, outS; JobState state; float progress; bool removable; std::wstring out_path; Job* ptr; };
    std::vector<Row> rows;
    uint64_t sum_in = 0, sum_out = 0; int done_cnt = 0, n_active = 0;
    std::vector<std::wstring> all_done_outputs, done_uncopied;
    {
        std::lock_guard<std::mutex> lk(g_ui_mtx);
        rows.reserve(g_jobs.size());
        for (auto& j : g_jobs) {
            Row r;
            r.ptr = j.get();
            r.state = j->state.load();
            r.progress = j->progress.load();
            r.name = j->name;
            r.inS = human_size(j->in_size);
            r.outS = (r.state == JobState::Done) ? human_size(j->out_size) : std::string("-");
            r.status = build_status(*j, r.state);
            r.removable = (r.state != JobState::Running);
            r.out_path = j->out_path;
            if (r.state == JobState::Running || r.state == JobState::Queued || r.state == JobState::Probing)
                n_active++;
            if (r.state == JobState::Done) {
                sum_in += j->in_size; sum_out += j->out_size; done_cnt++;
                all_done_outputs.push_back(j->out_path);
                if (!j->copied) done_uncopied.push_back(j->out_path);
            }
            rows.push_back(std::move(r));
        }
    }

    // ---- table ----
    std::vector<Job*> to_remove;
    std::vector<std::wstring> to_copy;
    ImGui::BeginChild("list", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true);
    if (rows.empty()) {
        ImGui::Dummy(ImVec2(0, 24));
        ImGui::TextDisabled("   No files yet - drag images or videos here to get started.");
    } else if (ImGui::BeginTable("jobs", 5,
               ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 0.34f);
        ImGui::TableSetupColumn("Input", ImGuiTableColumnFlags_WidthFixed, 78);
        ImGui::TableSetupColumn("Output", ImGuiTableColumnFlags_WidthFixed, 78);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 0.5f);
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 92);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < rows.size(); ++i) {
            Row& r = rows[i];
            ImGui::TableNextRow();
            ImGui::PushID((int)i);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.inS.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.outS.c_str());
            ImGui::TableNextColumn();
            if (r.state == JobState::Running) {
                ImGui::ProgressBar(r.progress, ImVec2(-FLT_MIN, 0), r.status.c_str());
            } else {
                ImVec4 col; bool colored = true;
                if (r.state == JobState::Done)           col = ImVec4(0.40f, 0.85f, 0.40f, 1);
                else if (r.state == JobState::Failed)     col = ImVec4(0.90f, 0.40f, 0.40f, 1);
                else if (r.state == JobState::Cancelled)  col = ImVec4(0.85f, 0.72f, 0.30f, 1);
                else colored = false;
                if (colored) ImGui::TextColored(col, "%s", r.status.c_str());
                else ImGui::TextUnformatted(r.status.c_str());
            }
            ImGui::TableNextColumn();
            if (r.state == JobState::Done && !r.out_path.empty()) {
                if (ImGui::SmallButton("Copy")) to_copy.push_back(r.out_path);
                ImGui::SameLine();
            }
            ImGui::BeginDisabled(!r.removable);
            if (ImGui::SmallButton("x")) to_remove.push_back(r.ptr);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    // ---- footer ----
    if (ImGui::GetTime() < copy_msg_until) {
        ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.40f, 1),
                           "Copied %d file(s) to clipboard. Paste into Discord with Ctrl+V.", copy_msg_n);
    } else if (done_cnt > 0 && sum_in > 0) {
        double pct = 100.0 * (1.0 - (double)sum_out / (double)sum_in);
        ImGui::Text("%d done  -  %s to %s  (saved %.0f%%)", done_cnt,
                    human_size(sum_in).c_str(), human_size(sum_out).c_str(), pct);
    } else {
        ImGui::TextDisabled("Ready.");
    }
    ImGui::End();

    // ---- apply actions ----
    if (add_files_clicked) {
        std::vector<std::wstring> keep;
        for (auto& f : dlg::open_files(g_hwnd))
            if (is_supported_media(f)) keep.push_back(f);
        enqueue_paths(keep);
    }
    if (add_folder_clicked) {
        std::wstring folder = dlg::open_folder(g_hwnd);
        if (!folder.empty()) enqueue_paths(dlg::enum_media(folder, recurse_folders));
    }
    if (browse_out_clicked) {
        std::wstring folder = dlg::open_folder(g_hwnd);
        if (!folder.empty()) {
            std::string u = wide_to_utf8(folder);
            strncpy(out_dir_buf, u.c_str(), sizeof out_dir_buf - 1);
            out_dir_buf[sizeof out_dir_buf - 1] = 0;
            out_mode = 1;
        }
    }

    // clipboard: paste content in
    if (paste_clicked) {
        std::vector<std::wstring> keep;
        for (auto& f : clip::paste(g_hwnd))
            if (is_supported_media(f)) keep.push_back(f);
        enqueue_paths(keep);
    }
    // clipboard: copy finished files out
    auto do_copy = [&](const std::vector<std::wstring>& v) {
        if (!v.empty() && clip::copy_files(g_hwnd, v)) {
            copy_msg_until = ImGui::GetTime() + 3.0;
            copy_msg_n = (int)v.size();
        }
    };
    if (!to_copy.empty()) do_copy(to_copy);
    if (copy_all_clicked)  do_copy(all_done_outputs);
    if (auto_copy && n_active == 0 && !done_uncopied.empty()) {
        do_copy(done_uncopied);
        std::lock_guard<std::mutex> lk(g_ui_mtx);
        for (auto& j : g_jobs) if (j->state.load() == JobState::Done) j->copied = true;
    }

    if (start_clicked && !g_worker_running.load()) {
        g_cancel = false;
        g_worker_running = true;
        EncodeOptions opts;
        opts.codec = codec_idx == 0 ? Codec::H265 : codec_idx == 1 ? Codec::AV1 : Codec::H264;
        opts.speed = speed_idx == 0 ? Speed::Fastest : speed_idx == 1 ? Speed::Balanced : Speed::Best;
        opts.use_hardware = use_gpu;
        opts.max_height = maxres_idx == 0 ? 0 : maxres_idx == 1 ? 1080 : maxres_idx == 2 ? 720 : 480;
        opts.fps_cap    = fps_idx == 0 ? 0 : fps_idx == 1 ? 60 : 30;
        opts.audio_kbps = audio_idx == 0 ? 0 : audio_idx == 1 ? 128 : audio_idx == 2 ? 96 : 64;
        opts.out_dir    = (out_mode == 1 && out_dir_buf[0]) ? utf8_to_wide(out_dir_buf) : L"";
        double mb = target_idx == 0 ? 10 : target_idx == 1 ? 50 : target_idx == 2 ? 500 : (double)custom_mb;
        uint64_t tb = (uint64_t)(mb * 1000.0 * 1000.0);
        std::thread(worker_run, opts, tb).detach();
    }
    if (cancel_clicked) g_cancel = true;

    if (!to_remove.empty() || clear_done || clear_all) {
        std::lock_guard<std::mutex> lk(g_ui_mtx);
        g_jobs.erase(std::remove_if(g_jobs.begin(), g_jobs.end(),
            [&](const std::unique_ptr<Job>& j) {
                JobState s = j->state.load();
                if (s == JobState::Running) return false;
                if (clear_all) return true;
                if (clear_done && (s == JobState::Done || s == JobState::Failed || s == JobState::Cancelled))
                    return true;
                for (Job* p : to_remove) if (p == j.get()) return true;
                return false;
            }), g_jobs.end());
    }
}

// ---------------------------------------------------------------------------
// DirectX 11 plumbing (from the Dear ImGui reference example)
// ---------------------------------------------------------------------------
static ID3D11Device*           g_pd3dDevice = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*         g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static UINT                    g_ResizeWidth = 0, g_ResizeHeight = 0;

static void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}
static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}
static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
    D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL levels[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                    levels, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                    &g_pd3dDevice, &level, &g_pd3dDeviceContext);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                    levels, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                    &g_pd3dDevice, &level, &g_pd3dDeviceContext);
    if (hr != S_OK) return false;
    CreateRenderTarget();
    return true;
}
static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            g_ResizeWidth = (UINT)LOWORD(lParam);
            g_ResizeHeight = (UINT)HIWORD(lParam);
            return 0;
        case WM_DROPFILES: {
            HDROP hdrop = (HDROP)wParam;
            UINT n = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::wstring> paths;
            for (UINT i = 0; i < n; ++i) {
                wchar_t p[MAX_PATH];
                if (DragQueryFileW(hdrop, i, p, MAX_PATH)) paths.emplace_back(p);
            }
            DragFinish(hdrop);
            add_paths_expanding(paths);
            return 0;
        }
        case WM_COPYDATA: {
            // Another instance (launched from the right-click menu) forwarded paths.
            COPYDATASTRUCT* cds = (COPYDATASTRUCT*)lParam;
            if (cds && cds->lpData && cds->cbData >= sizeof(wchar_t)) {
                std::wstring blob((const wchar_t*)cds->lpData, cds->cbData / sizeof(wchar_t));
                std::vector<std::wstring> paths;
                size_t start = 0, pos;
                while ((pos = blob.find(L'\n', start)) != std::wstring::npos) {
                    if (pos > start) paths.push_back(blob.substr(start, pos - start));
                    start = pos + 1;
                }
                if (start < blob.size()) paths.push_back(blob.substr(start));
                add_paths_expanding(paths);
            }
            ::ShowWindow(hWnd, SW_RESTORE);
            ::SetForegroundWindow(hWnd);
            return TRUE;
        }
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

int main(int, char**) {
    // File paths passed on the command line (Explorer verb, or "Open with").
    std::vector<std::wstring> cli_paths;
    {
        int n = 0;
        LPWSTR* a = CommandLineToArgvW(GetCommandLineW(), &n);
        if (a) { for (int i = 1; i < n; ++i) cli_paths.emplace_back(a[i]); LocalFree(a); }
    }

    // Single instance: if one is already running, forward our paths to it and exit
    // so right-clicking files always lands them in one window.
    HANDLE inst_mutex = CreateMutexW(nullptr, TRUE, L"CrunchCord_SingleInstance_v1");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = nullptr;
        for (int i = 0; i < 40 && !other; ++i) { // primary may still be starting up
            other = FindWindowW(L"CrunchCordWnd", nullptr);
            if (!other) Sleep(50);
        }
        if (other) {
            if (!cli_paths.empty()) {
                std::wstring blob;
                for (size_t i = 0; i < cli_paths.size(); ++i) { if (i) blob += L'\n'; blob += cli_paths[i]; }
                COPYDATASTRUCT cds{};
                cds.dwData = 1;
                cds.cbData = (DWORD)(blob.size() * sizeof(wchar_t));
                cds.lpData = (void*)blob.data();
                ::AllowSetForegroundWindow(ASFW_ANY);
                ::SendMessageW(other, WM_COPYDATA, 0, (LPARAM)&cds);
            }
            ::ShowWindow(other, SW_RESTORE);
            ::SetForegroundWindow(other);
        }
        if (inst_mutex) CloseHandle(inst_mutex);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    clip::init();
    std::thread(intake_run).detach();
    std::thread([] { g_hw = detect_hw_caps(); g_hw_ready = true; }).detach();

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
                       GetModuleHandle(nullptr), nullptr, ::LoadCursor(nullptr, IDC_ARROW),
                       nullptr, nullptr, L"CrunchCordWnd", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"CrunchCord",
                                WS_OVERLAPPEDWINDOW, 100, 100, 1000, 760,
                                nullptr, nullptr, wc.hInstance, nullptr);
    g_hwnd = hwnd;

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);
    ::DragAcceptFiles(hwnd, TRUE);
    if (!cli_paths.empty()) add_paths_expanding(cli_paths);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    io.FontGlobalScale = 1.15f;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 0.f;
    ImGui::GetStyle().FrameRounding = 4.f;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        render_ui();

        ImGui::Render();
        const float clear[4] = {0.09f, 0.09f, 0.11f, 1.0f};
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    g_app_quit = true;
    g_cancel = true;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    clip::shutdown();
    CoUninitialize();
    if (inst_mutex) CloseHandle(inst_mutex);
    return 0;
}
