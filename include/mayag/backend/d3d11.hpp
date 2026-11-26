#pragma once
#include "../core/target.hpp"
#include "gpu_policy.hpp"
// mayag::backend::D3D11Device — the native Windows GPU backend.
//
// Same transport contract as VulkanDevice (attach, resize, submit, atlas,
// textures), on Direct3D 11 and a DXGI flip-model swapchain, which is what
// every first-class Windows app presents through. Measured against Vulkan on
// the same machine (Intel Arc iGPU, Optimus laptop):
//
//                         Vulkan            D3D11 + DXGI flip
//   device bring-up       330 ms warm,      130 ms warm, 370 ms cold
//                         2.1 s cold        (one driver, not every ICD)
//   resize step           14.5 ms           2.4 ms
//
// Vulkan's loader initialises every installed driver (and wakes the discrete
// GPU) before it can say anything; DXGI asks the one adapter driving the
// monitor. And DXGI owns the composition path: FLIP_DISCARD buffers go to DWM
// (or straight to scan-out when windowed-independent-flip kicks in), resize is
// ResizeBuffers on the existing chain, and the frame-latency waitable object
// gives one frame of queue depth, not three.
//
// Nothing about how a pixel LOOKS lives here: the shaders are the same SPIR-V
// the Vulkan backend runs, cross-compiled to HLSL by tools/gen_shaders.py
// (render/shader_generated.hpp) and compiled here with d3dcompiler_47, which
// ships with Windows. The frame is planned by the same render::FramePlanner:
// shapes blend in linear light through an _SRGB view, text blends in encoded
// space through a UNORM view of the SAME texture -- D3D11's typeless formats
// give exactly what Vulkan's mutable-format swapchain does.
//
// Every DLL is loaded at runtime (d3d11, dxgi, d3dcompiler_47), so a program
// that never opens a window links none of them.

#if MAYAG_OS_WINDOWS

