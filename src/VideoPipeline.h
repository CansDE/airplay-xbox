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

// H.264 (Annex B) -> decoder thread -> renderer. Prefers D3D11VA, otherwise the software decoder.
class VideoPipeline {
public:
    explicit VideoPipeline(Renderer& renderer);
    ~VideoPipeline();

    void Start();
    void Stop();

    // Called from the AirPlay network thread; copies the data. ptsNs = sender timestamp in local clock
    // (0 = unknown); the frame is shown at ptsNs + kPlayoutDelayNs, matching the audio.
    void Push(const uint8_t* annexB, int size, uint64_t ptsNs);
    // Discards pending packets and flushes the decoder (e.g. when the connection changes).
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
        int64_t targetNs = 0;  // display time (0 = immediately)
    };
    std::deque<Packet> queue_;
    bool waitForKey_ = false;  // protected by mutex_

    AVCodecContext* codec_ = nullptr;
    AVBufferRef* hwDevice_ = nullptr;
    int hwErrors_ = 0;
    uint64_t frames_ = 0;
    uint64_t pushed_ = 0;  // protected by mutex_ (diagnostics only)
};

}  // namespace app
