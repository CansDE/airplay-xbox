#include "Renderer.h"

#include <algorithm>
#include <cstring>

#include "Lang.h"
#include "Log.h"

using Microsoft::WRL::ComPtr;

namespace app {

namespace {
bool Failed(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) return false;
    Log("Renderer: %s failed (0x%08X)", what, static_cast<unsigned>(hr));
    return true;
}
}  // namespace

bool Renderer::Init(IUnknown* coreWindow, UINT width, UINT height) {
    std::lock_guard<std::recursive_mutex> g(lock_);
    width_ = width;
    height_ = height;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                        D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (Failed(hr, "D3D11CreateDevice")) return false;

    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(device_.As(&mt))) mt->SetMultithreadProtected(TRUE);

    ComPtr<IDXGIDevice1> dxgiDevice;
    if (Failed(device_.As(&dxgiDevice), "IDXGIDevice1")) return false;
    dxgiDevice->SetMaximumFrameLatency(1);

    ComPtr<IDXGIAdapter> adapter;
    if (Failed(dxgiDevice->GetAdapter(&adapter), "GetAdapter")) return false;
    ComPtr<IDXGIFactory2> factory;
    if (Failed(adapter->GetParent(IID_PPV_ARGS(&factory)), "IDXGIFactory2")) return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (Failed(factory->CreateSwapChainForCoreWindow(device_.Get(), coreWindow, &desc, nullptr, &swapchain_),
               "CreateSwapChainForCoreWindow"))
        return false;

    if (!CreateSwapchainResources()) return false;

    // Video processor (may be missing on some devices -> idle screen only)
    if (SUCCEEDED(device_.As(&videoDevice_)) && SUCCEEDED(context_.As(&videoContext_))) {
        Log("Renderer: video processor available");
    } else {
        Log("Renderer: video processor NOT available");
    }

    // Direct2D / DirectWrite
    D2D1_FACTORY_OPTIONS opts{};
    if (Failed(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &opts,
                                 reinterpret_cast<void**>(d2dFactory_.GetAddressOf())),
               "D2D1CreateFactory"))
        return false;
    if (Failed(d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_), "ID2D1Factory1::CreateDevice")) return false;
    if (Failed(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_), "CreateDeviceContext"))
        return false;
    if (Failed(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwFactory_.GetAddressOf())),
               "DWriteCreateFactory"))
        return false;

    Log("Renderer: ready (%ux%u)", width_, height_);
    return true;
}

bool Renderer::CreateSwapchainResources() {
    backBuffer_.Reset();
    backRtv_.Reset();
    vpOut_.Reset();
    if (Failed(swapchain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer_)), "GetBuffer")) return false;
    if (Failed(device_->CreateRenderTargetView(backBuffer_.Get(), nullptr, &backRtv_), "CreateRenderTargetView"))
        return false;
    return true;
}

