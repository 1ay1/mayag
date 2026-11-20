#pragma once
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

#if defined(_WIN32)

#include "../core/win32.hpp"
#include "../core/startup.hpp"
#include "../render/draw_list.hpp"
#include "../render/frame_plan.hpp"
#include "../render/kernel.hpp"
#include "../render/shader_generated.hpp"
#include "backend.hpp"   // AtlasView / view_of

#include <d3d11.h>
#include <d3dcompiler.h>
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
    pD3DCompile compile = nullptr;
    HRESULT(WINAPI* create_blob)(SIZE_T, ID3DBlob**) = nullptr;
    bool ok = false;

    static const Api& get() {
        static const Api a = [] {
            Api r;
            const auto sys = [](const wchar_t* n) { return ::LoadLibraryExW(n, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32); };
            const HMODULE d3d = sys(L"d3d11.dll"), dxgi = sys(L"dxgi.dll"), comp = sys(L"d3dcompiler_47.dll");
            if (!d3d || !dxgi || !comp) return r;
            r.create_device = reinterpret_cast<decltype(r.create_device)>(reinterpret_cast<void*>(::GetProcAddress(d3d, "D3D11CreateDevice")));
            r.create_factory2 = reinterpret_cast<decltype(r.create_factory2)>(reinterpret_cast<void*>(::GetProcAddress(dxgi, "CreateDXGIFactory2")));
            r.compile = reinterpret_cast<pD3DCompile>(reinterpret_cast<void*>(::GetProcAddress(comp, "D3DCompile")));
            r.create_blob = reinterpret_cast<decltype(r.create_blob)>(reinterpret_cast<void*>(::GetProcAddress(comp, "D3DCreateBlob")));
            r.ok = r.create_device && r.create_factory2 && r.compile && r.create_blob;
            return r;
        }();
        return a;
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
        const char* f = std::getenv("MAYAG_PRESENT");
        if (f != nullptr && std::string_view{f} == "immediate") on = true;
        if (f != nullptr && std::string_view{f} == "fifo") on = false;
        low_latency_ = on;
    }
    void set_transparent(bool) noexcept {}   // DirectComposition transparency: not yet

    /// Device + swapchain on `hwnd`. Safe to call from a worker thread: DXGI
    /// swapchains are not tied to the window's thread (only Present's message
    /// pumping is, and ResizeBuffers is called from the UI thread).
    [[nodiscard]] bool attach(HWND hwnd, Vec2 logical, float dpi) {
        const d3d11_detail::Api& api = d3d11_detail::Api::get();
        if (!api.ok || hwnd == nullptr) return false;
        hwnd_ = hwnd;
        logical_ = logical;
        dpi_ = dpi;

        IDXGIFactory2* f2 = nullptr;
        if (FAILED(api.create_factory2(0, __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&f2)))) return false;
        factory_ = f2;
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
        HRESULT hr = api.create_device(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
        if (adapter) {
            DXGI_ADAPTER_DESC1 d{};
            if (SUCCEEDED(adapter->GetDesc1(&d)))
                ::WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, device_name_, sizeof device_name_, nullptr, nullptr);
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
        release(swapchain_);
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
        // Wait for the swapchain to want a frame (latency 1): the frame is
        // built from the freshest input instead of queuing behind vsync.
        // Not right after a resize: the chain's buffers are new, nothing is
        // queued, and a live drag-resize must not stall a refresh per step.
        if (latency_wait_ && !just_resized_) ::WaitForSingleObjectEx(latency_wait_, 100, TRUE);
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
        const HRESULT hr = swapchain_->Present(immediate ? 0 : 1, immediate ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) valid_ = false;
        content_changed_ = false;
        ++frames_;
    }

    [[nodiscard]] std::uint64_t frames_presented() const noexcept { return frames_; }

  private:
    struct Texture { ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0; };

    // ── setup ───────────────────────────────────────────────────────────

    [[nodiscard]] bool compile(std::string_view src, const char* target, const D3D_SHADER_MACRO* defs, ID3DBlob** out) {
        // d3dcompiler takes ~200 ms for the pixel shader -- more than device
        // creation. The bytecode depends only on the source, the target and
        // the defines, so it is cached under %LOCALAPPDATA%\mayag keyed by a
        // hash of all three; a stale or corrupt cache entry just recompiles.
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
        const auto& api = d3d11_detail::Api::get();
        if (cacheable) {
            const HANDLE f = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER sz{};
                bool ok = ::GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < (16 << 20) &&
                          SUCCEEDED(api.create_blob(static_cast<SIZE_T>(sz.QuadPart), out));
                DWORD got = 0;
                ok = ok && ::ReadFile(f, (*out)->GetBufferPointer(), static_cast<DWORD>(sz.QuadPart), &got, nullptr) &&
                     got == sz.QuadPart;
                ::CloseHandle(f);
                if (ok) return true;
                d3d11_detail::release(*out);
            }
        }
        ID3DBlob* err = nullptr;
        const HRESULT hr = api.compile(src.data(), src.size(), "mayag", defs, nullptr, "main", target,
                                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
        if (FAILED(hr) && err) std::fprintf(stderr, "mayag d3d11: %s\n", static_cast<const char*>(err->GetBufferPointer()));
        d3d11_detail::release(err);
        if (SUCCEEDED(hr) && cacheable) {
            // Write-then-rename, so a concurrent reader never sees half a file.
            wchar_t tmp[MAX_PATH];
            std::swprintf(tmp, MAX_PATH, L"%ls.%lu.tmp", path, ::GetCurrentProcessId());
            const HANDLE f = ::CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD put = 0;
                const bool ok = ::WriteFile(f, (*out)->GetBufferPointer(), static_cast<DWORD>((*out)->GetBufferSize()), &put, nullptr);
                ::CloseHandle(f);
                if (!ok || !::MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) ::DeleteFileW(tmp);
            }
        }
        return SUCCEEDED(hr);
    }

    [[nodiscard]] bool create_pipeline() {
        ID3DBlob *vsb = nullptr, *psb = nullptr, *ptb = nullptr;
        const D3D_SHADER_MACRO text_pass[] = {{"SPIRV_CROSS_CONSTANT_ID_0", "true"}, {nullptr, nullptr}};
        if (!compile(render::generated::hlsl_vertex, "vs_5_0", nullptr, &vsb) ||
            !compile(render::generated::hlsl_fragment, "ps_5_0", nullptr, &psb) ||
            !compile(render::generated::hlsl_fragment, "ps_5_0", text_pass, &ptb)) {
            d3d11_detail::release(vsb); d3d11_detail::release(psb); d3d11_detail::release(ptb);
            return false;
        }
        bool ok = SUCCEEDED(dev_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_)) &&
                  SUCCEEDED(dev_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_)) &&
                  SUCCEEDED(dev_->CreatePixelShader(ptb->GetBufferPointer(), ptb->GetBufferSize(), nullptr, &ps_text_));
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
        ok = ok && SUCCEEDED(dev_->CreateInputLayout(el, 8, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout_));
        d3d11_detail::release(vsb); d3d11_detail::release(psb); d3d11_detail::release(ptb);
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

    [[nodiscard]] bool make_targets() {
        ID3D11Texture2D* bb = nullptr;
        if (FAILED(swapchain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return false;
        D3D11_TEXTURE2D_DESC td{};
        bb->GetDesc(&td);
        sc_w_ = td.Width; sc_h_ = td.Height;
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
        release_targets();
        if (ctx_) ctx_->Flush();
        RECT rc{};
        ::GetClientRect(hwnd_, &rc);
        const UINT w = static_cast<UINT>(std::max<LONG>(rc.right - rc.left, 1));
        const UINT h = static_cast<UINT>(std::max<LONG>(rc.bottom - rc.top, 1));
        if (FAILED(swapchain_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, flags_))) { valid_ = false; return; }
        (void)make_targets();
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
    UINT flags_ = 0, sc_w_ = 0, sc_h_ = 0, capture_w_ = 0, capture_h_ = 0, instance_capacity_ = 0;
    int atlas_w_ = 0, atlas_h_ = 0;
    std::uint64_t frames_ = 0;
    char device_name_[128] = "d3d11";

    IDXGIFactory2* factory_ = nullptr;
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGISwapChain1* swapchain_ = nullptr;
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
