#include "AudioPipeline.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include "Log.h"
#include "Sync.h"

namespace app {

namespace {
constexpr int kSampleRate = 44100;
constexpr int64_t kToleranceNs = 30'000'000;       // +-30 ms Abweichung vom Zielzeitpunkt werden toleriert
constexpr int64_t kFallbackCushionNs = 100'000'000; // ohne brauchbaren Zeitstempel: 100 ms Vorrat nach Leerlauf
constexpr UINT32 kMaxSilenceSamples = kSampleRate / 2;  // nie mehr als 500 ms Stille am Stueck einfuegen

struct PcmBuffer {
    std::vector<uint8_t> data;
};

UINT32 NsToSamples(int64_t ns) { return static_cast<UINT32>(ns * kSampleRate / 1'000'000'000LL); }

// ALAC "magic cookie": 44100 Hz / 16 Bit / 2 Kanaele, 352 Samples pro Frame
const uint8_t kAlacCookie[36] = {0x00, 0x00, 0x00, 0x24, 0x61, 0x6c, 0x61, 0x63, 0x00, 0x00, 0x00, 0x00,
                                 0x00, 0x00, 0x01, 0x60, 0x00, 0x10, 0x28, 0x0a, 0x0e, 0x02, 0x00, 0xff,
                                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xac, 0x44};
const uint8_t kAacLcConfig[2] = {0x12, 0x10};
const uint8_t kAacEldConfig[4] = {0xf8, 0xe8, 0x50, 0x00};
}  // namespace

void AudioPipeline::Callback::OnBufferEnd(void* context) { delete static_cast<PcmBuffer*>(context); }

AudioPipeline::AudioPipeline() = default;
AudioPipeline::~AudioPipeline() { Shutdown(); }

bool AudioPipeline::Init() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (engine_) return true;
    HRESULT hr = XAudio2Create(&engine_, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        Log("Audio: XAudio2Create fehlgeschlagen (0x%08X)", static_cast<unsigned>(hr));
        engine_ = nullptr;
        return false;
    }
    hr = engine_->CreateMasteringVoice(&master_);
    if (FAILED(hr)) {
        Log("Audio: CreateMasteringVoice fehlgeschlagen (0x%08X)", static_cast<unsigned>(hr));
        engine_->Release();
        engine_ = nullptr;
        return false;
    }
    Log("Audio: XAudio2 bereit");
    return true;
}

void AudioPipeline::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    DestroyVoice();
    CloseDecoder();
    if (master_) {
        master_->DestroyVoice();
        master_ = nullptr;
    }
    if (engine_) {
        engine_->Release();
        engine_ = nullptr;
    }
}

bool AudioPipeline::CreateVoice() {
    DestroyVoice();
    if (!engine_) return false;
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = kSampleRate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = kSampleRate * 4;
    HRESULT hr = engine_->CreateSourceVoice(&voice_, &fmt, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &callback_);
    if (FAILED(hr)) {
        Log("Audio: CreateSourceVoice fehlgeschlagen (0x%08X)", static_cast<unsigned>(hr));
        voice_ = nullptr;
        return false;
    }
    submittedSamples_ = 0;
    // Die Voice laeuft durchgehend; ohne Puffer spielt sie einfach Stille. Der Zeitpunkt jedes Puffers wird
    // ueber die Warteschlangenlaenge gesteuert (siehe Submit).
    voice_->Start(0);
    return true;
}

void AudioPipeline::DestroyVoice() {
    if (voice_) {
        voice_->Stop();
        voice_->DestroyVoice();  // loest OnBufferEnd fuer alle offenen Puffer aus
        voice_ = nullptr;
    }
    submittedSamples_ = 0;
}

void AudioPipeline::CloseDecoder() {
    if (codec_) avcodec_free_context(&codec_);
    if (swr_) swr_free(&swr_);
}

bool AudioPipeline::OpenDecoder(unsigned char ct) {
    CloseDecoder();
    AVCodecID id;
    const uint8_t* extra;
    int extraSize;
    switch (ct) {
        case 2:
            id = AV_CODEC_ID_ALAC;
            extra = kAlacCookie;
            extraSize = sizeof(kAlacCookie);
            break;
        case 4:
            id = AV_CODEC_ID_AAC;
            extra = kAacLcConfig;
            extraSize = sizeof(kAacLcConfig);
            break;
        case 8:
            id = AV_CODEC_ID_AAC;
            extra = kAacEldConfig;
            extraSize = sizeof(kAacEldConfig);
            break;
        default:
            Log("Audio: nicht unterstuetzter Audio-Typ ct=%d", ct);
            return false;
    }
    const AVCodec* codec = avcodec_find_decoder(id);
    if (!codec) {
        Log("Audio: Decoder fuer ct=%d fehlt in FFmpeg", ct);
        return false;
    }
    codec_ = avcodec_alloc_context3(codec);
    codec_->sample_rate = kSampleRate;
    av_channel_layout_default(&codec_->ch_layout, 2);
    codec_->extradata = static_cast<uint8_t*>(av_mallocz(extraSize + AV_INPUT_BUFFER_PADDING_SIZE));
    memcpy(codec_->extradata, extra, extraSize);
    codec_->extradata_size = extraSize;
    codec_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (avcodec_open2(codec_, codec, nullptr) < 0) {
        Log("Audio: avcodec_open2 fuer ct=%d fehlgeschlagen", ct);
        avcodec_free_context(&codec_);
        return false;
    }
    ct_ = ct;
    return true;
}

void AudioPipeline::StartStream(unsigned char ct) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!engine_) return;
    DestroyVoice();
    frames_ = 0;
    dropped_ = 0;
    lateDrops_ = 0;
    silenceInserts_ = 0;
    if (!OpenDecoder(ct)) return;
    if (!CreateVoice()) return;
    SetVolumeDb(volumeDb_);
    Log("Audio: Stream gestartet (ct=%d, Wiedergabeverzoegerung %lld ms)", ct,
        static_cast<long long>(kPlayoutDelayNs / 1'000'000));
}