void Renderer::Resize(UINT width, UINT height) {
    std::lock_guard<std::recursive_mutex> g(lock_);
    if (!swapchain_ || width == 0 || height == 0 || (width == width_ && height == height_)) return;
    width_ = width;
    height_ = height;
    if (d2dContext_) d2dContext_->SetTarget(nullptr);
    backBuffer_.Reset();
    backRtv_.Reset();
    vpOut_.Reset();
    vp_.Reset();
    vpEnum_.Reset();
    vpSrcW_ = vpSrcH_ = 0;
    if (Failed(swapchain_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers")) return;
    CreateSwapchainResources();
}

bool Renderer::EnsureVideoProcessor(int srcW, int srcH) {
    if (!videoDevice_ || !videoContext_) return false;
    if (vp_ && vpOut_ && vpSrcW_ == srcW && vpSrcH_ == srcH) return true;

    vp_.Reset();
    vpEnum_.Reset();
    vpOut_.Reset();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = srcW;
    cd.InputHeight = srcH;
    cd.OutputWidth = width_;
    cd.OutputHeight = height_;
    cd.InputFrameRate = {60, 1};
    cd.OutputFrameRate = {60, 1};
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (Failed(videoDevice_->CreateVideoProcessorEnumerator(&cd, &vpEnum_), "CreateVideoProcessorEnumerator"))
        return false;
    if (Failed(videoDevice_->CreateVideoProcessor(vpEnum_.Get(), 0, &vp_), "CreateVideoProcessor")) return false;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
    od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (Failed(videoDevice_->CreateVideoProcessorOutputView(backBuffer_.Get(), vpEnum_.Get(), &od, &vpOut_),
               "CreateVideoProcessorOutputView"))
        return false;

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
    in.YCbCr_Matrix = 1;   // BT.709
    in.Nominal_Range = 1;  // 16-235
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
    out.Nominal_Range = 2;  // 0-255
    out.RGB_Range = 0;
    videoContext_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &in);
    videoContext_->VideoProcessorSetOutputColorSpace(vp_.Get(), &out);
    videoContext_->VideoProcessorSetStreamFrameFormat(vp_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vpSrcW_ = srcW;
    vpSrcH_ = srcH;
    Log("Renderer: video processor created for %dx%d", srcW, srcH);
    return true;
}

bool Renderer::BlitToBackBuffer(ID3D11Texture2D* tex, UINT slice, int srcW, int srcH) {
    if (!EnsureVideoProcessor(srcW, srcH)) return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC id{};
    id.FourCC = 0;
    id.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    id.Texture2D.MipSlice = 0;
    id.Texture2D.ArraySlice = slice;
    ComPtr<ID3D11VideoProcessorInputView> inView;
    if (Failed(videoDevice_->CreateVideoProcessorInputView(tex, vpEnum_.Get(), &id, &inView),
               "CreateVideoProcessorInputView"))
        return false;

    // Preserve aspect ratio (letterbox/pillarbox)
    double scale = std::min(static_cast<double>(width_) / srcW, static_cast<double>(height_) / srcH);
    LONG dw = static_cast<LONG>(srcW * scale), dh = static_cast<LONG>(srcH * scale);
    RECT dst{static_cast<LONG>((width_ - dw) / 2), static_cast<LONG>((height_ - dh) / 2), 0, 0};
    dst.right = dst.left + dw;
    dst.bottom = dst.top + dh;
    RECT src{0, 0, srcW, srcH};
    RECT full{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};

    const float black[4] = {0, 0, 0, 1};
    context_->ClearRenderTargetView(backRtv_.Get(), black);
    videoContext_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &src);
    videoContext_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &dst);
    videoContext_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &full);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inView.Get();
    if (Failed(videoContext_->VideoProcessorBlt(vp_.Get(), vpOut_.Get(), 0, 1, &stream), "VideoProcessorBlt"))
        return false;

    swapchain_->Present(1, 0);
    lastVideoTick_ = GetTickCount64();
    return true;
}

bool Renderer::PresentDecoded(ID3D11Texture2D* tex, UINT slice, int srcW, int srcH) {
    std::lock_guard<std::recursive_mutex> g(lock_);
    if (!swapchain_ || !tex) return false;
    return BlitToBackBuffer(tex, slice, srcW, srcH);
}

bool Renderer::PresentI420(const uint8_t* y, int yStride, const uint8_t* u, int uStride, const uint8_t* v,
                           int vStride, int w, int h) {
    std::lock_guard<std::recursive_mutex> g(lock_);
    if (!swapchain_) return false;
    const int tw = (w + 1) & ~1, th = (h + 1) & ~1;
    if (!swTexture_ || swW_ != tw || swH_ != th) {
        swTexture_.Reset();
        swStaging_.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = tw;
        td.Height = th;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (Failed(device_->CreateTexture2D(&td, nullptr, &swStaging_), "Staging-NV12")) return false;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.CPUAccessFlags = 0;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (Failed(device_->CreateTexture2D(&td, nullptr, &swTexture_), "Default-NV12")) return false;
        swW_ = tw;
        swH_ = th;
    }

    D3D11_MAPPED_SUBRESOURCE map{};
    if (Failed(context_->Map(swStaging_.Get(), 0, D3D11_MAP_WRITE, 0, &map), "Map NV12")) return false;
    auto* dst = static_cast<uint8_t*>(map.pData);
    for (int r = 0; r < h; ++r)
        memcpy(dst + static_cast<size_t>(r) * map.RowPitch, y + static_cast<size_t>(r) * yStride, w);
    uint8_t* uvDst = dst + static_cast<size_t>(map.RowPitch) * th;
    for (int r = 0; r < (h + 1) / 2; ++r) {
        uint8_t* row = uvDst + static_cast<size_t>(r) * map.RowPitch;
        const uint8_t* ur = u + static_cast<size_t>(r) * uStride;
        const uint8_t* vr = v + static_cast<size_t>(r) * vStride;
        for (int x = 0; x < (w + 1) / 2; ++x) {
            row[2 * x] = ur[x];
            row[2 * x + 1] = vr[x];
        }
    }
    context_->Unmap(swStaging_.Get(), 0);
    context_->CopyResource(swTexture_.Get(), swStaging_.Get());
    return BlitToBackBuffer(swTexture_.Get(), 0, w, h);
}

