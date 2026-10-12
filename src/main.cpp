// AirPlay-Empfaenger fuer die Xbox: UWP-App ohne XAML (CoreApplication + D3D11 direkt auf dem CoreWindow).
#include <windows.h>

#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Display.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include "AirPlayServer.h"
#include "AudioPipeline.h"
#include "HlsPlayer.h"
#include "Log.h"
#include "Renderer.h"
#include "VideoPipeline.h"

using namespace winrt;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Core;

namespace {

// Diagnose: schreibt sehr frueh (noch vor LogInit) Marker in <Package>\AC\Temp\airplay_boot.log
void Boot(const char* msg) {
    wchar_t path[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, path);
    if (n == 0 || n > MAX_PATH - 32) return;
    wcscat_s(path, L"airplay_boot.log");
    HANDLE h = CreateFile2(path, FILE_APPEND_DATA, FILE_SHARE_READ, OPEN_ALWAYS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char line[256];
    int len = snprintf(line, sizeof(line), "%s\r\n", msg);
    DWORD written;
    WriteFile(h, line, static_cast<DWORD>(len), &written, nullptr);
    CloseHandle(h);
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Stabile, zufaellige "MAC-Adresse" als AirPlay-Geraete-ID (einmal erzeugt, dann gespeichert).
std::string LoadOrCreateDeviceId() {
    auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
    if (auto stored = values.TryLookup(L"DeviceId")) {
        return Narrow(unbox_value<hstring>(stored).c_str());
    }
    std::random_device rd;
    unsigned char b[6];
    for (auto& x : b) x = static_cast<unsigned char>(rd());
    b[0] = static_cast<unsigned char>((b[0] & 0xFE) | 0x02);  // lokal verwaltet, Unicast
    char buf[32];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
    values.Insert(L"DeviceId", box_value(to_hstring(buf)));
    return buf;
}

// Anzeigename: optional aus LocalFolder\name.txt, sonst "Xbox Series X".
std::string LoadDeviceName(const std::wstring& folder) {
    FILE* f = _wfopen((folder + L"\\name.txt").c_str(), L"rb");
    if (f) {
        char buf[128] = {};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        std::string s(buf, n);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
        if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF) s.erase(0, 3);  // UTF-8-BOM
        if (!s.empty()) return s;
    }
    return "Xbox Series X";
}

std::wstring FindLocalIp() {
    try {
        for (auto const& host : winrt::Windows::Networking::Connectivity::NetworkInformation::GetHostNames()) {
            if (host.Type() == winrt::Windows::Networking::HostNameType::Ipv4 && host.IPInformation() &&
                host.IPInformation().NetworkAdapter()) {
                return host.DisplayName().c_str();
            }
        }
    } catch (...) {
    }
    return L"";
}

struct App : implements<App, IFrameworkViewSource, IFrameworkView> {
    // ---- IFrameworkViewSource / IFrameworkView ----
    IFrameworkView CreateView() { return *this; }

    void Initialize(CoreApplicationView const& view) {
        Boot("Initialize");
        view.Activated([](auto&&, auto&&) { CoreWindow::GetForCurrentThread().Activate(); });
        suspendingToken_ = CoreApplication::Suspending([this](auto&&, auto&&) {
            app::Log("App: Suspending");
            if (server_) server_->Stop();
            if (hls_) hls_->Stop();
            serverStarted_ = false;
        });
        resumingToken_ = CoreApplication::Resuming([this](auto&&, auto&&) {
            app::Log("App: Resuming");
            nextServerTry_ = 0;  // sofort neu starten
        });
    }

    void Load(hstring const&) {}
    void Uninitialize() {}

    void SetWindow(CoreWindow const& window) {
        Boot("SetWindow");
        window_ = window;
        try {
            winrt::Windows::UI::ViewManagement::ApplicationView::GetForCurrentView().SetDesiredBoundsMode(
                winrt::Windows::UI::ViewManagement::ApplicationViewBoundsMode::UseCoreWindow);
        } catch (...) {
        }
        window.Closed([this](auto&&, auto&&) { closed_ = true; });
        window.SizeChanged([this](auto&&, auto&&) { resizePending_ = true; });
        window.KeyDown([this](auto&&, KeyEventArgs const& args) {
            if (args.VirtualKey() == winrt::Windows::System::VirtualKey::GamepadB && server_) {
                server_->DisconnectClients();
            }
        });
    }

    void Run() {
        Boot("Run");
        auto folder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
        localFolder_ = folder.c_str();
        app::LogInit(localFolder_);
        app::InstallCrashDiagnostics();
        app::Log("==== AirPlay Xbox startet ====");

        Boot("vor Activate");
        window_.Activate();
        Boot("nach Activate");

        renderer_ = std::make_unique<app::Renderer>();
        UINT w, h;
        WindowPixelSize(w, h);
        if (!renderer_->Init(winrt::get_unknown(window_), w, h)) {
            app::Log("Renderer konnte nicht initialisiert werden - Ende");
            return;
        }
        video_ = std::make_unique<app::VideoPipeline>(*renderer_);
        audio_ = std::make_unique<app::AudioPipeline>();
        audio_->Init();
        video_->Start();
        hls_ = std::make_unique<app::HlsPlayer>(*renderer_);
        hls_->Start();
        server_ = std::make_unique<app::AirPlayServer>(*video_, *audio_, *hls_);

        deviceName_ = LoadDeviceName(localFolder_);
        deviceId_ = LoadOrCreateDeviceId();
        app::Log("Geraet: \"%s\" ID %s", deviceName_.c_str(), deviceId_.c_str());

        displayRequest_ = winrt::Windows::System::Display::DisplayRequest();
        auto dispatcher = window_.Dispatcher();

        uint64_t lastIpCheck = 0;
        while (!closed_) {
            dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            if (closed_) break;
            uint64_t now = GetTickCount64();

            if (resizePending_.exchange(false)) {
                UINT nw, nh;
                WindowPixelSize(nw, nh);
                renderer_->Resize(nw, nh);
            }
            if (!serverStarted_ && now >= nextServerTry_) {
                std::string err;
                serverStarted_ = server_->Start(deviceName_, deviceId_, Narrow(localFolder_ + L"\\airplay_key.pem"), err);
                if (!serverStarted_) {
                    lastError_ = err;
                    app::Log("Server-Start fehlgeschlagen: %s (neuer Versuch in 5 s)", err.c_str());
                    server_->Stop();
                    nextServerTry_ = now + 5000;
                } else {
                    lastError_.clear();
                }
            }
            if (now - lastIpCheck > 5000) {
                ip_ = FindLocalIp();
                lastIpCheck = now;
            }

            // Solange gespiegelt wird, bleibt das letzte Bild stehen - auch wenn das iPhone bei ruhigem Bildschirm
            // eine Weile keine neuen Bilder schickt. Der Wartebildschirm kommt erst nach dem Ende der Spiegelung
            // bzw. des AirPlay-Videos.
            const bool streaming = serverStarted_ && (server_->IsMirroring() || hls_->IsActive());
            const uint64_t lastVideo = renderer_->LastVideoTick();
            const bool showingVideo = streaming && lastVideo != 0;
            SetScreenAwake(showingVideo);
            if (!showingVideo) {
                if (lastVideo != 0 && !streaming) renderer_->ResetVideoState();
                renderer_->DrawIdle(BuildIdleInfo());
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
        }

        app::Log("App: beende");
        CoreApplication::Suspending(suspendingToken_);
        CoreApplication::Resuming(resumingToken_);
        server_->Stop();
        hls_->Shutdown();
        video_->Stop();
        audio_->Shutdown();
        SetScreenAwake(false);
    }

private:
    void WindowPixelSize(UINT& w, UINT& h) {
        auto b = window_.Bounds();
        float scale = 1.0f;
        try {
            scale = static_cast<float>(winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel());
        } catch (...) {
        }
        w = static_cast<UINT>(b.Width * scale + 0.5f);
        h = static_cast<UINT>(b.Height * scale + 0.5f);
        if (w == 0 || h == 0) {
            w = 1920;
            h = 1080;
        }
    }

    void SetScreenAwake(bool awake) {
        if (awake == awake_) return;
        try {
            if (awake) {
                displayRequest_.RequestActive();
            } else {
                displayRequest_.RequestRelease();
            }
            awake_ = awake;
        } catch (...) {
        }
    }

    app::IdleInfo BuildIdleInfo() {
        app::IdleInfo info;
        info.deviceName = Widen(deviceName_);
        info.ip = ip_;
        if (!lastError_.empty()) {
            info.status = L"Fehler: " + Widen(lastError_);
            info.hint = L"Der Start wird automatisch wiederholt. Ist die Xbox mit dem Heimnetz verbunden?";
        } else if (!serverStarted_) {
            info.status = L"Starte AirPlay ...";
        } else if (hls_->IsActive()) {
            info.status = L"AirPlay-Video wird geladen ...";
            info.hint = L"Steuerung über das iPhone (Pause, Spulen, Beenden).";
        } else if (std::string err = hls_->LastError(15000); !err.empty()) {
            info.status = L"Video konnte nicht abgespielt werden";
            info.hint = Widen(err);
        } else if (server_->ConnectionCount() > 0) {
            std::wstring client = Widen(server_->ClientDescription());
            info.status = client.empty() ? L"Verbunden - warte auf Bild" : L"Verbunden: " + client;
            info.hint = L"Starte die Bildschirmsynchronisierung oder gib Audio wieder.";
        } else {
            info.status = L"Bereit - wartet auf Verbindung";
            info.hint = L"iPhone/Mac: Kontrollzentrum → Bildschirmsynchronisierung → „" + Widen(deviceName_) +
                        L"“";
        }
        return info;
    }

    CoreWindow window_{nullptr};
    std::atomic<bool> closed_{false};
    std::atomic<bool> resizePending_{false};
    event_token suspendingToken_{};
    event_token resumingToken_{};

    std::wstring localFolder_;
    std::string deviceName_, deviceId_, lastError_;
    std::wstring ip_;
    bool serverStarted_ = false;
    uint64_t nextServerTry_ = 0;
    bool awake_ = false;

    std::unique_ptr<app::Renderer> renderer_;
    std::unique_ptr<app::VideoPipeline> video_;
    std::unique_ptr<app::AudioPipeline> audio_;
    std::unique_ptr<app::HlsPlayer> hls_;
    std::unique_ptr<app::AirPlayServer> server_;
    winrt::Windows::System::Display::DisplayRequest displayRequest_{nullptr};
};

}  // namespace

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    Boot("wWinMain");
    CoreApplication::Run(make<App>());
    Boot("Run beendet");
    return 0;
}
