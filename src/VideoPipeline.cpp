#include "VideoPipeline.h"

#include <chrono>

#include <d3d11.h>  // before the extern "C" block, otherwise d3d11.h (C++ operators) ends up inside it

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
}

#include "Log.h"
#include "Renderer.h"
#include "Sync.h"

namespace app {

namespace {

constexpr size_t kMaxQueuedPackets = 90;

// Does the Annex B packet contain an IDR frame (NAL 5) or SPS (NAL 7)?
bool ContainsKeyFrame(const uint8_t* d, int n) {
    for (int i = 0; i + 4 < n; ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            int type = d[i + 3] & 0x1f;
            if (type == 5 || type == 7) return true;
            i += 2;
        }
    }
    return false;
}

void LockCb(void* ctx) { static_cast<std::recursive_mutex*>(ctx)->lock(); }
void UnlockCb(void* ctx) { static_cast<std::recursive_mutex*>(ctx)->unlock(); }

AVPixelFormat GetFormatHw(AVCodecContext* ctx, const AVPixelFormat* fmts) {
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p != AV_PIX_FMT_D3D11) continue;
        AVBufferRef* frames = nullptr;
        if (avcodec_get_hw_frames_parameters(ctx, ctx->hw_device_ctx, AV_PIX_FMT_D3D11, &frames) < 0) break;
        auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
        auto* d3d = static_cast<AVD3D11VAFramesContext*>(fc->hwctx);
        d3d->BindFlags |= D3D11_BIND_SHADER_RESOURCE;
        fc->initial_pool_size += 6;  // headroom, because the renderer holds frames briefly
        if (av_hwframe_ctx_init(frames) < 0) {
            av_buffer_unref(&frames);
            break;
        }
        av_buffer_unref(&ctx->hw_frames_ctx);
        ctx->hw_frames_ctx = frames;
        return AV_PIX_FMT_D3D11;
    }
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_YUV420P) return *p;
    }
    return fmts[0];
}

}  // namespace

VideoPipeline::VideoPipeline(Renderer& renderer) : renderer_(renderer) {}

VideoPipeline::~VideoPipeline() { Stop(); }

void VideoPipeline::Start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { Run(); });
}

void VideoPipeline::Stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    CloseDecoder();
    if (hwDevice_) av_buffer_unref(&hwDevice_);
}

void VideoPipeline::Push(const uint8_t* annexB, int size, uint64_t ptsNs) {
    if (!running_ || size <= 0) return;
    const int64_t target = PlayoutTargetNs(ptsNs);
    std::lock_guard<std::mutex> lock(mutex_);
    if (pushed_++ < 3) {
        Log("Video: timestamp %s, transport latency %.1f ms", target ? "ok" : "unusable",
            ptsNs ? (NowNs() - static_cast<int64_t>(ptsNs)) / 1e6 : 0.0);
    }
    if (queue_.size() >= kMaxQueuedPackets) {
        // The decoder cannot keep up: discard everything and wait for the next keyframe.
        queue_.clear();
        waitForKey_ = true;
        Log("Video: queue full, dropping packets until the next keyframe");
    }
    if (waitForKey_) {
        if (!ContainsKeyFrame(annexB, size)) return;
        waitForKey_ = false;
    }
    queue_.push_back(Packet{std::vector<uint8_t>(annexB, annexB + size), target});
    cv_.notify_one();
}

void VideoPipeline::Flush() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        flushRequested_ = true;
    }
    cv_.notify_all();
}

