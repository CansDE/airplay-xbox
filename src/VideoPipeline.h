#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

struct AVCodecContext;
struct AVBufferRef;

namespace app {

class Renderer;

// H.264 (Annex-B) -> Decoder-Thread -> Renderer. Bevorzugt D3D11VA, sonst Software-Decoder.
class VideoPipeline {
public:
    explicit VideoPipeline(Renderer& renderer);
    ~VideoPipeline();

    void Start();
    void Stop();

    // Wird vom AirPlay-Netzwerkthread aufgerufen; kopiert die Daten. ptsNs = Sender-Zeitstempel in lokaler Uhr
    // (0 = unbekannt); das Bild wird zu ptsNs + kPlayoutDelayNs angezeigt, passend zum Ton.
    void Push(const uint8_t* annexB, int size, uint64_t ptsNs);
    // Verwirft wartende Pakete und leert den Decoder (z. B. bei Verbindungswechsel).
    void Flush();

    bool UsesHardware() const { return hardware_.load(); }

private:
    void Run();
    bool OpenDecoder(bool hardware);
    void CloseDecoder();
    void Decode(const std::vector<uint8_t>& packet);

    Renderer& renderer_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> flushRequested_{false};
    std::atomic<bool> hardware_{false};

    std::mutex mutex_;
    std::condition_variable cv_;
    struct Packet {
        std::vector<uint8_t> data;
        int64_t targetNs = 0;  // Anzeigezeitpunkt (0 = sofort)
    };
    std::deque<Packet> queue_;
    bool waitForKey_ = false;  // durch mutex_ geschuetzt

    AVCodecContext* codec_ = nullptr;
    AVBufferRef* hwDevice_ = nullptr;
    int hwErrors_ = 0;
    uint64_t frames_ = 0;
    uint64_t pushed_ = 0;  // durch mutex_ geschuetzt (nur Diagnose)
};

}  // namespace app
