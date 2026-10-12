#include "HlsPlayer.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Media.Streaming.Adaptive.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <thread>

#include "Lang.h"
#include "Log.h"
#include "Renderer.h"

using winrt::check_hresult;
using winrt::com_ptr;
using winrt::hresult_error;
using winrt::hstring;
using winrt::to_hstring;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Core;
using namespace winrt::Windows::Media::Playback;
using namespace winrt::Windows::Media::Streaming::Adaptive;
using namespace winrt::Windows::Storage::Streams;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface;

namespace app {

namespace {

constexpr wchar_t kHlsContentType[] = L"application/vnd.apple.mpegurl";
constexpr double kTicksPerSecond = 10000000.0;  // TimeSpan counts in 100 ns units
// Some video servers (e.g. sites whose video is shared via AirPlay from the iPhone browser) only serve mobile
// browsers and answer Windows user agents with 403. That's why the player identifies itself as Safari on the iPhone.
constexpr wchar_t kUserAgent[] =
    L"Mozilla/5.0 (iPhone; CPU iPhone OS 18_7 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) "
    L"Version/26.0 Mobile/15E148 Safari/604.1";

std::string Narrow(const hstring& w) { return winrt::to_string(w); }

TimeSpan Seconds(double s) { return TimeSpan{static_cast<int64_t>(s * kTicksPerSecond)}; }
double ToSeconds(TimeSpan t) { return static_cast<double>(t.count()) / kTicksPerSecond; }

IBuffer MakeBuffer(const std::string& s) {
    Buffer buffer(static_cast<uint32_t>(s.size()));
    memcpy(buffer.data(), s.data(), s.size());
    buffer.Length(static_cast<uint32_t>(s.size()));
    return buffer;
}

bool IsLocal(const std::string& url) { return url.rfind("http://localhost", 0) == 0; }

// "http://localhost:7100/itag/1/x.m3u8?a=b" -> "/itag/1/x.m3u8?a=b"
std::string LocalPath(const std::string& url) {
    size_t hostStart = url.find("//");
    size_t slash = url.find('/', hostStart == std::string::npos ? 0 : hostStart + 2);
    return slash == std::string::npos ? "/" : url.substr(slash);
}

// Otherwise the Windows player starts at the lowest quality level and switches up after a few seconds, which
// stutters briefly. On a home network, start right away with the best level up to this limit.
constexpr uint32_t kMaxInitialBitrate = 10000000;

std::string Hex(uint32_t v) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08X", v);
    return buf;
}

}  // namespace

struct HlsPlayer::Impl {
    explicit Impl(Renderer& r) : renderer(r) {}

    Renderer& renderer;

    // ---- Command queue for the worker thread (MTA; MediaPlayer and sources live there) ----
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> commands;
    bool running = false;

    std::atomic<bool> active{false};
    bool showFrames = false;  // only under renderer.Lock()

    mutable std::mutex infoMutex;
    Info info;
    std::string lastError;
    uint64_t lastErrorTick = 0;

    // Worker thread only
    MediaPlayer player{nullptr};
    AdaptiveMediaSource adaptive{nullptr};
    float volumeLinear = 1.0f;
    int pollErrors = 0;
    std::set<std::string> failedProperties;
    uint64_t lastStatusLog = 0;

    // Only under renderer.Lock() (frame callback)
    IDirect3DSurface surface{nullptr};
    uint32_t surfaceVersion = 0;
    bool loggedFirstFrame = false;
    int copyErrors = 0;

