// UxPlay-Header zuerst: sie brauchen die POSIX-Luecken-Fixes (ssize_t, SOL_TCP, ...) vor allen anderen Includes.
#include "uxplay_win_fixes.h"

extern "C" {
#include "dnssd.h"
#include "logger.h"
#include "raop.h"
#include "stream.h"
}

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "AirPlayServer.h"
#include "AudioPipeline.h"
#include "HlsPlayer.h"
#include "Log.h"
#include "Sync.h"
#include "VideoPipeline.h"

namespace app {

namespace {
// Gleiche Port-Belegung wie "uxplay -p": TCP 7100/7000/7001, UDP 7011/6001/6000
unsigned short kTcpPorts[3] = {7100, 7000, 7001};
unsigned short kUdpPorts[3] = {7011, 6001, 6000};

// Features wie bei "uxplay -hls" (siehe uxplay.cpp start_dnssd): Spiegelung + Audio, dazu Bit 0 (Video) und
// Bit 4 (HTTP Live Streaming) fuer AirPlay-Video. H.265 (Bit 42) bleibt aus.
const int kFeatureBitsOn[] = {0, 1, 2, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 25, 28, 30};

// Systemsprache als BCP-47-Tag (z. B. "de-DE") fuer die Tonspur-/Untertitelwahl bei AirPlay-Video.
// Lautstaerke (AirPlay-dB, -30 = leise .. 0 = voll) bleibt ueber Verbindungen und Neustarts erhalten. Der Sender
// uebernimmt sie beim Verbinden aus "initialVolume" in GET /info; ohne gespeicherten Wert starten wir bei halber
// Lautstaerke statt voll. "Stumm" (-144) wird nicht gespeichert, sonst begaenne die naechste Sitzung lautlos.
constexpr double kDefaultVolumeDb = -15.0;

double LoadVolumeDb() {
    try {
        auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        if (auto stored = values.TryLookup(L"VolumeDb")) {
            double db = winrt::unbox_value<double>(stored);
            if (db > -30.0 && db <= 0.0) return db;
        }
    } catch (...) {
    }
    return kDefaultVolumeDb;
}

void SaveVolumeDb(double db) {
    if (db <= -30.0 || db > 0.0) return;
    try {
        winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values().Insert(L"VolumeDb",
                                                                                            winrt::box_value(db));
    } catch (...) {
    }
}

const char* SystemLanguage() {
    static char lang[LOCALE_NAME_MAX_LENGTH] = "";
    if (!lang[0]) {
        wchar_t name[LOCALE_NAME_MAX_LENGTH] = L"";
        if (!GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) ||
            !WideCharToMultiByte(CP_UTF8, 0, name, -1, lang, sizeof(lang), nullptr, nullptr)) {
            strcpy(lang, "de-DE");
        }
    }
    return lang;
}

bool ParseDeviceId(const std::string& id, char hw[6]) {
    unsigned b[6];
    if (sscanf(id.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
    for (int i = 0; i < 6; ++i) hw[i] = static_cast<char>(b[i]);
    return true;
}
}  // namespace

struct AirPlayServer::Impl {
    VideoPipeline& video;
    AudioPipeline& audio;
    HlsPlayer& hls;

    raop_t* raop = nullptr;
    dnssd_t* dnssd = nullptr;
    unsigned short port = 0;
    bool running = false;

    std::atomic<int> connections{0};
    std::atomic<bool> mirroring{false};  // Spiegelungs-Stream aktiv (unabhaengig davon, ob gerade Bilder kommen)
    mutable std::mutex mutex;            // schuetzt clientName
    std::string clientName;
    std::atomic<double> clientVolume{kDefaultVolumeDb};

    // Gemeinsamer Versatz Senderuhr -> lokale Uhr fuer Bild UND Ton (wie in uxplay.cpp: remote_clock_offset).
    // Wird beim ersten Paket einer Verbindung festgelegt, damit beide Stroeme dieselbe Zeitbasis haben.
    std::mutex clockMutex;
    bool clockValid = false;
    int64_t clockOffsetNs = 0;

    Impl(VideoPipeline& v, AudioPipeline& a, HlsPlayer& h) : video(v), audio(a), hls(h) {}

    // Sender-Zeitstempel (ntp_time_remote, ns) -> lokale Uhr (NowNs-Zeitbasis); 0 bleibt 0.
    uint64_t ToLocal(uint64_t remoteNs) {
        if (remoteNs == 0) return 0;
        std::lock_guard<std::mutex> lock(clockMutex);
        if (!clockValid) {
            clockOffsetNs = NowNs() - static_cast<int64_t>(remoteNs);
            clockValid = true;
            Log("AirPlay: Uhrversatz zum Sender festgelegt");
        }
        return static_cast<uint64_t>(static_cast<int64_t>(remoteNs) + clockOffsetNs);
    }

    void ResetClock() {
        std::lock_guard<std::mutex> lock(clockMutex);
        clockValid = false;
    }
};

namespace {

AirPlayServer::Impl* I(void* cls) { return static_cast<AirPlayServer::Impl*>(cls); }

// ---- Callbacks der UxPlay-Bibliothek (laufen auf deren Netzwerk-Threads) ----------------------------------------

void CbAudioProcess(void* cls, raop_ntp_t*, audio_decode_struct* d) {
    auto* s = I(cls);
    s->audio.Push(d->data, d->data_len, s->ToLocal(d->ntp_time_remote));
}

void CbVideoProcess(void* cls, raop_ntp_t*, video_decode_struct* d) {
    auto* s = I(cls);
    if (d->is_h265) return;  // Feature-Bit 42 ist aus; ein H.265-Stream wird nicht erwartet
    if (!s->mirroring.exchange(true)) Log("AirPlay: Spiegelung aktiv");
    s->video.Push(d->data, d->data_len, s->ToLocal(d->ntp_time_remote));
}

void CbVideoPause(void*) {}
void CbVideoResume(void*) {}
void CbConnFeedback(void*) {}

void CbConnInit(void* cls) {
    int n = ++I(cls)->connections;
    Log("AirPlay: Verbindung geoeffnet (%d aktiv)", n);
}

void CbConnDestroy(void* cls) {
    auto* s = I(cls);
    int n = --s->connections;
    Log("AirPlay: Verbindung geschlossen (%d aktiv)", n);
    if (n <= 0) {
        s->connections = 0;
        s->audio.StopStream();
        s->video.Flush();
        s->mirroring = false;
        s->ResetClock();
        if (s->hls.IsActive()) s->hls.Stop();
        std::lock_guard<std::mutex> lock(s->mutex);
        s->clientName.clear();
    }
}

void CbConnReset(void* cls, int reason) {
    Log("AirPlay: Verbindung zurueckgesetzt (Grund %d)", reason);
    I(cls)->video.Flush();
}

void CbVideoReset(void* cls, reset_type_t type) {
    auto* s = I(cls);
    Log("AirPlay: video_reset (Typ %d)", static_cast<int>(type));
    switch (type) {
    case RESET_TYPE_ON_VIDEO_PLAY:
        return;
    case RESET_TYPE_NOHOLD:  // neues Geraet uebernimmt: auch ein laufendes AirPlay-Video beenden
        s->hls.Stop();
        raop_destroy_airplay_video(s->raop, -1);
        break;
    case RESET_TYPE_HLS_SHUTDOWN:  // Video zu Ende oder abgebrochen (wie video_reset in uxplay.cpp)
        s->hls.Stop();
        raop_destroy_airplay_video(s->raop, -1);
        raop_remove_hls_connections(s->raop);
        break;
    case RESET_TYPE_HLS_EOS:
        s->hls.Stop();
        break;
    default:
        break;
    }
    s->video.Flush();
    s->mirroring = false;
}

void CbAudioFlush(void* cls) { I(cls)->audio.Flush(); }
void CbVideoFlush(void* cls) { I(cls)->video.Flush(); }

double CbAudioSetClientVolume(void* cls) { return I(cls)->clientVolume; }

void CbAudioSetVolume(void* cls, float volume) {
    I(cls)->clientVolume = volume;
    SaveVolumeDb(volume);
    I(cls)->audio.SetVolumeDb(volume);
    I(cls)->hls.SetVolumeDb(volume);
}

void CbAudioSetMetadata(void*, const void*, int) {}
void CbAudioSetCoverart(void*, const void*, int) {}
void CbAudioStopCoverart(void*) {}
void CbAudioRemoteControlId(void*, const char*, const char*) {}
void CbAudioSetProgress(void*, uint32_t*, uint32_t*, uint32_t*) {}

void CbAudioGetFormat(void* cls, unsigned char* ct, unsigned short* spf, bool* usingScreen, bool* isMedia,
                      uint64_t* audioFormat) {
    Log("AirPlay: Audioformat ct=%d spf=%d usingScreen=%d isMedia=%d format=0x%llx", *ct, *spf, *usingScreen ? 1 : 0,
        *isMedia ? 1 : 0, static_cast<unsigned long long>(*audioFormat));
    I(cls)->audio.StartStream(*ct);
}

void CbVideoReportSize(void*, float* wSrc, float* hSrc, float* w, float* h) {
    Log("AirPlay: Videogroesse Quelle %.0fx%.0f, Anzeige %.0fx%.0f", *wSrc, *hSrc, *w, *h);
}

void CbMirrorVideoRunning(void*, bool) {}

void CbReportClientRequest(void* cls, char* deviceid, char* model, char* name, bool* admit) {
    auto* s = I(cls);
    Log("AirPlay: Verbindungsanfrage von %s (%s), deviceID=%s", name, model, deviceid);
    *admit = true;
    std::lock_guard<std::mutex> lock(s->mutex);
    s->clientName = std::string(name ? name : "") + " (" + (model ? model : "") + ")";
}

void CbDisplayPin(void*, char*) {}
void CbRegisterClient(void*, const char*, const char*, const char*) {}
bool CbCheckRegister(void*, const char*) { return true; }

const char* CbPasswd(void*, int* len) {
    *len = 0;  // kein Passwort
    return nullptr;
}

void CbExportDacp(void*, const char*, const char*) {}

int CbVideoSetCodec(void*, video_codec_t codec) {
    if (codec == VIDEO_CODEC_H265) {
        Log("AirPlay: H.265 angefordert, wird nicht unterstuetzt");
        return -1;
    }
    return 0;
}

// ---- AirPlay-Video (HLS/URL): Steuerbefehle des Senders an den HlsPlayer ----------------------------------------

void CollectPlaylist(void* cls, const char* path, const char* playlist, int len) {
    (*static_cast<HlsPlayer::Playlists*>(cls))[path] = std::string(playlist, len);
}

void CbOnVideoPlay(void* cls, const char* location, const float startPosition) {
    auto* s = I(cls);
    std::string url = location ? location : "";
    Log("AirPlay: Video abspielen: %s (Start %.1f s)", url.c_str(), startPosition);
    HlsPlayer::Playlists playlists;
    if (url.rfind("http://localhost", 0) == 0) {
        // Die Playlists hat UxPlay per FCUP vom iPhone geholt; der Player bekommt sie direkt aus dem Speicher.
        int n = raop_hls_export_playlists(s->raop, CollectPlaylist, &playlists);
        Log("AirPlay: %d Playlists vom Sender uebernommen", n);
    }
    s->video.Flush();
    s->mirroring = false;
    s->hls.Play(url, startPosition, std::move(playlists));
}

void CbOnVideoScrub(void* cls, const float position) {
    Log("AirPlay: Video spulen auf %.1f s", position);
    I(cls)->hls.Seek(position);
}

void CbOnVideoRate(void* cls, const float rate) {
    Log("AirPlay: Video-Rate %.2f", rate);
    I(cls)->hls.SetRate(rate);
}

void CbOnVideoStop(void* cls) {
    Log("AirPlay: Video stoppen");
    I(cls)->hls.Stop();
}

void CbOnVideoAcquirePlaybackInfo(void* cls, playback_info_t* info) {
    HlsPlayer::Info i = I(cls)->hls.GetInfo();
    info->duration = i.duration;
    info->position = i.position;
    info->seek_start = i.seekStart;
    info->seek_duration = i.seekDuration;
    info->rate = i.rate;
    info->ready_to_play = i.ready;
    info->playback_buffer_empty = i.bufferEmpty;
    info->playback_buffer_full = i.bufferFull;
    info->playback_likely_to_keep_up = !i.bufferEmpty;
    if (i.finished) {
        // Signal an UxPlay: Video beendet -> video_reset(RESET_TYPE_HLS_SHUTDOWN)
        info->position = -1.0;
        info->duration = -1.0;
    }
}

float CbOnVideoPlaylistRemove(void* cls) {
    HlsPlayer::Info i = I(cls)->hls.GetInfo();
    I(cls)->hls.SetRate(0.0f);
    Log("AirPlay: Video aus der Playlist entfernt bei %.1f s", i.position);
    return i.position > 0 ? static_cast<float>(i.position) : 0.0f;
}

void CbLog(void*, int level, const char* msg) {
    if (level > LOGGER_DEBUG) return;
    Log("uxplay[%d]: %s", level, msg);
}

}  // namespace

AirPlayServer::AirPlayServer(VideoPipeline& video, AudioPipeline& audio, HlsPlayer& hls)
    : impl_(new Impl(video, audio, hls)) {}

AirPlayServer::~AirPlayServer() { Stop(); }

bool AirPlayServer::Start(const std::string& name, const std::string& deviceId, const std::string& keyfilePath,
                          std::string& error) {
    Impl& s = *impl_;
    if (s.running) return true;

    const double volume = LoadVolumeDb();
    s.clientVolume = volume;
    s.audio.SetVolumeDb(static_cast<float>(volume));
    s.hls.SetVolumeDb(static_cast<float>(volume));
    Log("AirPlay: Startlautstaerke %.1f dB", volume);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        error = "WSAStartup fehlgeschlagen";
        return false;
    }

    char hw[6];
    if (!ParseDeviceId(deviceId, hw)) {
        error = "Ungueltige Geraete-ID: " + deviceId;
        return false;
    }

    ntp_global_init();
    Log("AirPlay: ntp_global_init ok");

    int dnssdError = 0;
    s.dnssd = dnssd_init(name.c_str(), static_cast<int>(name.size()), hw, 6, 0, &dnssdError);
    if (!s.dnssd || dnssdError) {
        error = "dnssd_init fehlgeschlagen (Fehler " + std::to_string(dnssdError) + ")";
        s.dnssd = nullptr;
        return false;
    }
    for (int bit = 0; bit < 62; ++bit) {
        bool on = std::find(std::begin(kFeatureBitsOn), std::end(kFeatureBitsOn), bit) != std::end(kFeatureBitsOn);
        dnssd_set_airplay_features(s.dnssd, bit, on ? 1 : 0);
    }

    Log("AirPlay: dnssd_init + Features ok");
    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.cls = &s;
    cbs.audio_process = CbAudioProcess;
    cbs.video_process = CbVideoProcess;
    cbs.video_pause = CbVideoPause;
    cbs.video_resume = CbVideoResume;
    cbs.conn_feedback = CbConnFeedback;
    cbs.conn_reset = CbConnReset;
    cbs.video_reset = CbVideoReset;
    cbs.conn_init = CbConnInit;
    cbs.conn_destroy = CbConnDestroy;
    cbs.audio_flush = CbAudioFlush;
    cbs.video_flush = CbVideoFlush;
    cbs.audio_set_client_volume = CbAudioSetClientVolume;
    cbs.audio_set_volume = CbAudioSetVolume;
    cbs.audio_set_metadata = CbAudioSetMetadata;
    cbs.audio_set_coverart = CbAudioSetCoverart;
    cbs.audio_stop_coverart_rendering = CbAudioStopCoverart;
    cbs.audio_remote_control_id = CbAudioRemoteControlId;
    cbs.audio_set_progress = CbAudioSetProgress;
    cbs.audio_get_format = CbAudioGetFormat;
    cbs.video_report_size = CbVideoReportSize;
    cbs.mirror_video_running = CbMirrorVideoRunning;
    cbs.report_client_request = CbReportClientRequest;
    cbs.display_pin = CbDisplayPin;
    cbs.register_client = CbRegisterClient;
    cbs.check_register = CbCheckRegister;
    cbs.passwd = CbPasswd;
    cbs.export_dacp = CbExportDacp;
    cbs.video_set_codec = CbVideoSetCodec;
    cbs.on_video_play = CbOnVideoPlay;
    cbs.on_video_scrub = CbOnVideoScrub;
    cbs.on_video_rate = CbOnVideoRate;
    cbs.on_video_stop = CbOnVideoStop;
    cbs.on_video_acquire_playback_info = CbOnVideoAcquirePlaybackInfo;
    cbs.on_video_playlist_remove = CbOnVideoPlaylistRemove;

    s.raop = raop_init(&cbs);
    if (!s.raop) {
        error = "raop_init fehlgeschlagen";
        Stop();
        return false;
    }
    raop_set_log_callback(s.raop, CbLog, nullptr);
    raop_set_log_level(s.raop, LOGGER_DEBUG);
    Log("AirPlay: raop_init ok");

    // nohold = 1: ein neues Geraet darf die laufende Spiegelung uebernehmen.
    if (raop_init2(s.raop, 1, deviceId.c_str(), keyfilePath.c_str())) {
        error = "raop_init2 fehlgeschlagen (Schluesseldatei: " + keyfilePath + ")";
        Stop();
        return false;
    }
    raop_set_plist(s.raop, "width", 1920);
    raop_set_plist(s.raop, "height", 1080);
    raop_set_plist(s.raop, "refreshRate", 60);
    raop_set_plist(s.raop, "maxFPS", 60);
    raop_set_plist(s.raop, "hls", 1);  // AirPlay-Video annehmen (sonst ignoriert UxPlay /play usw.)
    raop_set_lang(s.raop, nullptr, nullptr, SystemLanguage());
    Log("AirPlay: raop_init2 + plist ok");
    raop_set_tcp_ports(s.raop, kTcpPorts);
    raop_set_udp_ports(s.raop, kUdpPorts);
    Log("AirPlay: Ports gesetzt");

    s.port = raop_get_port(s.raop);
    Log("AirPlay: raop_get_port=%u", s.port);
    if (raop_start_httpd(s.raop, &s.port) < 0) {
        error = "HTTP-Server konnte nicht gestartet werden (Port belegt?)";
        Stop();
        return false;
    }
    raop_set_port(s.raop, s.port);
    raop_set_dnssd(s.raop, s.dnssd);
    Log("AirPlay: httpd gestartet, dnssd gesetzt");

    if (dnssd_register_raop(s.dnssd, s.port) || dnssd_register_airplay(s.dnssd, s.port)) {
        error = "mDNS-Registrierung fehlgeschlagen (Multicast im Netzwerk blockiert?)";
        Stop();
        return false;
    }
    s.running = true;
    Log("AirPlay: Server \"%s\" laeuft auf Port %u (ID %s)", name.c_str(), s.port, deviceId.c_str());
    return true;
}

void AirPlayServer::Stop() {
    Impl& s = *impl_;
    // Einzelschritte protokollieren: ein Absturz beim Anhalten (App wird pausiert) laesst sich so eingrenzen.
    if (s.dnssd) {
        Log("AirPlay: Stop - mDNS abmelden");
        dnssd_unregister_raop(s.dnssd);
        dnssd_unregister_airplay(s.dnssd);
    }
    if (s.raop) {
        Log("AirPlay: Stop - raop_destroy");
        raop_destroy(s.raop);
        s.raop = nullptr;
    }
    if (s.dnssd) {
        Log("AirPlay: Stop - dnssd_destroy");
        dnssd_destroy(s.dnssd);
        s.dnssd = nullptr;
    }
    if (s.running) Log("AirPlay: Server gestoppt");
    s.running = false;
    s.connections = 0;
    s.audio.StopStream();
    s.video.Flush();
    s.hls.Stop();
}

bool AirPlayServer::IsRunning() const { return impl_->running; }

int AirPlayServer::ConnectionCount() const { return impl_->connections; }

std::string AirPlayServer::ClientDescription() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->clientName;
}

bool AirPlayServer::IsMirroring() const { return impl_->mirroring; }

void AirPlayServer::DisconnectClients() {
    Impl& s = *impl_;
    if (!s.raop || !s.running) return;
    Log("AirPlay: trenne alle Verbindungen");
    s.hls.Stop();
    raop_stop_httpd(s.raop);
    unsigned short p = s.port;
    raop_start_httpd(s.raop, &p);
}

}  // namespace app
