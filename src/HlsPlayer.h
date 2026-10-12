#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace app {

class Renderer;

// AirPlay-Video ("Wiedergabe auf ..."): Das iPhone uebergibt statt gespiegelter Bilder eine Video-Adresse
// (HLS-Playlist oder direkte URL). Abgespielt wird sie mit dem Windows-MediaPlayer; dessen Bilder landen per
// Frame-Server-Modus im Renderer. Alle oeffentlichen Methoden sind thread-sicher und blockieren nicht.
class HlsPlayer {
public:
    // Lokaler Pfad (z. B. "/master.m3u8") -> Playlist-Inhalt, fuer Videos, deren Playlists das iPhone selbst liefert.
    using Playlists = std::map<std::string, std::string>;

    struct Info {
        double duration = 0.0;   // Sekunden (0 = unbekannt)
        double position = -1.0;  // -1 = noch nicht verfuegbar
        double seekStart = 0.0;
        double seekDuration = 0.0;
        float rate = 0.0f;
        bool ready = false;
        bool bufferEmpty = true;
        bool bufferFull = false;
        bool finished = false;  // Video zu Ende oder Fehler: der Sender soll die Wiedergabe beenden
    };

    explicit HlsPlayer(Renderer& renderer);
    ~HlsPlayer();

    void Start();
    void Shutdown();

    // location: http(s)-URL; bei "http://localhost:..." kommen die Playlists aus `playlists`.
    void Play(const std::string& location, double startSeconds, Playlists playlists);
    void Stop();
    void SetRate(float rate);
    void Seek(double seconds);
    void SetVolumeDb(float db);

    Info GetInfo() const;
    bool IsActive() const;
    // Fehlermeldung der letzten Wiedergabe (leer = keine), fuer den Wartebildschirm.
    std::string LastError(uint64_t maxAgeMs) const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace app
