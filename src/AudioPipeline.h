#pragma once
#include <xaudio2.h>

#include <atomic>
#include <cstdint>
#include <mutex>

struct AVCodecContext;
struct AVFrame;
struct SwrContext;

namespace app {

// AirPlay audio (ALAC / AAC-LC / AAC-ELD) -> FFmpeg -> PCM -> XAudio2.
// Each buffer is played at "sender timestamp + kPlayoutDelayNs": late buffers are dropped, silence is inserted
// before early ones. This rides out short network gaps without dropouts and keeps the audio lip-synced.
class AudioPipeline {
public:
    AudioPipeline();
    ~AudioPipeline();

    // Must be called on a COM-initialized thread (UI thread).
    bool Init();
    void Shutdown();

    // Start a new audio stream. ct: 2 = ALAC, 4 = AAC-LC, 8 = AAC-ELD (AirPlay codes).
    void StartStream(unsigned char ct);
    // End of stream: release decoder and source voice.
    void StopStream();
    // Discard buffered data (seek/pause on the sender).
    void Flush();
    // One (already decrypted) codec frame; ptsNs = sender timestamp in local clock (0 = unknown).
    void Push(const uint8_t* data, int size, uint64_t ptsNs);
    // AirPlay volume in dB (-30..0, -144 = mute).
    void SetVolumeDb(float db);

private:
    struct Callback : IXAudio2VoiceCallback {
        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
        void STDMETHODCALLTYPE OnStreamEnd() override {}
        void STDMETHODCALLTYPE OnBufferStart(void*) override {}
        void STDMETHODCALLTYPE OnBufferEnd(void* context) override;
        void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
        void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
    };

    bool OpenDecoder(unsigned char ct);
    void CloseDecoder();
    bool CreateVoice();
    void DestroyVoice();
    void Submit(AVFrame* frame, int64_t targetNs);
    bool SubmitPcm(const uint8_t* pcm, UINT32 samples);
    bool SubmitSilence(UINT32 samples);
    uint64_t QueuedSamples();

    std::mutex mutex_;
    Callback callback_;
    IXAudio2* engine_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    IXAudio2SourceVoice* voice_ = nullptr;
    uint64_t submittedSamples_ = 0;  // samples submitted since the voice was created (incl. silence)

    AVCodecContext* codec_ = nullptr;
    SwrContext* swr_ = nullptr;
    unsigned char ct_ = 0;

    // Statistics / diagnostics (per stream)
    uint64_t frames_ = 0;
    int dropped_ = 0;
    int lateDrops_ = 0;
    int silenceInserts_ = 0;

    std::atomic<float> volumeDb_{0.0f};
};

}  // namespace app
