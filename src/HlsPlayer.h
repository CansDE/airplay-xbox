#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace app {

class Renderer;

// AirPlay video ("Play on ..."): instead of mirrored frames, the iPhone hands over a video URL
// (HLS playlist or direct URL). It is played with the Windows MediaPlayer, whose frames reach the renderer
// via frame server mode. All public methods are thread-safe and non-blocking.
class HlsPlayer {
public:
    // Local path (e.g. "/master.m3u8") -> playlist content, for videos whose playlists the iPhone supplies itself.
    using Playlists = std::map<std::string, std::string>;

    struct Info {
        double duration = 0.0;   // seconds (0 = unknown)
        double position = -1.0;  // -1 = not available yet
        double seekStart = 0.0;
        double seekDuration = 0.0;
        float rate = 0.0f;
        bool ready = false;
        bool bufferEmpty = true;
        bool bufferFull = false;
        bool finished = false;  // video ended or failed: the sender should stop playback
    };

    explicit HlsPlayer(Renderer& renderer);
    ~HlsPlayer();

    void Start();
    void Shutdown();

    // location: http(s) URL; for "http://localhost:..." the playlists come from `playlists`.
    void Play(const std::string& location, double startSeconds, Playlists playlists);
    void Stop();
    void SetRate(float rate);
    void Seek(double seconds);
    void SetVolumeDb(float db);

    Info GetInfo() const;
    bool IsActive() const;
    // Error message of the last playback (empty = none), for the idle screen.
    std::string LastError(uint64_t maxAgeMs) const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace app