void AudioPipeline::StopStream() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frames_ > 0) {
        Log("Audio: Stream beendet (%llu Frames, %d zu spaet verworfen, %d-mal Stille eingefuegt)",
            static_cast<unsigned long long>(frames_), lateDrops_, silenceInserts_);
    }
    DestroyVoice();
    CloseDecoder();
    frames_ = 0;
}

void AudioPipeline::Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Neue Voice statt FlushSourceBuffers: so stimmt die Zaehlung submitted/played wieder exakt.
    if (voice_) CreateVoice();
    if (codec_) avcodec_flush_buffers(codec_);
}

void AudioPipeline::SetVolumeDb(float db) {
    volumeDb_ = db;
    float gain;
    if (db <= -30.0f) {
        gain = 0.0f;  // -30 dB und "stumm" (-144) -> aus
    } else if (db >= 0.0f) {
        gain = 1.0f;
    } else {
        gain = std::pow(10.0f, db / 20.0f);
    }
    if (master_) master_->SetVolume(gain);
}

uint64_t AudioPipeline::QueuedSamples() {
    XAUDIO2_VOICE_STATE state{};
    voice_->GetState(&state, 0);
    return submittedSamples_ > state.SamplesPlayed ? submittedSamples_ - state.SamplesPlayed : 0;
}

void AudioPipeline::Push(const uint8_t* data, int size, uint64_t ptsNs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!codec_ || !voice_ || size <= 0) {
        if (dropped_++ < 5)
            Log("Audio: Paket verworfen (Decoder=%d Voice=%d Groesse=%d)", codec_ ? 1 : 0, voice_ ? 1 : 0, size);
        return;
    }

    // Wie UxPlay: Frames mit unplausiblem ersten Byte (Fuellpakete) ueberspringen.
    bool valid = true;
    switch (ct_) {
        case 8:
            valid = data[0] == 0x8c || data[0] == 0x8d || data[0] == 0x8e || data[0] == 0x80 || data[0] == 0x81 ||
                    data[0] == 0x82;
            break;
        case 2:
            valid = data[0] == 0x20;
            break;
        case 4:
            valid = data[0] == 0xff;
            break;
        default:
            break;
    }
    if (!valid) {
        if (dropped_++ < 10)
            Log("Audio: Paket mit unerwartetem ersten Byte 0x%02x verworfen (ct=%d, Groesse=%d)", data[0], ct_, size);
        return;
    }

    const int64_t target = PlayoutTargetNs(ptsNs);
    if (frames_ < 3) {
        Log("Audio: Zeitstempel %s, Transportlatenz %.1f ms", target ? "ok" : "unbrauchbar",
            ptsNs ? (NowNs() - static_cast<int64_t>(ptsNs)) / 1e6 : 0.0);
    }

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return;
    if (av_new_packet(pkt, size) < 0) {
        av_packet_free(&pkt);
        return;
    }
    memcpy(pkt->data, data, size);
    int ret = avcodec_send_packet(codec_, pkt);
    av_packet_free(&pkt);
    if (ret < 0) {
        if (dropped_++ < 10) Log("Audio: send_packet Fehler %d", ret);
        return;
    }
    AVFrame* frame = av_frame_alloc();
    int64_t frameTarget = target;
    while (avcodec_receive_frame(codec_, frame) >= 0) {
        Submit(frame, frameTarget);
        if (frameTarget) frameTarget += static_cast<int64_t>(frame->nb_samples) * 1'000'000'000LL / kSampleRate;
        av_frame_unref(frame);
    }
    av_frame_free(&frame);
}