    void Post(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            commands.push_back(std::move(fn));
        }
        cv.notify_one();
    }

    void Fail(const std::string& message) {
        Log("HLS: %s", message.c_str());
        std::lock_guard<std::mutex> lock(infoMutex);
        info.finished = true;
        lastError = message;
        lastErrorTick = GetTickCount64();
    }

    void Run() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        uint64_t lastPoll = 0;
        for (;;) {
            std::function<void()> cmd;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait_for(lock, std::chrono::milliseconds(250), [this] { return !commands.empty() || !running; });
                if (!running) break;
                if (!commands.empty()) {
                    cmd = std::move(commands.front());
                    commands.pop_front();
                }
            }
            if (cmd) {
                try {
                    cmd();
                } catch (hresult_error const& e) {
                    Fail(Tr("Fehler ", "Error ") + Hex(static_cast<uint32_t>(e.code())) + ": " + Narrow(e.message()));
                    StopAfterError();
                } catch (...) {
                    Fail(Tr("unbekannter Fehler", "unknown error"));
                    StopAfterError();
                }
            }
            uint64_t now = GetTickCount64();
            if (now - lastPoll >= 250) {
                lastPoll = now;
                try {
                    Poll();
                } catch (hresult_error const& e) {
                    if (pollErrors++ < 3) Log("HLS: status query failed %s", Hex(static_cast<uint32_t>(e.code())).c_str());
                } catch (...) {
                    if (pollErrors++ < 3) Log("HLS: status query failed");
                }
            }
        }
        try {
            DoStop();
        } catch (...) {
        }
        winrt::uninit_apartment();
    }

    // ---- Playback ------------------------------------------------------------------------------------------------

    void ConfigureAdaptive() {
        uint32_t best = 0, lowest = 0;
        for (uint32_t rate : adaptive.AvailableBitrates()) {
            if (rate <= kMaxInitialBitrate && rate > best) best = rate;
            if (!lowest || rate < lowest) lowest = rate;
        }
        if (!best) best = lowest;
        if (best) adaptive.InitialBitrate(best);
        Log("HLS: %u quality levels, starting at %u kbit/s", adaptive.AvailableBitrates().Size(), best / 1000);
        adaptive.PlaybackBitrateChanged([](AdaptiveMediaSource const&,
                                           AdaptiveMediaSourcePlaybackBitrateChangedEventArgs const& args) {
            Log("HLS: quality %u -> %u kbit/s", args.OldValue() / 1000, args.NewValue() / 1000);
        });
    }

    MediaSource CreateSource(const std::string& location, const Playlists& playlists) {
        Uri uri(winrt::to_hstring(location));

        if (IsLocal(location)) {
            // Playlists come from the iPhone (YouTube etc.): master as a stream, media playlists via
            // DownloadRequested. No loopback connection needed (blocked for UWP apps).
            auto master = playlists.find("/master.m3u8");
            if (master == playlists.end()) throw hresult_error(E_FAIL, Tr(L"Master-Playlist fehlt", L"Master playlist missing"));
            InMemoryRandomAccessStream stream;
            stream.WriteAsync(MakeBuffer(master->second)).get();
            stream.Seek(0);
            auto result = AdaptiveMediaSource::CreateFromStreamAsync(stream, uri, kHlsContentType).get();
            if (result.Status() != AdaptiveMediaSourceCreationStatus::Success) {
                throw hresult_error(E_FAIL, L"AdaptiveMediaSource (local) status " +
                                                to_hstring(static_cast<int>(result.Status())));
            }
            adaptive = result.MediaSource();
            ConfigureAdaptive();
            auto copy = std::make_shared<Playlists>(playlists);
            adaptive.DownloadRequested([copy](AdaptiveMediaSource const&,
                                              AdaptiveMediaSourceDownloadRequestedEventArgs const& args) {
                const std::string url = Narrow(args.ResourceUri().RawUri());
                if (!IsLocal(url)) return;  // the player downloads segments from the network itself
                auto it = copy->find(LocalPath(url));
                if (it == copy->end()) {
                    Log("HLS: requested playlist missing: %s", url.c_str());
                    return;
                }
                args.Result().Buffer(MakeBuffer(it->second));
                args.Result().ContentType(kHlsContentType);
            });
            return MediaSource::CreateFromAdaptiveMediaSource(adaptive);
        }

        if (location.find(".m3u8") != std::string::npos) {
            winrt::Windows::Web::Http::HttpClient http;
            http.DefaultRequestHeaders().UserAgent().TryParseAdd(kUserAgent);
            auto result = AdaptiveMediaSource::CreateFromUriAsync(uri, http).get();
            if (result.Status() == AdaptiveMediaSourceCreationStatus::Success) {
                adaptive = result.MediaSource();
                ConfigureAdaptive();
                return MediaSource::CreateFromAdaptiveMediaSource(adaptive);
            }
            int httpStatus = result.HttpResponseMessage() ? static_cast<int>(result.HttpResponseMessage().StatusCode()) : 0;
            Log("HLS: AdaptiveMediaSource Status %d (HTTP %d, %s)", static_cast<int>(result.Status()), httpStatus,
                Hex(static_cast<uint32_t>(result.ExtendedError())).c_str());
            if (httpStatus >= 400) {
                // Direct playback would fail at the same server, just with a meaningless message.
                throw hresult_error(E_ACCESSDENIED,
                                    Tr(L"Der Videoserver lehnt den Abruf ab (HTTP ",
                                       L"The video server refused the request (HTTP ") +
                                        to_hstring(httpStatus) +
                                        Tr(L"). Der Link ist vermutlich abgelaufen - Video in der App neu öffnen.",
                                           L"). The link has probably expired - reopen the video in the app."));
            }
            Log("HLS: trying direct playback");
        }
        return MediaSource::CreateFromUri(uri);
    }

    void DoPlay(const std::string& location, double startSeconds, const Playlists& playlists) {
        DoStop();
        Log("HLS: playing %s from %.1f s (%zu playlists from the sender)", location.c_str(), startSeconds, playlists.size());
        {
            std::lock_guard<std::mutex> lock(infoMutex);
            info = Info{};
            lastError.clear();
        }
        active = true;
        renderer.ResetVideoState();  // "loading video" idle screen until the first frame

        // Don't use MediaPlaybackItem(source, startTime): it trims the video, the position then counts from 0 at the
        // start point and the iPhone would get wrong positions reported (resuming jumps back).
        // Instead, open normally and then seek to the start position.
        MediaSource source = CreateSource(location, playlists);
        MediaPlaybackItem item(source);

        player = MediaPlayer();
        player.AutoPlay(false);
        player.Volume(volumeLinear);
        player.IsVideoFrameServerEnabled(true);
        player.VideoFrameAvailable([this](MediaPlayer const& sender, auto&&) { OnFrame(sender); });
        player.MediaOpened([startSeconds](MediaPlayer const& sender, auto&&) {
            auto s = sender.PlaybackSession();
            Log("HLS: media opened (%ux%u, duration %.1f s)", s.NaturalVideoWidth(), s.NaturalVideoHeight(),
                ToSeconds(s.NaturalDuration()));
            if (startSeconds > 0.5 && s.CanSeek()) {
                s.Position(Seconds(startSeconds));
                Log("HLS: seeking to start position %.1f s", startSeconds);
            }
        });
        player.MediaFailed([this](MediaPlayer const&, MediaPlayerFailedEventArgs const& args) {
            Fail(Tr("Wiedergabe fehlgeschlagen (Fehler ", "Playback failed (error ") + std::to_string(static_cast<int>(args.Error())) + ", " +
                 Hex(static_cast<uint32_t>(args.ExtendedErrorCode())) + "): " + Narrow(args.ErrorMessage()));
        });
        player.PlaybackSession().BufferingStarted([](auto&&, auto&&) { Log("HLS: buffering ..."); });
        player.PlaybackSession().BufferingEnded([](auto&&, auto&&) { Log("HLS: buffering finished"); });
        player.MediaEnded([this](MediaPlayer const&, auto&&) {
            Log("HLS: video ended");
            std::lock_guard<std::mutex> lock(infoMutex);
            info.finished = true;
        });
        {
            std::lock_guard<std::recursive_mutex> g(renderer.Lock());
            showFrames = true;
            loggedFirstFrame = false;
            copyErrors = 0;
        }
        player.Source(item);
        player.Play();
    }

    void DoStop() {
        {
            std::lock_guard<std::recursive_mutex> g(renderer.Lock());
            showFrames = false;
            surface = nullptr;
        }
        if (player) {
            double position = -1.0;
            try {
                position = ToSeconds(player.PlaybackSession().Position());
            } catch (...) {
            }
            Log("HLS: playback stopped at %.1f s", position);
            try {
                player.Pause();
                player.Source(nullptr);
                player.Close();
            } catch (...) {
            }
            player = nullptr;
        }
        adaptive = nullptr;
        active = false;
    }

    // "finished" stays set so the sender learns about the end on the next /playback-info.
    void StopAfterError() {
        try {
            DoStop();
        } catch (...) {
        }
    }

    void Poll() {
        if (!player) return;
        auto s = player.PlaybackSession();
        Info i;
        {
            std::lock_guard<std::mutex> lock(infoMutex);
            i = info;
        }
        const auto state = s.PlaybackState();
        i.ready = state == MediaPlaybackState::Playing || state == MediaPlaybackState::Paused ||
                  state == MediaPlaybackState::Buffering;
        // Depending on the source, individual values may return E_NOINTERFACE (e.g. with HLS); then a fallback is used
        // so that position and duration still reach the sender (otherwise "resume watching" jumps back).
        auto safe = [this](const char* name, auto fn, auto fallback) {
            try {
                return fn();
            } catch (...) {
                if (failedProperties.insert(name).second) Log("HLS: %s not available", name);
                return static_cast<decltype(fn())>(fallback);
            }
        };
        i.rate = state == MediaPlaybackState::Playing
                     ? static_cast<float>(safe("PlaybackRate", [&] { return s.PlaybackRate(); }, 1.0))
                     : 0.0f;
        i.bufferEmpty = state == MediaPlaybackState::Buffering || state == MediaPlaybackState::Opening;
        i.bufferFull = !i.bufferEmpty && safe("BufferingProgress", [&] { return s.BufferingProgress(); }, 1.0) >= 1.0;
        double duration = ToSeconds(safe("NaturalDuration", [&] { return s.NaturalDuration(); }, TimeSpan{}));
        if (duration < 0 || duration > 1e7) duration = 0;  // live stream: no fixed length
        i.duration = duration;
        i.position = i.ready ? ToSeconds(safe("Position", [&] { return s.Position(); }, TimeSpan{-10000000})) : -1.0;
        i.seekStart = 0;
        i.seekDuration = duration;
        try {
            auto ranges = s.GetSeekableRanges();
            if (ranges.Size() > 0) {
                double start = ToSeconds(ranges.GetAt(0).Start);
                double end = ToSeconds(ranges.GetAt(ranges.Size() - 1).End);
                i.seekStart = start;
                i.seekDuration = std::max(0.0, end - start);
            }
        } catch (...) {
        }
        const uint64_t now = GetTickCount64();
        if (now - lastStatusLog >= 10000) {
            lastStatusLog = now;
            Log("HLS: Status %d, Position %.1f / %.1f s, Rate %.1f", static_cast<int>(state), i.position, i.duration,
                i.rate);
        }
        std::lock_guard<std::mutex> lock(infoMutex);
        i.finished = info.finished;  // may have been set by MediaEnded/MediaFailed meanwhile
        info = i;
    }

    // ---- Frame output (MediaPlayer thread) -----------------------------------------------------------------------

    void OnFrame(MediaPlayer const& sender) {
        std::lock_guard<std::recursive_mutex> g(renderer.Lock());
        if (!showFrames) return;
        UINT w = 0, h = 0;
        uint32_t version = 0;
        ID3D11Texture2D* target = renderer.ExternalTarget(w, h, version);
        if (!target) return;
        try {
            if (!surface || surfaceVersion != version) {
                com_ptr<IDXGISurface> dxgi;
                if (FAILED(target->QueryInterface(IID_PPV_ARGS(dxgi.put())))) return;
                com_ptr<::IInspectable> inspectable;
                check_hresult(CreateDirect3D11SurfaceFromDXGISurface(dxgi.get(), inspectable.put()));
                surface = inspectable.as<IDirect3DSurface>();
                surfaceVersion = version;
            }
            auto session = sender.PlaybackSession();
            const double vw = session.NaturalVideoWidth(), vh = session.NaturalVideoHeight();
            Rect rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
            if (vw > 0 && vh > 0) {
                const double scale = std::min(w / vw, h / vh);
                rect.Width = static_cast<float>(vw * scale);
                rect.Height = static_cast<float>(vh * scale);
                rect.X = (w - rect.Width) / 2;
                rect.Y = (h - rect.Height) / 2;
            }
            sender.CopyFrameToVideoSurface(surface, rect);
            renderer.PresentExternal();
            if (!loggedFirstFrame) {
                loggedFirstFrame = true;
                Log("HLS: first frame shown (%.0fx%.0f)", vw, vh);
            }
        } catch (hresult_error const& e) {
            if (copyErrors++ < 5) Log("HLS: frame copy failed %s", Hex(static_cast<uint32_t>(e.code())).c_str());
        }
    }
};