void Renderer::ResetVideoState() {
    std::lock_guard<std::recursive_mutex> g(lock_);
    vp_.Reset();
    vpEnum_.Reset();
    vpOut_.Reset();
    vpSrcW_ = vpSrcH_ = 0;
    swTexture_.Reset();
    swStaging_.Reset();
    swW_ = swH_ = 0;
    extRtv_.Reset();
    extTexture_.Reset();
    lastVideoTick_ = 0;
}

ID3D11Texture2D* Renderer::ExternalTarget(UINT& width, UINT& height, uint32_t& version) {
    std::lock_guard<std::recursive_mutex> g(lock_);
    if (!swapchain_ || !backBuffer_) return nullptr;
    D3D11_TEXTURE2D_DESC cur{};
    if (extTexture_) extTexture_->GetDesc(&cur);
    if (!extTexture_ || cur.Width != width_ || cur.Height != height_) {
        extRtv_.Reset();
        extTexture_.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width_;
        td.Height = height_;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (Failed(device_->CreateTexture2D(&td, nullptr, &extTexture_), "Video-Zieltextur")) return nullptr;
        if (Failed(device_->CreateRenderTargetView(extTexture_.Get(), nullptr, &extRtv_), "Video-Ziel-RTV")) {
            extTexture_.Reset();
            return nullptr;
        }
        ++extVersion_;
    }
    const float black[4] = {0, 0, 0, 1};
    context_->ClearRenderTargetView(extRtv_.Get(), black);
    width = width_;
    height = height_;
    version = extVersion_;
    return extTexture_.Get();
}

void Renderer::PresentExternal() {
    std::lock_guard<std::recursive_mutex> g(lock_);
    if (!swapchain_ || !backBuffer_ || !extTexture_) return;
    context_->CopyResource(backBuffer_.Get(), extTexture_.Get());
    swapchain_->Present(1, 0);
    lastVideoTick_ = GetTickCount64();
}

void Renderer::DrawIdle(const IdleInfo& info) {
    std::lock_guard<std::recursive_mutex> g(lock_);
    if (!swapchain_ || !d2dContext_ || !backBuffer_) return;

    ComPtr<IDXGISurface> surface;
    if (FAILED(backBuffer_.As(&surface))) return;
    D2D1_BITMAP_PROPERTIES1 bp =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    ComPtr<ID2D1Bitmap1> target;
    if (FAILED(d2dContext_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &target))) return;
    d2dContext_->SetTarget(target.Get());

    const float W = static_cast<float>(width_), H = static_cast<float>(height_);
    const float s = H / 1080.0f;

    ComPtr<ID2D1SolidColorBrush> white, grey, accent;
    d2dContext_->BeginDraw();
    d2dContext_->Clear(D2D1::ColorF(0.04f, 0.05f, 0.09f));
    d2dContext_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &white);
    d2dContext_->CreateSolidColorBrush(D2D1::ColorF(0.68f, 0.72f, 0.80f), &grey);
    d2dContext_->CreateSolidColorBrush(D2D1::ColorF(0.30f, 0.62f, 1.0f), &accent);

    auto text = [&](const std::wstring& str, float size, float y, ID2D1SolidColorBrush* brush,
                    DWRITE_FONT_WEIGHT weight) {
        ComPtr<IDWriteTextFormat> fmt;
        if (FAILED(dwFactory_->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                                DWRITE_FONT_STRETCH_NORMAL, size * s, L"de-de", &fmt)))
            return;
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        d2dContext_->DrawText(str.c_str(), static_cast<UINT32>(str.size()), fmt.Get(),
                              D2D1::RectF(0, y * s, W, (y + size * 1.6f) * s), brush);
    };

    text(L"AirPlay", 120, 230, white.Get(), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    text(info.deviceName, 64, 400, accent.Get(), DWRITE_FONT_WEIGHT_NORMAL);
    text(info.status, 44, 520, white.Get(), DWRITE_FONT_WEIGHT_NORMAL);
    text(info.hint, 34, 640, grey.Get(), DWRITE_FONT_WEIGHT_NORMAL);
    if (!info.ip.empty()) text(Tr(L"IP-Adresse: ", L"IP address: ") + info.ip, 30, 940, grey.Get(), DWRITE_FONT_WEIGHT_NORMAL);

    d2dContext_->EndDraw();
    d2dContext_->SetTarget(nullptr);
    swapchain_->Present(1, 0);
}

}  // namespace app