bool VideoPipeline::OpenDecoder(bool hardware) {
    CloseDecoder();
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        Log("Video: FFmpeg has no H.264 decoder");
        return false;
    }
    codec_ = avcodec_alloc_context3(codec);
    codec_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_->flags2 |= AV_CODEC_FLAG2_FAST;

    if (hardware) {
        if (!hwDevice_) {
            AVBufferRef* dev = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
            if (!dev) return false;
            auto* dctx = reinterpret_cast<AVHWDeviceContext*>(dev->data);
            auto* d3d = static_cast<AVD3D11VADeviceContext*>(dctx->hwctx);
            d3d->device = renderer_.Device();
            d3d->device->AddRef();
            d3d->lock = LockCb;
            d3d->unlock = UnlockCb;
            d3d->lock_ctx = &renderer_.Lock();
            if (av_hwdevice_ctx_init(dev) < 0) {
                Log("Video: av_hwdevice_ctx_init (D3D11VA) failed");
                av_buffer_unref(&dev);
                avcodec_free_context(&codec_);
                return false;
            }
            hwDevice_ = dev;
        }
        codec_->hw_device_ctx = av_buffer_ref(hwDevice_);
        codec_->get_format = GetFormatHw;
        codec_->thread_count = 1;
    } else {
        codec_->thread_count = 4;
        codec_->thread_type = FF_THREAD_SLICE;
    }

    int ret = avcodec_open2(codec_, codec, nullptr);
    if (ret < 0) {
        char err[128];
        av_strerror(ret, err, sizeof(err));
        Log("Video: avcodec_open2 (%s) failed: %s", hardware ? "D3D11VA" : "Software", err);
        avcodec_free_context(&codec_);
        return false;
    }
    hardware_ = hardware;
    hwErrors_ = 0;
    Log("Video: H.264 decoder opened (%s)", hardware ? "D3D11VA Hardware" : "Software");
    return true;
}

void VideoPipeline::CloseDecoder() {
    if (codec_) avcodec_free_context(&codec_);
}

void VideoPipeline::Decode(const std::vector<uint8_t>& packet) {
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return;
    if (av_new_packet(pkt, static_cast<int>(packet.size())) < 0) {
        av_packet_free(&pkt);
        return;
    }
    memcpy(pkt->data, packet.data(), packet.size());

    int ret = avcodec_send_packet(codec_, pkt);
    av_packet_free(&pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        char err[128];
        av_strerror(ret, err, sizeof(err));
        Log("Video: send_packet error: %s", err);
        if (hardware_ && frames_ == 0 && ++hwErrors_ >= 5) {
            Log("Video: hardware decoder delivers no frames, switching to software");
            if (!OpenDecoder(false)) CloseDecoder();
        }
        return;
    }

    AVFrame* frame = av_frame_alloc();
    while (codec_ && avcodec_receive_frame(codec_, frame) >= 0) {
        bool shown = false;
        if (frame->format == AV_PIX_FMT_D3D11) {
            shown = renderer_.PresentDecoded(reinterpret_cast<ID3D11Texture2D*>(frame->data[0]),
                                             static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1])),
                                             frame->width, frame->height);
        } else if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P) {
            shown = renderer_.PresentI420(frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1],
                                          frame->data[2], frame->linesize[2], frame->width, frame->height);
        } else if (frames_ == 0) {
            Log("Video: unsupported pixel format %d", frame->format);
        }
        if (shown && frames_++ == 0) Log("Video: first frame shown (%dx%d)", frame->width, frame->height);
        av_frame_unref(frame);
    }
    av_frame_free(&frame);
}

void VideoPipeline::Run() {
    // Decoding takes a few milliseconds; work on the packet starts this much earlier.
    constexpr int64_t kDecodeLeadNs = 8'000'000;

    while (running_) {
        Packet packet;
        bool flush = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(100), [this] { return !queue_.empty() || !running_; });
            if (!running_) break;
            if (flushRequested_.exchange(false)) flush = true;
            if (!queue_.empty()) {
                // Wait until the display time of the next packet (Flush/Stop interrupt the wait).
                const int64_t wait = queue_.front().targetNs ? queue_.front().targetNs - kDecodeLeadNs - NowNs() : 0;
                if (!flush && wait > 1'000'000) {
                    cv_.wait_for(lock, std::chrono::nanoseconds(wait),
                                 [this] { return !running_ || flushRequested_.load(); });
                    continue;
                }
                packet = std::move(queue_.front());
                queue_.pop_front();
            }
        }
        if (flush && codec_) {
            avcodec_flush_buffers(codec_);
            frames_ = 0;
        }
        if (packet.data.empty()) continue;

        if (!codec_ && !OpenDecoder(true) && !OpenDecoder(false)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        Decode(packet.data);
    }
}

}  // namespace app
