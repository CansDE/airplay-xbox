#pragma once
#include <xaudio2.h>

#include <atomic>
#include <cstdint>
#include <mutex>

struct AVCodecContext;
struct AVFrame;
struct SwrContext;

namespace app {

// AirPlay-Audio (ALAC / AAC-LC / AAC-ELD) -> FFmpeg -> PCM -> XAudio2.
// Jeder Puffer wird zu "Sender-Zeitstempel + kPlayoutDelayNs" abgespielt: zu spaete Puffer werden verworfen,
// vor zu fruehen wird Stille eingefuegt. So ueberstehen kurze Netzpausen ohne Aussetzer und der Ton bleibt lippensynchron.
class AudioPipeline {
public:
    AudioPipeline();
    ~AudioPipeline();

    // Muss auf einem COM-initialisierten Thread (UI-Thread) aufgerufen werden.
    bool Init();
    void Shutdown();

    // Neuen Audiostream beginnen. ct: 2 = ALAC, 4 = AAC-LC, 8 = AAC-ELD (AirPlay-Codes).
    void StartStream(unsigned char ct);
    // Ende des Streams: Decoder und Source-Voice freigeben.
    void StopStream();
    // Gepufferte Daten verwerfen (Seek/Pause beim Sender).
    void Flush();
    // Ein (bereits entschluesseltes) Codec-Frame; ptsNs = Sender-Zeitstempel in lokaler Uhr (0 = unbekannt).
    void Push(const uint8_t* data, int size, uint64_t ptsNs);
    // AirPlay-Lautstaerke in dB (-30..0, -144 = stumm).
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
    uint64_t submittedSamples_ = 0;  // seit Erzeugung der Voice uebergebene Samples (inkl. Stille)

    AVCodecContext* codec_ = nullptr;
    SwrContext* swr_ = nullptr;
    unsigned char ct_ = 0;

    // Statistik / Diagnose (pro Stream)
    uint64_t frames_ = 0;
    int dropped_ = 0;
    int lateDrops_ = 0;
    int silenceInserts_ = 0;

    std::atomic<float> volumeDb_{0.0f};
};

}  // namespace app
