/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "audio_pipeline.h"
#include <board.h>
#include <audio/audio_codec.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <mooncake_log.h>
#include <cstring>
#include <algorithm>

// Opus downlink decoder (esp_audio_codec, same lib xiaozhi uses). C headers.
#include <esp_opus_dec.h>
#include <esp_audio_dec.h>
#include <esp_audio_types.h>

namespace custom_agent {

static constexpr const char* _tag = "agent-audio";

// Opus downlink frame length (ms); MUST match the backend encoder (src/opus.ts). 60ms
// at 16kHz = 960 samples/packet. Decode scratch is sized generously (120ms) so an
// unexpected larger packet can never overflow the output buffer.
static constexpr int kOpusFrameMs        = 60;
static constexpr size_t kOpusMaxFrameSamples = 16000 / 1000 * 120;  // 1920 samples

// One heap-allocated PCM buffer travelling through the playback queue.
struct PcmBuf {
    int16_t* data;
    size_t samples;
};

// One raw Opus WS frame travelling from the WS-rx task to the decode task.
struct OpusMsg {
    uint8_t* data;  // PSRAM copy of the WS binary frame (freed by the decode task)
    size_t len;
};

static constexpr size_t kOpusFrameSamples = 16000 / 1000 * kOpusFrameMs;  // 960 (audio-length accounting)
static constexpr int kOpusInQueueDepth    = 32;     // raw Opus frames buffered ahead of decode
// Decode-task stack. On Xtensa StackType_t is 1 byte, so xTaskCreateStatic's depth is in
// BYTES. 16KB: the 4KB tcp_receive task overflowed when decoding inline, so give plenty.
static constexpr int kOpusTaskStackBytes = 16384;

// Playback jitter buffer depth (in chunks). This is deliberately deep: the WS/TCP
// receive task that feeds enqueuePcm() runs at a very low priority and shares the
// cores with the WireGuard manager (prio 7, core 1) and LVGL (prio 3, core 1), so
// audio bytes arrive in bursts with multi-100ms starvation gaps. A deep queue lets
// a burst (often a whole segment) buffer ahead and drain smoothly, decoupling
// playback from receive jitter. Each item is one ~4KB server chunk, so 96 holds
// ~12s of 16kHz mono audio (<=384KB PSRAM peak). Playback still starts on the first
// chunk, so this adds buffering slack, not latency.
static constexpr int kPlayQueueDepth = 96;

bool AudioPipeline::init()
{
    codec_ = Board::GetInstance().GetAudioCodec();
    if (codec_ == nullptr) {
        mclog::tagError(_tag, "no audio codec");
        return false;
    }

    inRate_     = codec_->input_sample_rate();
    inChannels_ = codec_->input_channels();
    outRate_    = codec_->output_sample_rate();  // diagnostics: playback rate for duration math
    mclog::tagInfo(_tag, "codec in: {}Hz x{}ch, out: {}Hz x{}ch", inRate_, inChannels_, codec_->output_sample_rate(),
                   codec_->output_channels());

    codec_->EnableInput(true);
    codec_->EnableOutput(true);
    // Make sure we are audible even if no volume was provisioned yet.
    if (codec_->output_volume() <= 0) {
        codec_->SetOutputVolume(80);
    }

    // input frames per ~20ms at the codec rate, all channels interleaved
    const int inFrames = inRate_ * kFrameMs / 1000;
    readBuf_.assign(static_cast<size_t>(inFrames) * inChannels_, 0);
    prevSamples_.assign(static_cast<size_t>(inChannels_), 0);

    playQueue_ = xQueueCreate(kPlayQueueDepth, sizeof(PcmBuf));
    if (playQueue_ == nullptr) {
        mclog::tagError(_tag, "failed to create play queue");
        return false;
    }

    running_ = true;
    // Priority 8: above the microlink WireGuard/net tasks (prio 7) and LVGL (prio 3)
    // so refilling the small (~90ms) I2S DMA buffer is never preempted by WG crypto
    // or a face/text render -- an under-prioritized play task was a source of dropouts.
    // Safe to sit this high because the task spends almost all its time blocked in
    // i2s_channel_write (DMA back-pressure), yielding the core; it only needs the CPU
    // for the brief per-chunk memcpy, so it cannot starve the network tasks.
    xTaskCreatePinnedToCore(playTaskTrampoline, "agent_play", 4096, this, 8, &playTaskHdl_, 1);

    return true;
}

void AudioPipeline::deinit()
{
    running_ = false;
    flushPlayback();
    if (playTaskHdl_) {
        // playTask exits on running_==false; give it a moment then drop the handle.
        vTaskDelay(pdMS_TO_TICKS(50));
        playTaskHdl_ = nullptr;
    }
    // Stop the Opus decode task (running_==false already signals it; a nullptr sentinel
    // wakes it if it's blocked on the queue) and reclaim its PSRAM stack + internal TCB.
    if (opusTaskHdl_) {
        if (opusInQueue_) {
            OpusMsg stop{nullptr, 0};
            xQueueSend(opusInQueue_, &stop, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        opusTaskHdl_ = nullptr;
    }
    if (opusInQueue_) {
        OpusMsg m;
        while (xQueueReceive(opusInQueue_, &m, 0) == pdTRUE) {
            if (m.data) {
                heap_caps_free(m.data);
            }
        }
        vQueueDelete(opusInQueue_);
        opusInQueue_ = nullptr;
    }
    if (opusStack_) {
        heap_caps_free(opusStack_);
        opusStack_ = nullptr;
    }
    if (opusTcb_) {
        heap_caps_free(opusTcb_);
        opusTcb_ = nullptr;
    }
    if (playQueue_) {
        vQueueDelete(playQueue_);
        playQueue_ = nullptr;
    }
    if (opusDec_) {
        esp_opus_dec_close(opusDec_);
        opusDec_ = nullptr;
    }
    if (opusFrameBuf_) {
        heap_caps_free(opusFrameBuf_);
        opusFrameBuf_ = nullptr;
    }
}

bool AudioPipeline::captureFrame2ch16k(std::vector<int16_t>& out)
{
    if (codec_ == nullptr) {
        return false;
    }

    // Read interleaved frames. InputData() reads readBuf_.size() int16 samples.
    // Capture (this task) and playback (playTask) hit the codec concurrently, but
    // CoreS3 is full-duplex (separate I2S in/out devices) so read and write don't
    // contend. NOTE: AudioCodec itself does not lock — a future half-duplex board
    // would need InputData/OutputData guarded by a mutex.
    if (!codec_->InputData(readBuf_)) {
        return false;
    }
    const int total    = static_cast<int>(readBuf_.size());
    const int ch       = inChannels_;
    const int inFrames = total / ch;

    // No resampling needed if the codec already runs at the target rate: hand back
    // the interleaved frame as-is (all channels preserved).
    if (inRate_ == kOutRate) {
        out.assign(readBuf_.begin(), readBuf_.begin() + static_cast<size_t>(inFrames) * ch);
        return true;
    }

    // Linear resample inRate_ -> kOutRate, continuous across calls. Every channel is
    // interpolated at the same fractional positions, so the channels stay aligned and
    // the AEC reference keeps its phase relationship to the mic.
    const float step = static_cast<float>(inRate_) / static_cast<float>(kOutRate);
    out.clear();
    out.reserve((static_cast<size_t>(inFrames) * kOutRate / inRate_ + 2) * ch);

    float t = resamplePos_;
    while (t < inFrames - 1) {
        const int i   = static_cast<int>(t);
        const float f = t - i;
        for (int c = 0; c < ch; ++c) {
            const int16_t a = (i < 0) ? prevSamples_[c] : readBuf_[static_cast<size_t>(i) * ch + c];
            const int16_t b = readBuf_[static_cast<size_t>(i + 1) * ch + c];
            out.push_back(static_cast<int16_t>(a + (b - a) * f));
        }
        t += step;
    }
    // Carry the fractional remainder and each channel's last sample into the next block.
    resamplePos_ = t - inFrames;
    for (int c = 0; c < ch; ++c) {
        prevSamples_[c] = readBuf_[static_cast<size_t>(inFrames - 1) * ch + c];
    }

    return true;
}

// Per-segment receive accounting for one arriving WS audio frame (WS-rx task only --
// no locking). `bytes` is what crossed the wire: raw PCM, or compressed Opus.
void AudioPipeline::noteRxBytes(size_t bytes)
{
    const int64_t now = esp_timer_get_time();
    if (segFirstRxUs_ == 0) {
        segFirstRxUs_ = now;
    }
    segLastRxUs_ = now;
    segRxBytes_ += bytes;
    ++segRxChunks_;
}

// Copy `count` 16k mono samples into a PSRAM buffer and queue them for playback.
// Called from the WS-rx task (raw path) and the Opus decode task. Blocks up to 1s on a
// full queue (backpressure paces the producer to playback); only drops on hard failure.
void AudioPipeline::pushPcmSamples(const int16_t* samples, size_t count)
{
    if (playQueue_ == nullptr || samples == nullptr || count == 0) {
        return;
    }
    const size_t bytes = count * sizeof(int16_t);
    auto* buf = static_cast<int16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        buf = static_cast<int16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
    }
    if (buf == nullptr) {
        mclog::tagError(_tag, "playback alloc failed ({} bytes)", bytes);
        return;
    }
    std::memcpy(buf, samples, bytes);

    PcmBuf item{buf, count};
    if (xQueueSend(playQueue_, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
        segDrops_.fetch_add(1);
        mclog::tagWarn(_tag, "play queue full, dropping chunk");
        heap_caps_free(buf);
    }
}

void AudioPipeline::enqueuePcm(const uint8_t* bytes, size_t len)
{
    if (playQueue_ == nullptr || bytes == nullptr || len < sizeof(int16_t)) {
        return;
    }
    noteRxBytes(len);                                    // raw path: wire bytes == PCM bytes
    segPlaySamples_ += static_cast<uint32_t>(len / sizeof(int16_t));
    pushPcmSamples(reinterpret_cast<const int16_t*>(bytes), len / sizeof(int16_t));
}

// Lazily create the Opus decode task + input queue (WS-rx task). The decode task uses a
// PSRAM stack so it doesn't draw down the tight internal-RAM budget (the decoder's working
// buffers are PSRAM too). TCB stays internal (the scheduler/tick ISR touch it).
bool AudioPipeline::ensureOpusPipeline()
{
    if (opusTaskHdl_ != nullptr) {
        return true;
    }
    if (opusPipelineTried_) {
        return false;
    }
    opusPipelineTried_ = true;

    opusInQueue_ = xQueueCreate(kOpusInQueueDepth, sizeof(OpusMsg));
    if (opusInQueue_ == nullptr) {
        mclog::tagError(_tag, "opus in-queue create failed");
        return false;
    }
    opusStack_ = static_cast<StackType_t*>(
        heap_caps_malloc(kOpusTaskStackBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    opusTcb_ = static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    if (opusStack_ == nullptr || opusTcb_ == nullptr) {
        mclog::tagError(_tag, "opus task alloc failed (stack/tcb)");
        if (opusStack_) {
            heap_caps_free(opusStack_);
            opusStack_ = nullptr;
        }
        if (opusTcb_) {
            heap_caps_free(opusTcb_);
            opusTcb_ = nullptr;
        }
        vQueueDelete(opusInQueue_);
        opusInQueue_ = nullptr;
        return false;
    }
    // Priority 6: above LVGL, at/below the WG/net tasks (7) -- it just converts Opus to
    // PCM into the play queue and otherwise blocks. Core 1 with the play task.
    opusTaskHdl_ = xTaskCreateStaticPinnedToCore(opusDecodeTaskTrampoline, "agent_opus", kOpusTaskStackBytes, this, 6,
                                                 opusStack_, opusTcb_, 1);
    if (opusTaskHdl_ == nullptr) {
        mclog::tagError(_tag, "opus task create failed");
        return false;
    }
    mclog::tagInfo(_tag, "opus decode task started ({}KB PSRAM stack)", kOpusTaskStackBytes / 1024);
    return true;
}

bool AudioPipeline::ensureOpusDecoder()
{
    if (opusDec_ != nullptr) {
        return true;
    }
    if (opusOpenTried_) {
        return false;  // hard-failed earlier; don't spam open attempts
    }
    opusOpenTried_ = true;

    esp_opus_dec_cfg_t cfg = {};
    cfg.sample_rate    = static_cast<uint32_t>(outRate_ > 0 ? outRate_ : kOutRate);
    cfg.channel        = ESP_AUDIO_MONO;
    cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;  // must match backend (kOpusFrameMs)
    cfg.self_delimited = false;

    const size_t beforeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const esp_audio_err_t ret = esp_opus_dec_open(&cfg, sizeof(cfg), &opusDec_);
    const size_t afterInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (ret != ESP_AUDIO_ERR_OK || opusDec_ == nullptr) {
        mclog::tagError(_tag, "opus decoder open failed (ret={}); dropping opus audio", (int)ret);
        opusDec_ = nullptr;
        return false;
    }
    opusFrameBuf_ = static_cast<int16_t*>(
        heap_caps_malloc(kOpusMaxFrameSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (opusFrameBuf_ == nullptr) {
        opusFrameBuf_ = static_cast<int16_t*>(heap_caps_malloc(kOpusMaxFrameSamples * sizeof(int16_t), MALLOC_CAP_8BIT));
    }
    if (opusFrameBuf_ == nullptr) {
        mclog::tagError(_tag, "opus frame buffer alloc failed");
        esp_opus_dec_close(opusDec_);
        opusDec_ = nullptr;
        return false;
    }
    mclog::tagInfo(_tag, "opus decoder open: {}Hz {}ms, int_free {}->{} ({} bytes)", (unsigned)cfg.sample_rate,
                   kOpusFrameMs, (unsigned long)beforeInt, (unsigned long)afterInt,
                   (long)((long)beforeInt - (long)afterInt));
    return true;
}

// Runs on the WS receive task (4KB stack). MUST stay light: no Opus decode here (it
// overflowed this task's stack). We only account, copy the frame to PSRAM, and hand it to
// the decode task. Audio length is counted from the packet count (each packet = one
// kOpusFrameMs frame) so endSegment() has it without waiting for the async decode.
void AudioPipeline::enqueueOpusMessage(const uint8_t* data, size_t len)
{
    if (playQueue_ == nullptr || data == nullptr || len == 0) {
        return;
    }
    noteRxBytes(len);  // opus path: wire bytes == compressed bytes (SEG ratio reflects the win)
    segOpus_ = true;

    // Count whole packets (walk the length prefixes only -- no decode) for the audio-length
    // accounting. Each Opus packet decodes to one kOpusFrameMs frame (kOpusFrameSamples).
    size_t off      = 0;
    unsigned frames = 0;
    while (off + 2 <= len) {
        const size_t pktLen = static_cast<size_t>(data[off]) | (static_cast<size_t>(data[off + 1]) << 8);
        off += 2;
        if (pktLen == 0 || off + pktLen > len) {
            if ((opusErrCount_++ % 50) == 0) {
                mclog::tagWarn(_tag, "opus frame parse error (pktLen={} off={} len={})", pktLen, off, len);
            }
            break;
        }
        ++frames;
        off += pktLen;
    }
    segPlaySamples_ += frames * static_cast<uint32_t>(kOpusFrameSamples);

    if (!ensureOpusPipeline()) {
        return;  // decode task unavailable -> drop (logged once in ensureOpusPipeline)
    }

    // Copy the frame to PSRAM and hand it to the decode task. Drop (don't block the socket)
    // if the decode task has fallen far behind -- it won't, since decode is far faster than
    // the network, but a backlog would otherwise stall the WS receive task.
    auto* copy = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy == nullptr) {
        copy = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_8BIT));
    }
    if (copy == nullptr) {
        return;
    }
    std::memcpy(copy, data, len);
    OpusMsg msg{copy, len};
    if (xQueueSend(opusInQueue_, &msg, 0) != pdTRUE) {
        segDrops_.fetch_add(1);
        heap_caps_free(copy);
    }
}

void AudioPipeline::opusDecodeTaskTrampoline(void* arg)
{
    static_cast<AudioPipeline*>(arg)->opusDecodeTask();
    vTaskDelete(nullptr);
}

// Dedicated decode task (PSRAM stack). Drains opusInQueue_, decodes each packet, and feeds
// the PCM play queue. Exits when running_ goes false (deinit pushes a nullptr sentinel).
void AudioPipeline::opusDecodeTask()
{
    while (running_.load()) {
        OpusMsg msg;
        if (xQueueReceive(opusInQueue_, &msg, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        if (msg.data == nullptr) {
            break;  // stop sentinel
        }
        if (ensureOpusDecoder()) {
            const uint8_t* d = msg.data;
            const size_t len = msg.len;
            size_t off       = 0;
            while (off + 2 <= len) {
                const size_t pktLen = static_cast<size_t>(d[off]) | (static_cast<size_t>(d[off + 1]) << 8);
                off += 2;
                if (pktLen == 0 || off + pktLen > len) {
                    break;  // framing desync (already logged on the rx side)
                }
                esp_audio_dec_in_raw_t raw = {};
                raw.buffer        = const_cast<uint8_t*>(d + off);
                raw.len           = static_cast<uint32_t>(pktLen);
                raw.consumed      = 0;
                raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;
                esp_audio_dec_out_frame_t out = {};
                out.buffer       = reinterpret_cast<uint8_t*>(opusFrameBuf_);
                out.len          = static_cast<uint32_t>(kOpusMaxFrameSamples * sizeof(int16_t));
                out.decoded_size = 0;
                esp_audio_dec_info_t info = {};

                const esp_audio_err_t ret = esp_opus_dec_decode(opusDec_, &raw, &out, &info);
                if (ret == ESP_AUDIO_ERR_OK && out.decoded_size > 0) {
                    pushPcmSamples(opusFrameBuf_, out.decoded_size / sizeof(int16_t));
                } else if ((opusErrCount_++ % 50) == 0) {
                    mclog::tagWarn(_tag, "opus decode failed (ret={} pktLen={})", (int)ret, pktLen);
                }
                off += pktLen;
            }
        }
        heap_caps_free(msg.data);
    }
}

bool AudioPipeline::isPlaying() const
{
    if (playQueue_ == nullptr) {
        return false;
    }
    // Also count Opus frames still queued for decode -- they are audio in flight that
    // hasn't reached the play queue yet (else a turn could look "done" mid-decode).
    if (opusInQueue_ != nullptr && uxQueueMessagesWaiting(opusInQueue_) > 0) {
        return true;
    }
    return writing_.load() || uxQueueMessagesWaiting(playQueue_) > 0;
}

void AudioPipeline::flushPlayback()
{
    // Drop undecoded Opus frames first (the decode task may push one more in flight --
    // acceptable: <=60ms of stale audio on disconnect).
    if (opusInQueue_ != nullptr) {
        OpusMsg m;
        while (xQueueReceive(opusInQueue_, &m, 0) == pdTRUE) {
            if (m.data) {
                heap_caps_free(m.data);
            }
        }
    }
    if (playQueue_ == nullptr) {
        return;
    }
    PcmBuf item;
    while (xQueueReceive(playQueue_, &item, 0) == pdTRUE) {
        heap_caps_free(item.data);
    }
}

void AudioPipeline::playTaskTrampoline(void* arg)
{
    static_cast<AudioPipeline*>(arg)->playTask();
    vTaskDelete(nullptr);
}

void AudioPipeline::playTask()
{
    constexpr size_t kWriteChunk = 512;  // samples per codec write
    // diagnostics: a mid-stream wait longer than this means the queue ran dry while a
    // turn was still in progress -> an audible gap (the I2S DMA buffer is only ~90ms
    // deep). In normal operation the next chunk is already queued, so the wait is ~0.
    // The play task is the highest-priority audio task, so a wait can only mean the
    // producer (network/RX) fell behind -- never that this task was descheduled.
    constexpr int64_t kStallUs = 30'000;
    std::vector<int16_t> scratch;
    scratch.reserve(kWriteChunk);
    bool active = false;  // were we mid-playback? (separates end-of-turn idle from a real stall)
    while (running_.load()) {
        const int64_t waitStart = esp_timer_get_time();
        PcmBuf item;
        if (xQueueReceive(playQueue_, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
            active = false;  // queue idle: end of turn, not an underrun
            continue;
        }
        const int64_t waited = esp_timer_get_time() - waitStart;
        if (active && waited >= kStallUs) {
            underruns_.fetch_add(1);
            starvedMs_.fetch_add(static_cast<uint32_t>(waited / 1000));
        }
        // Track the play-queue low-watermark (slack left right after a dequeue): if it
        // reaches 0 the producer is barely keeping up; combined with underruns above it
        // shows whether the jitter buffer is actually absorbing the receive bursts.
        const int depth = static_cast<int>(uxQueueMessagesWaiting(playQueue_));
        int prevMin     = playQueueMin_.load();
        while (depth < prevMin && !playQueueMin_.compare_exchange_weak(prevMin, depth)) {
        }
        // First audible sample of the turn -> report the end-to-end latency once.
        if (!firstPlayLogged_.exchange(true)) {
            const int64_t t0 = turnStartUs_.load();
            if (t0 > 0) {
                mclog::tagInfo(_tag, "RESP firstPlay +{}ms", (long)((esp_timer_get_time() - t0) / 1000));
            }
        }
        writing_ = true;
        size_t off = 0;
        while (off < item.samples && running_.load()) {
            const size_t n = std::min<size_t>(kWriteChunk, item.samples - off);
            scratch.assign(item.data + off, item.data + off + n);  // OutputData writes scratch.size() samples
            codec_->OutputData(scratch);
            off += n;
        }
        heap_caps_free(item.data);
        active = true;
        // Only clear writing_ once the queue is empty, so isPlaying() stays true
        // across back-to-back chunks within a turn.
        if (uxQueueMessagesWaiting(playQueue_) == 0) {
            writing_ = false;
        }
    }
    writing_ = false;
}

/* ----------------------------- diagnostics ----------------------------- */

void AudioPipeline::noteTurnStart()
{
    turnStartUs_.store(esp_timer_get_time());
    firstPlayLogged_.store(false);
}

void AudioPipeline::beginSegment()
{
    segStartUs_     = esp_timer_get_time();
    segFirstRxUs_   = 0;
    segLastRxUs_    = 0;
    segRxBytes_     = 0;
    segRxChunks_    = 0;
    segDrops_.store(0);
    segPlaySamples_ = 0;
    segOpus_        = false;
    playQueueMin_.store(kPlayQueueDepth + 1);  // sentinel: lowered as the play task dequeues
    underruns_.store(0);
    starvedMs_.store(0);
}

void AudioPipeline::endSegment()
{
    if (segRxBytes_ == 0) {
        return;  // empty / aborted segment -- nothing to report
    }
    const int64_t rxSpanMs = (segLastRxUs_ > segFirstRxUs_) ? (segLastRxUs_ - segFirstRxUs_) / 1000 : 0;
    const int64_t ttfbMs   = (segStartUs_ > 0 && segFirstRxUs_ > 0) ? (segFirstRxUs_ - segStartUs_) / 1000 : 0;
    const int rate         = outRate_ > 0 ? outRate_ : kOutRate;
    // Audio length from the DECODED sample count (Opus: compressed bytes != samples).
    const int64_t audioMs = static_cast<int64_t>(segPlaySamples_) * 1000 / rate;
    // KB/s is the on-the-wire receive throughput (compressed for Opus).
    const int kBps = rxSpanMs > 0 ? static_cast<int>((int64_t)segRxBytes_ * 1000 / 1024 / rxSpanMs) : 0;
    // Receive wall-clock as a percentage of the audio's intrinsic length. >100% means the
    // network delivered this segment slower than it plays -> a guaranteed underrun. With
    // Opus the wire bytes drop ~8x, so ratio should fall well under 100% on weak WiFi.
    const int ratioPct  = audioMs > 0 ? static_cast<int>(rxSpanMs * 100 / audioMs) : 0;
    const int qmin      = playQueueMin_.load();
    const char* codec   = segOpus_ ? "opus" : "pcm";
    mclog::tagInfo(_tag,
                   "SEG {} audio={}ms rx={}ms ratio={}% {}KB/s ttfb={}ms bytes={} chunks={} qmin={} drops={} "
                   "underrun={}({}ms)",
                   codec, (long)audioMs, (long)rxSpanMs, ratioPct, kBps, (long)ttfbMs, (unsigned long)segRxBytes_,
                   (unsigned long)segRxChunks_, qmin > kPlayQueueDepth ? -1 : qmin,
                   (unsigned long)segDrops_.load(), (unsigned long)underruns_.load(), (unsigned long)starvedMs_.load());
}

}  // namespace custom_agent