#include "../core/win32.hpp"
#include "../core/startup.hpp"
#include "../render/draw_list.hpp"
#include "../render/frame_plan.hpp"
#include "../render/kernel.hpp"
#include "../render/shader_generated.hpp"
#include "backend.hpp"   // AtlasView / view_of

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mayag::backend {

namespace d3d11_detail {

template <class T> void release(T*& p) noexcept { if (p != nullptr) { p->Release(); p = nullptr; } }

/// The runtime entry points, loaded once.
struct Api {
    HRESULT(WINAPI* create_device)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                   ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**) = nullptr;
    HRESULT(WINAPI* create_factory2)(UINT, REFIID, void**) = nullptr;
    bool ok = false;

    static const Api& get() {
        static const Api a = [] {
            Api r;
            const auto sys = [](const wchar_t* n) { return ::LoadLibraryExW(n, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32); };
            const HMODULE d3d = sys(L"d3d11.dll"), dxgi = sys(L"dxgi.dll");
            if (!d3d || !dxgi) return r;
            r.create_device = reinterpret_cast<decltype(r.create_device)>(reinterpret_cast<void*>(::GetProcAddress(d3d, "D3D11CreateDevice")));
            r.create_factory2 = reinterpret_cast<decltype(r.create_factory2)>(reinterpret_cast<void*>(::GetProcAddress(dxgi, "CreateDXGIFactory2")));
            r.ok = r.create_device && r.create_factory2;
            return r;
        }();
        return a;
    }
};

/// The HLSL compiler: only on a shader-cache miss (first run, or after an
/// update changed the shaders), so a normal launch never maps its 4 MB.
struct Compiler {
    pD3DCompile compile = nullptr;
    static const Compiler& get() {
        static const Compiler c = [] {
            Compiler r;
            if (const HMODULE m = ::LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
                r.compile = reinterpret_cast<pD3DCompile>(reinterpret_cast<void*>(::GetProcAddress(m, "D3DCompile")));
            return r;
        }();
        return c;
    }
};

}  // namespace d3d11_detail

class D3D11Device {
  public:
    D3D11Device() = default;
    D3D11Device(const D3D11Device&) = delete;
    D3D11Device& operator=(const D3D11Device&) = delete;
    ~D3D11Device() { destroy(); }

    void set_low_latency(bool on) noexcept {
        // MAYAG_PRESENT=immediate|fifo overrides, as for Vulkan.
        const auto forced = gpu_policy::present_mode();
        if (forced == gpu_policy::Present::immediate) on = true;
        if (forced == gpu_policy::Present::fifo) on = false;
        low_latency_ = on;
    }
    void set_transparent(bool) noexcept {}   // DirectComposition transparency: not yet

    /// Device + swapchain on `hwnd`, in one go.
    [[nodiscard]] bool attach(HWND hwnd, Vec2 logical, float dpi) {
        return prepare() && attach_swapchain(hwnd, logical, dpi);
    }

    /// Everything that needs no window: factory, adapter, device, pipeline.
    /// This is ~all of the bring-up cost, so a window starts it BEFORE it
    /// creates its HWND and the two overlap. Any thread.
    [[nodiscard]] bool prepare() {
        if (dev_ != nullptr) return true;
        const d3d11_detail::Api& api = d3d11_detail::Api::get();
        if (!api.ok) return false;
        startup::mark("  d3d11 dlls loaded");
        IDXGIFactory2* f2 = nullptr;
        if (FAILED(api.create_factory2(0, __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&f2)))) return false;
        factory_ = f2;
        startup::mark("  d3d11 dxgi factory");
        // The adapter that drives the panel: on a hybrid laptop the iGPU, so
        // nothing has to be copied across PCIe to scan out, and the discrete
        // GPU can stay asleep. MAYAG_GPU=discrete asks for the other one.
        IDXGIAdapter1* adapter = nullptr;
        {
            const char* want = std::getenv("MAYAG_GPU");
            const auto pref = (want != nullptr && std::string_view{want} == "discrete")
                                  ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE : DXGI_GPU_PREFERENCE_MINIMUM_POWER;
            IDXGIFactory6* f6 = nullptr;
            if (SUCCEEDED(f2->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&f6)))) {
                (void)f6->EnumAdapterByGpuPreference(0, pref, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter));
                f6->Release();
            }
        }
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        startup::mark("  d3d11 adapter chosen");
        HRESULT hr = api.create_device(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
        if (adapter) {
            DXGI_ADAPTER_DESC1 d{};
            if (SUCCEEDED(adapter->GetDesc1(&d))) {
                ::WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, device_name_, sizeof device_name_, nullptr, nullptr);
                // Intel iGPUs present a composition swapchain's resize frame
                // slightly out of step with the window (the gap shows at the
                // moving edge); waiting for that output's vblank first is the
                // known remedy (noflicker_directx_window: syncIntelOutput).
                if (d.VendorId == 0x8086) (void)adapter->EnumOutputs(0, &intel_output_);
            }
            adapter->Release();
        }
        if (FAILED(hr) || dev_ == nullptr) return false;
        startup::mark("  d3d11 device created");

        // Tearing support: lets a vsync-off present go out immediately even
        // in a window (DXGI_PRESENT_ALLOW_TEARING), the latency floor.
        {
            IDXGIFactory5* f5 = nullptr;
            BOOL allow = FALSE;
            if (SUCCEEDED(f2->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&f5)))) {
                (void)f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow);
                f5->Release();
            }
            tearing_ = allow != FALSE;
        }

        if (!create_pipeline()) return false;
        startup::mark("  d3d11 pipeline");
        return true;
    }

    /// The swapchain on `hwnd` (a few ms). Any thread: DXGI swapchains are not
    /// tied to the window's thread for creation; ResizeBuffers and Present
    /// are called from the UI thread.
    [[nodiscard]] bool attach_swapchain(HWND hwnd, Vec2 logical, float dpi) {
        if (dev_ == nullptr || hwnd == nullptr) return false;
        hwnd_ = hwnd;
        logical_ = logical;
        dpi_ = dpi;
        if (!create_swapchain()) return false;
        startup::mark("  d3d11 swapchain");
        valid_ = true;
        return true;
    }

    void destroy() {
        using d3d11_detail::release;
        if (ctx_) { ctx_->ClearState(); ctx_->Flush(); }
        for (auto& [id, t] : textures_) { release(t.srv); release(t.tex); }
        textures_.clear();
        release_targets();
        if (dcomp_visual_) { dcomp_visual_->Release(); dcomp_visual_ = nullptr; }
        if (dcomp_target_) { dcomp_target_->Release(); dcomp_target_ = nullptr; }
        if (dcomp_dev_) { dcomp_dev_->Release(); dcomp_dev_ = nullptr; }
        release(swapchain_);
        release(intel_output_);
        if (latency_wait_) { ::CloseHandle(latency_wait_); latency_wait_ = nullptr; }
        release(atlas_srv_); release(atlas_tex_);
        release(color_srv_); release(color_tex_);
        release(capture_srv_); release(capture_tex_);
        release(white_srv_); release(white_tex_);
        release(instance_buf_); release(viewport_cb_);
        release(sampler_); release(blend_); release(raster_);
        release(vs_); release(ps_); release(ps_text_); release(layout_);
        release(ctx_); release(dev_); release(factory_);
        valid_ = false;
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const char* device_name() const noexcept { return device_name_; }
    [[nodiscard]] bool needs_present() const noexcept { return content_changed_; }

    /// Composed through DirectComposition (resize frames can be synchronous).
    [[nodiscard]] bool composed() const noexcept { return dcomp_visual_ != nullptr; }
    /// The next submit() is a RESIZE frame: present at once, discarding any
    /// queued frame (DXGI_PRESENT_RESTART), then wait for the vblank so it is
    /// on the glass before the window finishes moving -- the recipe that makes
    /// a composition swapchain resize without lag or tearing at the edge.
    void present_for_resize() noexcept { resize_frame_ = true; }

    void resize(Vec2 logical, float dpi) {
        logical_ = logical;
        dpi_ = dpi;
        resize_pending_ = true;
    }

    template <typename AtlasT>
    void sync_atlas(AtlasT& atlas) { sync_atlas_view(view_of(atlas)); }

    void sync_atlas_view(const AtlasView& a) {
        if (!valid_ || a.width <= 0 || a.height <= 0 || a.coverage == nullptr) return;
        if (a.width != atlas_w_ || a.height != atlas_h_) {
            using d3d11_detail::release;
            release(atlas_srv_); release(atlas_tex_);
            release(color_srv_); release(color_tex_);
            atlas_w_ = a.width; atlas_h_ = a.height;
            make_texture(atlas_w_, atlas_h_, DXGI_FORMAT_R8_UNORM, nullptr, 0, atlas_tex_, atlas_srv_);
            make_texture(atlas_w_, atlas_h_, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, 0, color_tex_, color_srv_);
            ctx_->UpdateSubresource(atlas_tex_, 0, nullptr, a.coverage, static_cast<UINT>(a.width), 0);
            a.ack_coverage();
            if (a.has_color && a.color) { ctx_->UpdateSubresource(color_tex_, 0, nullptr, a.color, static_cast<UINT>(a.width) * 4, 0); a.ack_color(); }
            content_changed_ = true;
            return;
        }
        if (a.coverage_dirty) {
            ctx_->UpdateSubresource(atlas_tex_, 0, nullptr, a.coverage, static_cast<UINT>(a.width), 0);
            a.ack_coverage();
            content_changed_ = true;
        }
        if (a.has_color && a.color && a.color_dirty) {
            ctx_->UpdateSubresource(color_tex_, 0, nullptr, a.color, static_cast<UINT>(a.width) * 4, 0);
            a.ack_color();
            content_changed_ = true;
        }
    }

    std::uint32_t upload_texture(std::uint32_t id, const std::uint8_t* rgba, int w, int h) {
        if (!valid_ || rgba == nullptr || w <= 0 || h <= 0 || id == 0) return 0;
        auto it = textures_.find(id);
        if (it != textures_.end() && (it->second.w != w || it->second.h != h)) {
            d3d11_detail::release(it->second.srv); d3d11_detail::release(it->second.tex);
            textures_.erase(it);
            it = textures_.end();
        }
        if (it == textures_.end()) {
            Texture t{}; t.w = w; t.h = h;
            if (!make_texture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, rgba, static_cast<UINT>(w) * 4, t.tex, t.srv)) return 0;
            textures_.emplace(id, t);
        } else {
            ctx_->UpdateSubresource(it->second.tex, 0, nullptr, rgba, static_cast<UINT>(w) * 4, 0);
        }
        content_changed_ = true;
        return id;
    }
    void release_texture(std::uint32_t id) {
        if (auto it = textures_.find(id); it != textures_.end()) {
            d3d11_detail::release(it->second.srv); d3d11_detail::release(it->second.tex);
            textures_.erase(it);
        }
    }

    /// Render and present one frame.
    void submit(const DrawList& list, Color<Srgb> clear) {
        if (!valid_) return;
        if (resize_pending_) { resize_pending_ = false; resize_swapchain(); }
        if (rtv_srgb_ == nullptr) return;
        // Wait for the swapchain to want a frame (latency 1). The host asks
        // queue_full() first and holds a late draw rather than come here
        // with the queue full (see there); a probe that found it free left a
        // token, so this does not wait twice. Not right after a resize: the
        // chain's buffers are new and nothing is queued.
        LARGE_INTEGER wait0{};
        if (pstats_.on) ::QueryPerformanceCounter(&wait0);
        if (latency_wait_ && !just_resized_ && !queue_token_ && !resize_frame_) ::WaitForSingleObjectEx(latency_wait_, 100, TRUE);
        queue_token_ = false;
        if (pstats_.on) {
            LARGE_INTEGER w1, f;
            ::QueryPerformanceCounter(&w1);
            ::QueryPerformanceFrequency(&f);
            pstats_.wait_ms.push_back(static_cast<double>(w1.QuadPart - wait0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart));
        }
        just_resized_ = false;

        upload_instances(list);
        const UINT vw = sc_w_, vh = sc_h_;
        const float vp[4] = {static_cast<float>(vw), static_cast<float>(vh), 0.0f, 0.0f};
        ctx_->UpdateSubresource(viewport_cb_, 0, nullptr, vp, 0, 0);

        const auto lin = clear.template to<Linear>();
        const float cc[4] = {lin.c0, lin.c1, lin.c2, 1.0f};
        ctx_->ClearRenderTargetView(rtv_srgb_, cc);

        // Fixed state for every pass.
        const UINT stride = sizeof(Instance), offset = 0;
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx_->IASetInputLayout(layout_);
        ctx_->IASetVertexBuffers(0, 1, &instance_buf_, &stride, &offset);
        ctx_->VSSetShader(vs_, nullptr, 0);
        ctx_->VSSetConstantBuffers(0, 1, &viewport_cb_);
        ctx_->RSSetState(raster_);
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(vw), static_cast<float>(vh), 0, 1};
        ctx_->RSSetViewports(1, &viewport);
        ctx_->OMSetBlendState(blend_, nullptr, 0xFFFFFFFFu);
        ID3D11SamplerState* samplers[4] = {sampler_, sampler_, sampler_, sampler_};
        ctx_->PSSetSamplers(0, 4, samplers);

        const auto& batches = list.batches();
        const auto& insts = list.instances();
        const render::FramePlan& plan = planner_.plan(list, vw, vh, /*encoded_text=*/rtv_unorm_ != nullptr);
        for (const auto& step : plan.steps) {
            if (step.kind == render::PassStep::Kind::capture) {
                capture_region(backdrop_region(insts[step.capture_instance], vw, vh));
                continue;
            }
            const bool text = step.kind == render::PassStep::Kind::text;
            // Unbind the capture SRV before rendering into the target it was
            // copied from? It is a separate texture, so no hazard; bind all.
            ID3D11ShaderResourceView* srvs[3] = {atlas_srv_ ? atlas_srv_ : white_srv_,
                                                 color_srv_ ? color_srv_ : white_srv_,
                                                 capture_srv_ ? capture_srv_ : white_srv_};
            ctx_->PSSetShaderResources(0, 3, srvs);
            ctx_->OMSetRenderTargets(1, text ? &rtv_unorm_ : &rtv_srgb_, nullptr);
            ctx_->PSSetShader(text ? ps_text_ : ps_, nullptr, 0);
            ID3D11ShaderResourceView* bound = nullptr;
            for (std::uint32_t r = step.runs_begin; r < step.runs_end; ++r) {
                const render::DrawRun& run = plan.runs[r];
                ID3D11ShaderResourceView* want = srv_for(batches[run.batch].texture);
                if (want != bound) { ctx_->PSSetShaderResources(3, 1, &want); bound = want; }
                const D3D11_RECT sc = clamp_scissor(batches[run.batch].clip, vw, vh);
                ctx_->RSSetScissorRects(1, &sc);
                ctx_->DrawInstanced(4, run.count, 0, run.first);
            }
        }
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);

        const bool immediate = low_latency_ && tearing_;
        LARGE_INTEGER submit_q{};
        // empty queue: the previous present went out before the latest vblank
        // (the learned clock says when). Not DXGI's statistics: they report a
        // flip up to a refresh late, so with one present per refresh nearly
        // every frame looked queued -- and the learner, starved of evidence,
        // never widened a margin that was missing every vblank.
        bool queue_was_empty = last_submit_qpc_ == 0;
        if (!queue_was_empty && latch_.anchor != 0) {
            LARGE_INTEGER nq, fq;
            ::QueryPerformanceCounter(&nq);
            ::QueryPerformanceFrequency(&fq);
            const auto pq = static_cast<LONGLONG>(latch_.period_s * static_cast<double>(fq.QuadPart));
            if (pq > 0 && nq.QuadPart > latch_.anchor) {
                const LONGLONG last_vblank = latch_.anchor + (nq.QuadPart - latch_.anchor) / pq * pq;
                queue_was_empty = last_submit_qpc_ <= last_vblank;
            }
        }
        ::QueryPerformanceCounter(&submit_q);
        last_submit_qpc_ = submit_q.QuadPart;
        HRESULT hr;
        if (std::exchange(resize_frame_, false)) {
            if (intel_output_ != nullptr) (void)intel_output_->WaitForVBlank();
            hr = swapchain_->Present(0, DXGI_PRESENT_RESTART);
            // on the glass before we return -- one vblank wait per step, not two
            // (the Intel sync above already waited one)
            if (SUCCEEDED(hr) && intel_output_ == nullptr) (void)swapchain_->Present(1, DXGI_PRESENT_DO_NOT_SEQUENCE);
        } else {
            hr = swapchain_->Present(immediate ? 0 : 1, immediate ? DXGI_PRESENT_ALLOW_TEARING : 0);
        }
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) valid_ = false;
        content_changed_ = false;
        ++frames_;
        learn_latch(submit_q, queue_was_empty);
        if (pstats_.on) track_present(submit_q);
    }

    /// How long before a vblank a frame must be presented to make it: DWM's
    /// composition deadline. It sits several ms before scan-out and moves
    /// from launch to launch (one laptop: ~7 ms of lead on some, ~10+ on
    /// others), so it is learned. Only frames presented into an EMPTY queue
    /// are evidence: one queued behind another shows late whatever its lead
    /// (that contamination is what made an earlier learner run away to a
    /// full refresh). Such a frame shown a refresh later than the vblank it
    /// was handed over for -- past the pipeline's own depth, 1 composed, 0 on
    /// an overlay plane -- missed: +1 ms. Each late draw that made it:
    /// -5 us. Bounded to [1 ms, 3/4 refresh]; 0 until it has evidence.
    [[nodiscard]] double latch_margin_s() const noexcept { return latch_.tried >= 8 && depth_ == 0 ? latch_.margin_s : 0.0; }
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> latch_misses() const noexcept { return {latch_.missed, latch_.tried}; }

  private:
    struct LatchLearner {
        struct Sent { UINT count; LONGLONG target; double lead_s; int depth; };
        std::vector<Sent> sent;
        LONGLONG anchor = 0;       // QPC of a vblank a shown frame hit
        UINT anchor_refresh = 0;
        double period_s = 1.0 / 60.0;
        double margin_s = 1.0 / 120.0;
        std::uint64_t tried = 0, missed = 0;
        UINT last_count = 0;   // DXGI's present count at the last present (a drop: the chain was reallocated)
        int late_run = 0;      // consecutive late frames (a stuck queue, not a deadline)
    } latch_;

    [[nodiscard]] int pipeline_depth() const noexcept {
        int depth = 1;   // composed by DWM: shown a refresh after the vblank it latched on
        IDXGISwapChainMedia* media = nullptr;
        if (SUCCEEDED(swapchain_->QueryInterface(__uuidof(IDXGISwapChainMedia), reinterpret_cast<void**>(&media)))) {
            DXGI_FRAME_STATISTICS_MEDIA fm{};
            if (SUCCEEDED(media->GetFrameStatisticsMedia(&fm)) && fm.CompositionMode == DXGI_FRAME_PRESENTATION_MODE_OVERLAY)
                depth = 0;   // flipped to a plane: shown ON it
            media->Release();
        }
        return depth;
    }

    void learn_latch(LARGE_INTEGER submit_q, bool queue_was_empty) {
        auto& L = latch_;
        LARGE_INTEGER f;
        ::QueryPerformanceFrequency(&f);
        const double tick_s = 1.0 / static_cast<double>(f.QuadPart);
        // ResizeBuffers restarts DXGI's present counts, and until a frame of
        // the new chain is shown the statistics still name the OLD chain's
        // last frame. Anything keyed on counts must restart with them, and
        // ignore statistics from before -- otherwise stale entries block every
        // verdict and a frozen anchor mispredicts the queue (measured: after
        // a window drag, every second draw held, a steady 30 Hz).
        UINT count = 0;
        if (FAILED(swapchain_->GetLastPresentCount(&count))) return;
        if (count < L.last_count) { L.sent.clear(); L.anchor = 0; L.anchor_refresh = 0; }
        L.last_count = count;
        DXGI_FRAME_STATISTICS fs{};
        if (FAILED(swapchain_->GetFrameStatistics(&fs)) || fs.SyncQPCTime.QuadPart == 0 || fs.PresentCount > count) return;
        if (L.anchor != 0 && fs.SyncRefreshCount > L.anchor_refresh && fs.SyncRefreshCount - L.anchor_refresh <= 600 &&
            fs.SyncQPCTime.QuadPart > L.anchor) {
            const double p = static_cast<double>(fs.SyncQPCTime.QuadPart - L.anchor) * tick_s /
                             static_cast<double>(fs.SyncRefreshCount - L.anchor_refresh);
            if (p > 1.0 / 500.0 && p < 1.0 / 20.0) L.period_s = L.period_s * 0.9 + p * 0.1;
        }
        for (const auto& s : L.sent) {
            if (s.count > fs.PresentCount) break;
            if (s.count != fs.PresentCount || s.lead_s >= L.period_s * 0.75) continue;   // replaced, or early: no verdict
            const double late = static_cast<double>(fs.SyncQPCTime.QuadPart - s.target) * tick_s;
            const int k = std::max(0, static_cast<int>(std::lround(late / L.period_s)));
            ++L.tried;
            if (k > s.depth) {
                ++L.missed;
                // Several misses in a row are not a deadline problem: the
                // queue is one frame deep with a frame that missed, and every
                // present since queues behind it (shown exactly a refresh
                // late, none lost -- measured in 2 s stretches). Widening the
                // margin cannot fix that; one held refresh drains it.
                if (++L.late_run >= 3) { drain_ = true; L.late_run = 0; continue; }
                L.margin_s = std::min(L.margin_s + 1.0e-3, L.period_s * 0.75);
            } else {
                L.late_run = 0;
                if (s.lead_s < L.margin_s + 2.0e-3) {
                // -5 us a hit against +1 ms a miss: it settles where about one
                // late draw in 200 misses (at -20 us it was one in 25, and
                // that showed as judder: every 25th frame a refresh late)
                L.margin_s = std::max(L.margin_s - 5e-6, 1.0e-3);
                }
            }
        }
        std::erase_if(L.sent, [&](const LatchLearner::Sent& s) { return s.count <= fs.PresentCount; });
        // Composed by DWM or flipped to a plane: asked every 30 presents (a
        // window changes between them as it is covered, resized, moved). On a
        // plane a frame shows ON the vblank it makes, so the deadline can be
        // learned and the queue predicted from the clock. Composed, DWM
        // keeps only the newest frame per composition and its timing is its
        // own: that clock-based prediction measured as half the frames never
        // shown, so composed windows keep the latency object and half a
        // refresh (see queue_full, latch_margin_s).
        if (frames_ % 30 == 1) depth_ = pipeline_depth();
        if (depth_ != 0) {
            L.anchor = fs.SyncQPCTime.QuadPart;
            L.anchor_refresh = fs.SyncRefreshCount;
            return;
        }
        L.anchor = fs.SyncQPCTime.QuadPart;
        L.anchor_refresh = fs.SyncRefreshCount;
        if (!queue_was_empty) return;
        const LONGLONG period_q = static_cast<LONGLONG>(L.period_s / tick_s);
        if (period_q <= 0 || submit_q.QuadPart <= L.anchor) return;
        const LONGLONG target = L.anchor + ((submit_q.QuadPart - L.anchor) / period_q + 1) * period_q;
        L.sent.push_back({count, target, static_cast<double>(target - submit_q.QuadPart) * tick_s, depth_});
        if (L.sent.size() > 32) L.sent.erase(L.sent.begin());
    }

  public:

    /// Is the one-frame queue still full -- would a Present now block, and
    /// then show a whole refresh late? Asked, never waited on.
    ///
    /// This is what made every motion lag on some launches: one frame that
    /// missed its vblank left the queue full at the next late draw, which
    /// blocked until that vblank, presented just after it, missed again --
    /// a stable state, every frame a refresh late for as long as anything
    /// moved (measured: ~7.7 ms blocked in the wait per frame, 16.5 ms
    /// present->glass, in overlay and composed mode alike). The host holds
    /// such a draw to its next late-latch point instead: the queued frame
    /// flips, and the pipeline is back in step at the cost of one frame,
    /// once. A free queue's signal is taken here and kept for submit().
    [[nodiscard]] bool queue_full() noexcept {
        if (latency_wait_ == nullptr || queue_token_ || resize_pending_ || just_resized_) return false;
        if (std::exchange(drain_, false)) return true;   // a stuck queue: hold one refresh (learn_latch)
        // The flip queue: the last present is still waiting for a vblank if
        // it was handed over after the most recent one (the learned clock
        // says when that was). The latency object alone cannot see it -- it
        // signals once the GPU has taken the frame, so a second frame could
        // still queue behind the first for the NEXT vblank: a stable state on
        // some launches, every frame a refresh late (24.4 ms present->glass
        // at 60 Hz, vs 7.7 out of it). Not DXGI's frame statistics: they
        // report a flip up to a refresh after it, so at the late-latch point
        // the frame that just flipped still looked pending and every second
        // draw was held (a steady 30 Hz).
        if (last_submit_qpc_ != 0 && latch_.anchor != 0 && depth_ == 0) {
            LARGE_INTEGER now_q, f;
            ::QueryPerformanceCounter(&now_q);
            ::QueryPerformanceFrequency(&f);
            const auto period_q = static_cast<LONGLONG>(latch_.period_s * static_cast<double>(f.QuadPart));
            if (period_q > 0 && now_q.QuadPart > latch_.anchor && now_q.QuadPart - latch_.anchor < 3 * period_q) {
                const LONGLONG last_vblank = latch_.anchor + (now_q.QuadPart - latch_.anchor) / period_q * period_q;
                if (last_submit_qpc_ > last_vblank) return true;
            }
        }
        if (::WaitForSingleObjectEx(latency_wait_, 0, FALSE) == WAIT_OBJECT_0) {
            queue_token_ = true;
            return false;
        }
        return true;
    }

    [[nodiscard]] std::uint64_t frames_presented() const noexcept { return frames_; }

    /// The vblank our latest shown present reached the screen on (QPC) and
    /// the refresh counter there: the display's REAL phase and rate, on the
    /// monitor the window is on. False until something was shown.
    [[nodiscard]] bool last_shown_vblank(LONGLONG& qpc, UINT& refresh) const noexcept {
        if (swapchain_ == nullptr) return false;
        DXGI_FRAME_STATISTICS fs{};
        if (FAILED(swapchain_->GetFrameStatistics(&fs)) || fs.SyncQPCTime.QuadPart == 0) return false;
        qpc = fs.SyncQPCTime.QuadPart;
        refresh = fs.SyncRefreshCount;
        return true;
    }

  private:
    // ── MAYAG_PRESENTSTATS=1: what the screen actually showed ────────────
    // PresentMon's two numbers without ETW (no admin): DXGI's frame
    // statistics name the last present that reached scan-out and when, so
    // each present gets its submit -> on-screen time, and presents that
    // never showed (replaced in the queue) are counted. Printed every 2 s.
    struct PresentStats {
        bool on = [] { const char* e = std::getenv("MAYAG_PRESENTSTATS"); return e && *e && *e != '0'; }();
        struct Sent { UINT count; LONGLONG q; };
        std::vector<Sent> sent;
        std::vector<double> lat_ms;
        std::vector<double> gap_ms;    // submit-to-submit, while presents come within 100 ms (a burst)
        std::vector<double> wait_ms;   // blocked in the frame-latency wait before Present
        LONGLONG prev_q = 0;
        UINT last_shown = 0;
        UINT last_refresh = 0;
        int dropped = 0, shown = 0, gaps = 0;   // gaps: a refresh passed showing nothing new mid-burst
        LONGLONG window_start = 0;
        LONGLONG last_qpc = 0;
        double period_ms = 0;
    } pstats_;
    bool queue_token_ = false;   ///< queue_full() took the latency object's signal
    bool drain_ = false;         ///< frames keep showing a refresh late: skip one present
    int depth_ = 1;              ///< 0: on an overlay plane; 1: composed by DWM
    LONGLONG last_submit_qpc_ = 0;   ///< QPC just before the last Present

    void track_present(LARGE_INTEGER submit_q) {
        auto& s = pstats_;
        LARGE_INTEGER f;
        ::QueryPerformanceFrequency(&f);
        UINT count = 0;
        if (s.prev_q != 0) {
            const double g = static_cast<double>(submit_q.QuadPart - s.prev_q) * 1000.0 / static_cast<double>(f.QuadPart);
            if (g < 100.0) s.gap_ms.push_back(g);
        }
        s.prev_q = submit_q.QuadPart;
        if (SUCCEEDED(swapchain_->GetLastPresentCount(&count))) {
            // ResizeBuffers restarts the counts: what was pending is gone, not lost
            if (!s.sent.empty() && count <= s.sent.back().count) { s.sent.clear(); s.last_shown = 0; s.last_refresh = 0; s.last_qpc = 0; }
            s.sent.push_back({count, submit_q.QuadPart});
        }
        DXGI_FRAME_STATISTICS fs{};
        if (SUCCEEDED(swapchain_->GetFrameStatistics(&fs)) && fs.PresentCount <= count && fs.PresentCount != s.last_shown) {
            for (const auto& p : s.sent) {
                if (p.count > fs.PresentCount) break;
                if (p.count == fs.PresentCount) {
                    s.lat_ms.push_back(static_cast<double>(fs.SyncQPCTime.QuadPart - p.q) * 1000.0 / static_cast<double>(f.QuadPart));
                    ++s.shown;
                } else if (p.count > s.last_shown) {
                    ++s.dropped;
                }
            }
            // consecutive presents shown more than one refresh apart while
            // they were submitted less than one apart: a missed refresh
            if (s.last_shown != 0 && fs.PresentCount == s.last_shown + 1 && fs.SyncRefreshCount > s.last_refresh + 1)
                ++s.gaps;
            if (s.last_shown != 0 && fs.SyncRefreshCount > s.last_refresh && fs.SyncQPCTime.QuadPart > s.last_qpc && s.last_qpc != 0) {
                const double per = static_cast<double>(fs.SyncQPCTime.QuadPart - s.last_qpc) * 1000.0 / static_cast<double>(f.QuadPart) /
                                   static_cast<double>(fs.SyncRefreshCount - s.last_refresh);
                if (per > 4 && per < 50) s.period_ms = per;
            }
            s.last_qpc = fs.SyncQPCTime.QuadPart;
            std::erase_if(s.sent, [&](const PresentStats::Sent& p) { return p.count <= fs.PresentCount; });
            s.last_shown = fs.PresentCount;
            s.last_refresh = fs.SyncRefreshCount;
        }
        if (s.sent.size() > 256) s.sent.erase(s.sent.begin(), s.sent.begin() + 128);
        if (s.window_start == 0) s.window_start = submit_q.QuadPart;
        if (submit_q.QuadPart - s.window_start > 2 * f.QuadPart) {
            if (!s.lat_ms.empty()) {
                std::sort(s.lat_ms.begin(), s.lat_ms.end());
                const auto at = [&](double q) { return s.lat_ms[std::min(s.lat_ms.size() - 1, static_cast<std::size_t>(q * static_cast<double>(s.lat_ms.size())))]; };
                std::fprintf(stderr, "mayag: present->screen p50 %.1f  p90 %.1f  max %.1f ms  (%d shown, %d never shown, %d missed refreshes)",
                             at(0.5), at(0.9), s.lat_ms.back(), s.shown, s.dropped, s.gaps);
                if (!s.gap_ms.empty()) {
                    std::sort(s.gap_ms.begin(), s.gap_ms.end());
                    const auto g = [&](double q) { return s.gap_ms[std::min(s.gap_ms.size() - 1, static_cast<std::size_t>(q * static_cast<double>(s.gap_ms.size())))]; };
                    std::fprintf(stderr, "  present gaps p50 %.1f p90 %.1f max %.1f ms", g(0.5), g(0.9), s.gap_ms.back());
                }
                std::fprintf(stderr, "\n");
                if (!s.wait_ms.empty()) {
                    std::sort(s.wait_ms.begin(), s.wait_ms.end());
                    std::fprintf(stderr, "mayag: latency wait p50 %.2f p90 %.2f ms", s.wait_ms[s.wait_ms.size() / 2], s.wait_ms[s.wait_ms.size() * 9 / 10]);
                }
                IDXGISwapChainMedia* media = nullptr;
                if (SUCCEEDED(swapchain_->QueryInterface(__uuidof(IDXGISwapChainMedia), reinterpret_cast<void**>(&media)))) {
                    DXGI_FRAME_STATISTICS_MEDIA fm{};
                    if (SUCCEEDED(media->GetFrameStatisticsMedia(&fm)))
                        std::fprintf(stderr, "  mode %s", fm.CompositionMode == DXGI_FRAME_PRESENTATION_MODE_COMPOSED ? "composed"
                                                     : fm.CompositionMode == DXGI_FRAME_PRESENTATION_MODE_OVERLAY ? "overlay"
                                                     : fm.CompositionMode == DXGI_FRAME_PRESENTATION_MODE_NONE ? "none" : "failed");
                    media->Release();
                }
                std::fprintf(stderr, "\n");
                std::fprintf(stderr, "mayag: display refresh %.2f ms (measured)  latch margin %.2f ms (%llu of %llu missed)\n", s.period_ms,
                             latch_.margin_s * 1e3, static_cast<unsigned long long>(latch_.missed), static_cast<unsigned long long>(latch_.tried));
            }
            s.lat_ms.clear();
            s.wait_ms.clear();
            s.gap_ms.clear();
            s.shown = s.dropped = s.gaps = 0;
            s.window_start = submit_q.QuadPart;
        }
    }

  public:

  private:
    struct Texture { ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0; };

    // ── setup ───────────────────────────────────────────────────────────

    using Bytecode = std::vector<std::uint8_t>;
    [[nodiscard]] bool compile(std::string_view src, const char* target, const D3D_SHADER_MACRO* defs, Bytecode& out) {
        // d3dcompiler takes ~200 ms for the pixel shader -- more than device
        // creation. The bytecode depends only on the source, the target and
        // the defines, so it is cached under %LOCALAPPDATA%\mayag keyed by a
        // hash of all three; a stale or corrupt cache entry just recompiles.
        // On a hit d3dcompiler_47.dll (4 MB) is never even loaded.
        std::uint64_t h = 1469598103934665603ull;
        const auto mix = [&](std::string_view s) { for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; } };
        mix(src); mix(target);
        for (const D3D_SHADER_MACRO* d = defs; d && d->Name; ++d) { mix(d->Name); mix("="); mix(d->Definition); }
        wchar_t path[MAX_PATH] = {};
        bool cacheable = false;
        if (const char* l = std::getenv("LOCALAPPDATA"); l != nullptr && std::getenv("MAYAG_SHADER_CACHE") == nullptr) {
            wchar_t dir[MAX_PATH];
            std::swprintf(dir, MAX_PATH, L"%hs\\mayag", l);
            ::CreateDirectoryW(dir, nullptr);
            std::swprintf(path, MAX_PATH, L"%ls\\d3d11-%016llx.dxbc", dir, static_cast<unsigned long long>(h));
            cacheable = true;
        }
        if (cacheable) {
            const HANDLE f = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER sz{};
                bool ok = ::GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < (16 << 20);
                DWORD got = 0;
                if (ok) out.resize(static_cast<std::size_t>(sz.QuadPart));
                ok = ok && ::ReadFile(f, out.data(), static_cast<DWORD>(sz.QuadPart), &got, nullptr) && got == sz.QuadPart;
                ::CloseHandle(f);
                if (ok) return true;
                out.clear();
            }
        }
        const auto& comp = d3d11_detail::Compiler::get();
        if (comp.compile == nullptr) return false;
        ID3DBlob* blob = nullptr;
        ID3DBlob* err = nullptr;
        const HRESULT hr = comp.compile(src.data(), src.size(), "mayag", defs, nullptr, "main", target,
                                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
        if (FAILED(hr) && err) std::fprintf(stderr, "mayag d3d11: %s\n", static_cast<const char*>(err->GetBufferPointer()));
        d3d11_detail::release(err);
        if (SUCCEEDED(hr)) {
            const auto* p = static_cast<const std::uint8_t*>(blob->GetBufferPointer());
            out.assign(p, p + blob->GetBufferSize());
        }
        d3d11_detail::release(blob);
        if (SUCCEEDED(hr) && cacheable) {
            // Write-then-rename, so a concurrent reader never sees half a file.
            wchar_t tmp[MAX_PATH];
            std::swprintf(tmp, MAX_PATH, L"%ls.%lu.tmp", path, ::GetCurrentProcessId());
            const HANDLE f = ::CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD put = 0;
                const bool ok = ::WriteFile(f, out.data(), static_cast<DWORD>(out.size()), &put, nullptr);
                ::CloseHandle(f);
                if (!ok || !::MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) ::DeleteFileW(tmp);
            }
        }
        return SUCCEEDED(hr);
    }

    [[nodiscard]] bool create_pipeline() {
        Bytecode vsb, psb, ptb;
        const D3D_SHADER_MACRO text_pass[] = {{"SPIRV_CROSS_CONSTANT_ID_0", "true"}, {nullptr, nullptr}};
        if (!compile(render::generated::hlsl_vertex, "vs_5_0", nullptr, vsb) ||
            !compile(render::generated::hlsl_fragment, "ps_5_0", nullptr, psb) ||
            !compile(render::generated::hlsl_fragment, "ps_5_0", text_pass, ptb))
            return false;
        bool ok = SUCCEEDED(dev_->CreateVertexShader(vsb.data(), vsb.size(), nullptr, &vs_)) &&
                  SUCCEEDED(dev_->CreatePixelShader(psb.data(), psb.size(), nullptr, &ps_)) &&
                  SUCCEEDED(dev_->CreatePixelShader(ptb.data(), ptb.size(), nullptr, &ps_text_));
        // One per-instance stream = the Instance struct, as Vulkan binds it.
        // SPIRV-Cross names vertex inputs TEXCOORD<location>.
        const D3D11_INPUT_ELEMENT_DESC el[8] = {
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Instance, rect), D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Instance, radii), D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Instance, color), D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Instance, color2), D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Instance, axis), D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 5, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Instance, uv), D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 6, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Instance, params), D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 7, DXGI_FORMAT_R32G32B32A32_UINT, 0, offsetof(Instance, kind), D3D11_INPUT_PER_INSTANCE_DATA, 1},
        };
        ok = ok && SUCCEEDED(dev_->CreateInputLayout(el, 8, vsb.data(), vsb.size(), &layout_));
        if (!ok) return false;

        // Premultiplied "over", the same equation as Vulkan's pipeline.
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.ScissorEnable = TRUE; rd.DepthClipEnable = TRUE;
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = 16; cb.Usage = D3D11_USAGE_DEFAULT; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev_->CreateBlendState(&bd, &blend_)) || FAILED(dev_->CreateRasterizerState(&rd, &raster_)) ||
            FAILED(dev_->CreateSamplerState(&sd, &sampler_)) || FAILED(dev_->CreateBuffer(&cb, nullptr, &viewport_cb_)))
            return false;
        // Placeholders so every slot is always bound to something valid.
        const std::uint8_t white[4] = {255, 255, 255, 255};
        return make_texture(1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, white, 4, white_tex_, white_srv_);
    }

    [[nodiscard]] bool create_swapchain() {
        if (create_composition_swapchain()) return true;
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 0; sd.Height = 0;   // the window's client size
        // Typeless backbuffers are not allowed for flip-model swapchains, so
        // the chain is UNORM and the shapes pass writes through an _SRGB
        // RTV of it -- legal for flip model, and exactly Vulkan's split:
        // linear-light blending for shapes, encoded-space blending for text.
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                   (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
        flags_ = sd.Flags;
        IDXGISwapChain1* sc = nullptr;
        if (FAILED(factory_->CreateSwapChainForHwnd(dev_, hwnd_, &sd, nullptr, nullptr, &sc))) return false;
        startup::mark("  d3d11 CreateSwapChainForHwnd");
        swapchain_ = sc;
        // Alt+Enter is the app's business (it has its own fullscreen).
        (void)factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
        IDXGISwapChain2* sc2 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&sc2)))) {
            sc2->SetMaximumFrameLatency(1);
            latency_wait_ = sc2->GetFrameLatencyWaitableObject();
            sc2->Release();
        }
        return make_targets();
    }

    // ── DirectComposition presentation ───────────────────────────────────────────────
    // What Zed's Windows renderer and Windows Terminal do: the swapchain is
    // created FOR COMPOSITION and set as the content of a DirectComposition
    // visual on the window, instead of being bound to the HWND. DWM then
    // composes window and content as one tree: a resize never stretches the
    // old buffers across the new frame (the HWND swapchain's DXGI_SCALING),
    // there is no redirection surface underneath to fall out of step with,
    // and the chain stays eligible for independent flip / MPO planes.
    // MAYAG_DCOMP=0 keeps the HWND swapchain (for A/B and old drivers).
    [[nodiscard]] bool create_composition_swapchain() {
        if (const char* e = std::getenv("MAYAG_DCOMP"); e != nullptr && *e == '0') return false;
        using CreateDevice = HRESULT(WINAPI*)(IDXGIDevice*, REFIID, void**);
        static const CreateDevice create = [] {
            const HMODULE m = ::LoadLibraryExW(L"dcomp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            return m ? reinterpret_cast<CreateDevice>(reinterpret_cast<void*>(::GetProcAddress(m, "DCompositionCreateDevice"))) : nullptr;
        }();
        if (create == nullptr) return false;
        RECT rc{};
        ::GetClientRect(hwnd_, &rc);
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = static_cast<UINT>(std::max<LONG>(rc.right - rc.left, 1));   // required: no HWND to size from
        sd.Height = static_cast<UINT>(std::max<LONG>(rc.bottom - rc.top, 1));
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Scaling = DXGI_SCALING_STRETCH;      // the only scaling composition chains take
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;  // opaque: flip-eligible
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                   (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
        IDXGIDevice* dxgi_dev = nullptr;
        if (FAILED(dev_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_dev)))) return false;
        IDCompositionDevice* cdev = nullptr;
        const bool have_dev = SUCCEEDED(create(dxgi_dev, __uuidof(IDCompositionDevice), reinterpret_cast<void**>(&cdev)));
        dxgi_dev->Release();
        if (!have_dev) return false;
        IDXGISwapChain1* sc = nullptr;
        IDCompositionTarget* target = nullptr;
        IDCompositionVisual* visual = nullptr;
        const bool ok = SUCCEEDED(factory_->CreateSwapChainForComposition(dev_, &sd, nullptr, &sc)) &&
                        SUCCEEDED(cdev->CreateTargetForHwnd(hwnd_, TRUE, &target)) &&
                        SUCCEEDED(cdev->CreateVisual(&visual)) && SUCCEEDED(visual->SetContent(sc)) &&
                        SUCCEEDED(target->SetRoot(visual)) && SUCCEEDED(cdev->Commit());
        if (!ok) {
            for (IUnknown* u : {static_cast<IUnknown*>(visual), static_cast<IUnknown*>(target), static_cast<IUnknown*>(sc),
                                static_cast<IUnknown*>(cdev)})
                if (u != nullptr) u->Release();
            return false;
        }
        startup::mark("  d3d11 CreateSwapChainForComposition");
        flags_ = sd.Flags;
        swapchain_ = sc;
        dcomp_dev_ = cdev; dcomp_target_ = target; dcomp_visual_ = visual;
        IDXGISwapChain2* sc2 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&sc2)))) {
            sc2->SetMaximumFrameLatency(1);
            latency_wait_ = sc2->GetFrameLatencyWaitableObject();
            sc2->Release();
        }
        return make_targets();
    }

    [[nodiscard]] bool make_targets() {
        ID3D11Texture2D* bb = nullptr;
        if (FAILED(swapchain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return false;
        D3D11_TEXTURE2D_DESC td{};
        bb->GetDesc(&td);
        cap_w_ = td.Width; cap_h_ = td.Height;
        sc_w_ = td.Width; sc_h_ = td.Height;   // reallocate() narrows it to the source region
        D3D11_RENDER_TARGET_VIEW_DESC rv{};
        rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rv.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        bool ok = SUCCEEDED(dev_->CreateRenderTargetView(bb, &rv, &rtv_srgb_));
        rv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (FAILED(dev_->CreateRenderTargetView(bb, &rv, &rtv_unorm_))) rtv_unorm_ = nullptr;   // text falls back to the shapes pass
        backbuffer_ = bb;
        return ok;
    }

    void release_targets() {
        using d3d11_detail::release;
        if (ctx_) ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        release(rtv_srgb_); release(rtv_unorm_); release(backbuffer_);
    }

    void resize_swapchain() {
        if (!swapchain_) return;
        RECT rc{};
        ::GetClientRect(hwnd_, &rc);
        // The size the window was TOLD (resize()): inside WM_NCCALCSIZE the
        // client rect still holds the old size -- the window moves after.
        const UINT w = logical_.x > 0.0f ? static_cast<UINT>(std::max(1.0f, std::round(logical_.x * dpi_)))
                                         : static_cast<UINT>(std::max<LONG>(rc.right - rc.left, 1));
        const UINT h = logical_.y > 0.0f ? static_cast<UINT>(std::max(1.0f, std::round(logical_.y * dpi_)))
                                         : static_cast<UINT>(std::max<LONG>(rc.bottom - rc.top, 1));
        if (w == cap_w_ && h == cap_h_) return;
        content_changed_ = true;
        // A plain reallocation, as Zed's composition swapchain does on WM_SIZE.
        // (A source region inside headroom buffers -- no reallocation -- was
        // measured twice, on HWND and on DirectComposition chains: DWM then
        // composes it ~83 ms behind at 30 fps. Rejected.)
        reallocate(w, h, w, h);
    }

    void reallocate(UINT bw, UINT bh, UINT w, UINT h) {
        release_targets();
        if (ctx_) ctx_->Flush();
        if (FAILED(swapchain_->ResizeBuffers(0, bw, bh, DXGI_FORMAT_UNKNOWN, flags_))) { valid_ = false; return; }
        (void)make_targets();
        if (dcomp_visual_ != nullptr && (bw != w || bh != h)) {
            IDXGISwapChain2* sc2 = nullptr;
            if (SUCCEEDED(swapchain_->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&sc2)))) {
                if (SUCCEEDED(sc2->SetSourceSize(w, h))) { sc_w_ = w; sc_h_ = h; }
                sc2->Release();
            }
        }
        content_changed_ = true;
        just_resized_ = true;
    }

    [[nodiscard]] bool make_texture(int w, int h, DXGI_FORMAT fmt, const void* data, UINT pitch,
                                    ID3D11Texture2D*& tex, ID3D11ShaderResourceView*& srv) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(w); td.Height = static_cast<UINT>(h);
        td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{data, pitch, 0};
        if (FAILED(dev_->CreateTexture2D(&td, data ? &sd : nullptr, &tex))) return false;
        return SUCCEEDED(dev_->CreateShaderResourceView(tex, nullptr, &srv));
    }

    void upload_instances(const DrawList& list) {
        const auto& insts = list.instances();
        const UINT bytes = static_cast<UINT>(std::max<std::size_t>(insts.size(), 1) * sizeof(Instance));
        if (bytes > instance_capacity_) {
            d3d11_detail::release(instance_buf_);
            instance_capacity_ = bytes * 2;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = instance_capacity_; bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(dev_->CreateBuffer(&bd, nullptr, &instance_buf_))) { instance_capacity_ = 0; return; }
        }
        if (insts.empty()) return;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx_->Map(instance_buf_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, insts.data(), insts.size() * sizeof(Instance));
            ctx_->Unmap(instance_buf_, 0);
        }
    }

    [[nodiscard]] ID3D11ShaderResourceView* srv_for(std::uint32_t id) const {
        if (auto it = textures_.find(id); it != textures_.end() && it->second.srv) return it->second.srv;
        return white_srv_;
    }

    [[nodiscard]] static D3D11_RECT clamp_scissor(const Rect& clip, UINT vw, UINT vh) {
        if (clip.size.x >= 1e8f || clip.size.y >= 1e8f || clip.size.x <= 0.0f || clip.size.y <= 0.0f)
            return {0, 0, static_cast<LONG>(vw), static_cast<LONG>(vh)};
        const LONG x0 = std::clamp(static_cast<LONG>(std::floor(clip.left())), 0L, static_cast<LONG>(vw));
        const LONG y0 = std::clamp(static_cast<LONG>(std::floor(clip.top())), 0L, static_cast<LONG>(vh));
        const LONG x1 = std::clamp(static_cast<LONG>(std::ceil(clip.right())), x0, static_cast<LONG>(vw));
        const LONG y1 = std::clamp(static_cast<LONG>(std::ceil(clip.bottom())), y0, static_cast<LONG>(vh));
        return {x0, y0, x1, y1};
    }

    [[nodiscard]] static D3D11_RECT backdrop_region(const Instance& in, UINT vw, UINT vh) {
        return clamp_scissor(Rect{in.rect.x, in.rect.y, in.rect.z, in.rect.w}.expand(kernel::backdrop_reach(in.params.x)), vw, vh);
    }

    /// Copy `reg` of the target into the mip-mapped capture texture and
    /// rebuild its mips (what a backdrop blur samples), like Vulkan does.
    void capture_region(D3D11_RECT reg) {
        if (capture_w_ != sc_w_ || capture_h_ != sc_h_ || capture_tex_ == nullptr) {
            d3d11_detail::release(capture_srv_); d3d11_detail::release(capture_tex_);
            D3D11_TEXTURE2D_DESC td{};
            td.Width = sc_w_; td.Height = sc_h_; td.MipLevels = 0; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;   // sampled back to LINEAR, as Vulkan's capture
            td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (FAILED(dev_->CreateTexture2D(&td, nullptr, &capture_tex_)) ||
                FAILED(dev_->CreateShaderResourceView(capture_tex_, nullptr, &capture_srv_))) return;
            capture_w_ = sc_w_; capture_h_ = sc_h_;
        }
        if (reg.right <= reg.left || reg.bottom <= reg.top) return;
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        const D3D11_BOX box{static_cast<UINT>(reg.left), static_cast<UINT>(reg.top), 0,
                            static_cast<UINT>(reg.right), static_cast<UINT>(reg.bottom), 1};
        ctx_->CopySubresourceRegion(capture_tex_, 0, static_cast<UINT>(reg.left), static_cast<UINT>(reg.top), 0,
                                    backbuffer_, 0, &box);
        ctx_->GenerateMips(capture_srv_);
    }

    // ── state ───────────────────────────────────────────────────────────

    HWND hwnd_ = nullptr;
    Vec2 logical_{};
    float dpi_ = 1.0f;
    bool valid_ = false, low_latency_ = false, tearing_ = false, resize_pending_ = false, content_changed_ = true;
    bool just_resized_ = false;
    bool resize_frame_ = false;    ///< the next submit is a synchronous resize frame (present_for_resize)
    UINT flags_ = 0, sc_w_ = 0, sc_h_ = 0, capture_w_ = 0, capture_h_ = 0, instance_capacity_ = 0;
    UINT cap_w_ = 0, cap_h_ = 0;   ///< the buffers' size
    int atlas_w_ = 0, atlas_h_ = 0;
    std::uint64_t frames_ = 0;
    char device_name_[128] = "d3d11";

    IDXGIFactory2* factory_ = nullptr;
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGISwapChain1* swapchain_ = nullptr;
    IDXGIOutput* intel_output_ = nullptr;   ///< an Intel iGPU's output: resize frames sync to its vblank
    IDCompositionDevice* dcomp_dev_ = nullptr;      ///< DirectComposition: the chain is a visual's content
    IDCompositionTarget* dcomp_target_ = nullptr;
    IDCompositionVisual* dcomp_visual_ = nullptr;
    HANDLE latency_wait_ = nullptr;
    ID3D11Texture2D* backbuffer_ = nullptr;
    ID3D11RenderTargetView* rtv_srgb_ = nullptr;
    ID3D11RenderTargetView* rtv_unorm_ = nullptr;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11PixelShader* ps_text_ = nullptr;
    ID3D11InputLayout* layout_ = nullptr;
    ID3D11Buffer* instance_buf_ = nullptr;
    ID3D11Buffer* viewport_cb_ = nullptr;
    ID3D11BlendState* blend_ = nullptr;
    ID3D11RasterizerState* raster_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11Texture2D *atlas_tex_ = nullptr, *color_tex_ = nullptr, *capture_tex_ = nullptr, *white_tex_ = nullptr;
    ID3D11ShaderResourceView *atlas_srv_ = nullptr, *color_srv_ = nullptr, *capture_srv_ = nullptr, *white_srv_ = nullptr;
    std::unordered_map<std::uint32_t, Texture> textures_;
    render::FramePlanner planner_;
};

}  // namespace mayag::backend

// A device owns its COM objects outright; moving it to the worker that
// brings it up (and back) leaves nothing the sender can still reach.
template <> inline constexpr bool jaal::sendable_opt_in<mayag::backend::D3D11Device> = true;

#endif  // _WIN32
