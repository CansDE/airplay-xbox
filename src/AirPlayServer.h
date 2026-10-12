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

// Kapselt den AirPlay-Empfaenger (UxPlay-Protokollbibliothek): mDNS-Ankuendigung, RTSP/HTTP-Server,
// Pairing, FairPlay, Spiegelung (Video) und Audio. Dekodierte Medien gehen an die Pipelines,
// AirPlay-Video ("Wiedergabe auf ...", HLS/URL) an den HlsPlayer.
class AirPlayServer {
public:
    AirPlayServer(VideoPipeline& video, AudioPipeline& audio, HlsPlayer& hls);
    ~AirPlayServer();

    // deviceId: "AA:BB:CC:DD:EE:FF"; keyfilePath: Datei fuer den dauerhaften Pairing-Schluessel.
    bool Start(const std::string& name, const std::string& deviceId, const std::string& keyfilePath,
               std::string& error);
    void Stop();
    bool IsRunning() const;

    int ConnectionCount() const;
    std::string ClientDescription() const;  // z. B. "iPhone von Alex (iPhone15,2)"; leer wenn niemand verbunden
    bool IsMirroring() const;               // true, solange ein Spiegelungs-Stream laeuft

    // Alle Verbindungen trennen (z. B. Controller-Taste B).
    void DisconnectClients();

    struct Impl;  // oeffentlich deklariert, damit die C-Callbacks in der .cpp darauf zugreifen koennen

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace app
