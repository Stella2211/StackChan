/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "audio_pipeline.h"
#include <board.h>
#include <audio/audio_codec.h>
#include <esp_heap_caps.h>
#include <mooncake_log.h>
#include <cstring>
#include <algorithm>

namespace custom_agent {

static constexpr const char* _tag = "agent-audio";

// One heap-allocated PCM buffer travelling through the playback queue.
struct PcmBuf {
    int16_t* data;
    size_t samples;
};

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
    if (playQueue_) {
        vQueueDelete(playQueue_);
        playQueue_ = nullptr;
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

void AudioPipeline::enqueuePcm(const uint8_t* bytes, size_t len)
{
    if (playQueue_ == nullptr || bytes == nullptr || len < sizeof(int16_t)) {
        return;
    }
    const size_t samples = len / sizeof(int16_t);
    auto* buf = static_cast<int16_t*>(heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        buf = static_cast<int16_t*>(heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_8BIT));
    }
    if (buf == nullptr) {
        mclog::tagError(_tag, "playback alloc failed ({} bytes)", samples * sizeof(int16_t));
        return;
    }
    std::memcpy(buf, bytes, samples * sizeof(int16_t));

    PcmBuf item{buf, samples};
    if (xQueueSend(playQueue_, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
        mclog::tagWarn(_tag, "play queue full, dropping chunk");
        heap_caps_free(buf);
    }
}

bool AudioPipeline::isPlaying() const
{
    if (playQueue_ == nullptr) {
        return false;
    }
    return writing_.load() || uxQueueMessagesWaiting(playQueue_) > 0;
}

void AudioPipeline::flushPlayback()
{
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
    std::vector<int16_t> scratch;
    scratch.reserve(kWriteChunk);
    while (running_.load()) {
        PcmBuf item;
        if (xQueueReceive(playQueue_, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
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
        // Only clear writing_ once the queue is empty, so isPlaying() stays true
        // across back-to-back chunks within a turn.
        if (uxQueueMessagesWaiting(playQueue_) == 0) {
            writing_ = false;
        }
    }
    writing_ = false;
}

}  // namespace custom_agent
