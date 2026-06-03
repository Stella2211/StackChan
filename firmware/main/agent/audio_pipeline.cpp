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

static constexpr int kPlayQueueDepth = 16;

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
    micBuf_.assign(static_cast<size_t>(inFrames), 0);

    playQueue_ = xQueueCreate(kPlayQueueDepth, sizeof(PcmBuf));
    if (playQueue_ == nullptr) {
        mclog::tagError(_tag, "failed to create play queue");
        return false;
    }

    running_ = true;
    xTaskCreatePinnedToCore(playTaskTrampoline, "agent_play", 4096, this, 4, &playTaskHdl_, 1);

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

bool AudioPipeline::captureFrame16k(std::vector<int16_t>& out)
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
    const int total = static_cast<int>(readBuf_.size());

    // Extract mic channel (index 0 of each interleaved frame).
    const int inFrames = total / inChannels_;
    if (inChannels_ == 1) {
        std::memcpy(micBuf_.data(), readBuf_.data(), inFrames * sizeof(int16_t));
    } else {
        for (int i = 0; i < inFrames; ++i) {
            micBuf_[i] = readBuf_[static_cast<size_t>(i) * inChannels_];
        }
    }

    // No resampling needed if the codec already runs at the target rate.
    if (inRate_ == kOutRate) {
        out.assign(micBuf_.begin(), micBuf_.begin() + inFrames);
        return true;
    }

    // Linear resample inRate_ -> kOutRate, continuous across calls.
    const float step = static_cast<float>(inRate_) / static_cast<float>(kOutRate);
    out.clear();
    out.reserve(static_cast<size_t>(inFrames * kOutRate / inRate_) + 2);

    float t = resamplePos_;
    while (t < inFrames - 1) {
        const int i   = static_cast<int>(t);
        const float f = t - i;
        int16_t a     = (i < 0) ? prevSample_ : micBuf_[i];
        int16_t b     = micBuf_[i + 1];
        out.push_back(static_cast<int16_t>(a + (b - a) * f));
        t += step;
    }
    // Carry the fractional remainder into the next block.
    resamplePos_ = t - inFrames;
    prevSample_  = micBuf_[inFrames - 1];

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
