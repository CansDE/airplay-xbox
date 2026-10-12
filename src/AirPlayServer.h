#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace app {

class VideoPipeline;
class AudioPipeline;
class HlsPlayer;

// Wraps the AirPlay receiver (UxPlay protocol library): mDNS advertisement, RTSP/HTTP server,
// pairing, FairPlay, mirroring (video) and audio. Decoded media goes to the pipelines,
// AirPlay video ("Play on ...", HLS/URL) to the HlsPlayer.
class AirPlayServer {
public:
    AirPlayServer(VideoPipeline& video, AudioPipeline& audio, HlsPlayer& hls);
    ~AirPlayServer();

    // deviceId: "AA:BB:CC:DD:EE:FF"; keyfilePath: file for the persistent pairing key.
    bool Start(const std::string& name, const std::string& deviceId, const std::string& keyfilePath,
               std::string& error);
    void Stop();
    bool IsRunning() const;

    int ConnectionCount() const;
    std::string ClientDescription() const;  // e.g. "Alex's iPhone (iPhone15,2)"; empty if nobody is connected
    bool IsMirroring() const;               // true while a mirroring stream is running

    // Disconnect all clients (e.g. controller button B).
    void DisconnectClients();

    struct Impl;  // declared public so the C callbacks in the .cpp can access it

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace app