HlsPlayer::HlsPlayer(Renderer& renderer) : impl_(new Impl(renderer)) {}

HlsPlayer::~HlsPlayer() { Shutdown(); }

void HlsPlayer::Start() {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.running) return;
    s.running = true;
    s.thread = std::thread([&s] { s.Run(); });
}

void HlsPlayer::Shutdown() {
    Impl& s = *impl_;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.running) return;
        s.running = false;
    }
    s.cv.notify_all();
    if (s.thread.joinable()) s.thread.join();
}

void HlsPlayer::Play(const std::string& location, double startSeconds, Playlists playlists) {
    Impl& s = *impl_;
    s.active = true;  // immediately, so the display does not jump back to "Ready"
    s.Post([&s, location, startSeconds, p = std::move(playlists)] { s.DoPlay(location, startSeconds, p); });
}

void HlsPlayer::Stop() {
    Impl& s = *impl_;
    s.Post([&s] { s.DoStop(); });
}

void HlsPlayer::SetRate(float rate) {
    Impl& s = *impl_;
    s.Post([&s, rate] {
        if (!s.player) return;
        if (rate == 0.0f) {
            s.player.Pause();
        } else {
            s.player.PlaybackSession().PlaybackRate(rate);
            s.player.Play();
        }
    });
}

void HlsPlayer::Seek(double seconds) {
    Impl& s = *impl_;
    s.Post([&s, seconds] {
        if (s.player) s.player.PlaybackSession().Position(Seconds(seconds));
    });
}

void HlsPlayer::SetVolumeDb(float db) {
    Impl& s = *impl_;
    const float linear = db <= -30.0f ? 0.0f : std::pow(10.0f, db / 20.0f);
    s.Post([&s, linear] {
        s.volumeLinear = linear;
        if (s.player) s.player.Volume(linear);
    });
}

HlsPlayer::Info HlsPlayer::GetInfo() const {
    std::lock_guard<std::mutex> lock(impl_->infoMutex);
    return impl_->info;
}

bool HlsPlayer::IsActive() const { return impl_->active; }

std::string HlsPlayer::LastError(uint64_t maxAgeMs) const {
    std::lock_guard<std::mutex> lock(impl_->infoMutex);
    if (impl_->lastError.empty() || GetTickCount64() - impl_->lastErrorTick > maxAgeMs) return {};
    return impl_->lastError;
}

}  // namespace app