bool AudioPipeline::SubmitPcm(const uint8_t* pcm, UINT32 samples) {
    auto* buf = new PcmBuffer();
    buf->data.assign(pcm, pcm + static_cast<size_t>(samples) * 4);
    XAUDIO2_BUFFER xb{};
    xb.AudioBytes = static_cast<UINT32>(buf->data.size());
    xb.pAudioData = buf->data.data();
    xb.pContext = buf;
    if (FAILED(voice_->SubmitSourceBuffer(&xb))) {
        delete buf;
        return false;
    }
    submittedSamples_ += samples;
    return true;
}

bool AudioPipeline::SubmitSilence(UINT32 samples) {
    std::vector<uint8_t> zeros(static_cast<size_t>(samples) * 4, 0);
    return SubmitPcm(zeros.data(), samples);
}

void AudioPipeline::Submit(AVFrame* frame, int64_t targetNs) {
    if (!swr_) {
        AVChannelLayout out;
        av_channel_layout_default(&out, 2);
        if (swr_alloc_set_opts2(&swr_, &out, AV_SAMPLE_FMT_S16, kSampleRate, &frame->ch_layout,
                                static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr) < 0 ||
            swr_init(swr_) < 0) {
            Log("Audio: swresample-Initialisierung fehlgeschlagen");
            swr_free(&swr_);
            return;
        }
    }

    std::vector<uint8_t> pcm(static_cast<size_t>(frame->nb_samples) * 4);
    uint8_t* outPtr = pcm.data();
    int n = swr_convert(swr_, &outPtr, frame->nb_samples, const_cast<const uint8_t**>(frame->extended_data),
                        frame->nb_samples);
    if (n <= 0) return;

    const uint64_t queued = QueuedSamples();
    const int64_t now = NowNs();
    // Zeitpunkt, zu dem dieser Puffer bei der aktuellen Warteschlange zu hoeren waere
    const int64_t startNs = now + static_cast<int64_t>(queued) * 1'000'000'000LL / kSampleRate;

    if (targetNs) {
        const int64_t err = startNs - targetNs;
        if (err > kToleranceNs) {
            // Zu spaet (Netzpause laenger als die Wiedergabeverzoegerung): verwerfen, damit der Ton synchron bleibt.
            if (lateDrops_++ < 10) Log("Audio: Puffer %.0f ms zu spaet, verworfen", err / 1e6);
            return;
        }
        if (err < -kToleranceNs) {
            // Zu frueh (Stream-Start oder nach einer Pause): mit Stille bis zum Zielzeitpunkt auffuellen.
            const UINT32 silence = std::min(NsToSamples(-err), kMaxSilenceSamples);
            if (silenceInserts_++ < 10) Log("Audio: %.0f ms Stille eingefuegt (Synchronisation)", silence * 1000.0 / kSampleRate);
            SubmitSilence(silence);
        }
    } else if (queued == 0) {
        // Kein brauchbarer Zeitstempel: nach Leerlauf wenigstens einen kleinen Vorrat anlegen.
        SubmitSilence(NsToSamples(kFallbackCushionNs));
    }

    if (SubmitPcm(pcm.data(), static_cast<UINT32>(n)) && ++frames_ == 1) {
        Log("Audio: erster Puffer uebergeben (%d Samples, Warteschlange %.0f ms)", n, queued * 1000.0 / kSampleRate);
    }
}

}  // namespace app
