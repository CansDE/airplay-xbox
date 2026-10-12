#pragma once
#include <d2d1_1.h>
#include <d3d11_4.h>
#include <dwrite.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace app {

struct IdleInfo {
    std::wstring deviceName;
    std::wstring ip;
    std::wstring status;      // e.g. "Ready" / "Connected: iPhone"
    std::wstring hint;        // usage hint
};

// Owns the D3D11 device, swap chain and drawing paths. All public methods are thread-safe
// (the decode thread presents video, the UI thread draws the idle screen).
class Renderer {
public:
    // coreWindow: IUnknown* of the CoreWindow; width/height in pixels.
    bool Init(IUnknown* coreWindow, UINT width, UINT height);
    void Resize(UINT width, UINT height);

    ID3D11Device* Device() const { return device_.Get(); }
    std::recursive_mutex& Lock() { return lock_; }

    // Present a hardware-decoded NV12 frame (texture array slice).
    bool PresentDecoded(ID3D11Texture2D* tex, UINT slice, int srcW, int srcH);
    // Upload and present a software-decoded frame (I420).
    bool PresentI420(const uint8_t* y, int yStride, const uint8_t* u, int uStride, const uint8_t* v, int vStride,
                     int w, int h);

    void DrawIdle(const IdleInfo& info);

    // Target for frames from the Windows MediaPlayer (AirPlay video): window-sized BGRA texture, cleared to black.
    // version changes when the texture is recreated. The caller holds Lock() until PresentExternal().
    ID3D11Texture2D* ExternalTarget(UINT& width, UINT& height, uint32_t& version);
    void PresentExternal();

    // Time (GetTickCount64) of the last presented video frame; 0 = never.
    uint64_t LastVideoTick() const { return lastVideoTick_.load(); }
    void ResetVideoState();  // at stream end: discard the video processor, fill black

private:
    bool EnsureVideoProcessor(int srcW, int srcH);
    bool BlitToBackBuffer(ID3D11Texture2D* tex, UINT slice, int srcW, int srcH);
    bool CreateSwapchainResources();

    std::recursive_mutex lock_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> backRtv_;

    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> vp_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> vpOut_;
    int vpSrcW_ = 0, vpSrcH_ = 0;

    // Software upload
    Microsoft::WRL::ComPtr<ID3D11Texture2D> swStaging_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> swTexture_;
    int swW_ = 0, swH_ = 0;

    // Direct2D / DirectWrite (idle screen)
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2dContext_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwFactory_;

    // AirPlay video (MediaPlayer frame server)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> extTexture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> extRtv_;
    uint32_t extVersion_ = 0;

    UINT width_ = 0, height_ = 0;
    std::atomic<uint64_t> lastVideoTick_{0};
};

}  // namespace app
