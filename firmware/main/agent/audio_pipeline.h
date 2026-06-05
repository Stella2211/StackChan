/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <vector>
#include <cstdint>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class AudioCodec;  // from <audio/audio_codec.h>

namespace custom_agent {

/**
 * @brief Microphone capture + speaker playback for the custom agent.
 *
 * Capture: reads raw frames from the duplex codec (24kHz, interleaved -- 2ch
 *          [mic, AEC-ref] on CoreS3) and resamples to 16kHz while keeping every
 *          channel. The interleaved 16kHz frame is fed to the ESP-SR AFE
 *          (wake-word or voice-processing), which owns mic extraction / NS / VAD
 *          and emits the processed mono the backend expects. The reference channel
 *          is preserved so device AEC can be enabled later.
 *
 * Playback: a background task drains a queue of 24kHz mono PCM buffers straight
 *           to the codec (the backend already outputs 24kHz, matching the codec
 *           output rate, so no resampling is needed). Writes pace at real time,
 *           which naturally rate-limits playback.
 *
 * The engine enforces half-duplex (it stops sending mic while audio is playing).
 */
class AudioPipeline {
public:
    static constexpr int kOutRate    = 16000;  // backend / AFE input rate
    static constexpr int kFrameMs    = 20;     // capture frame size
    static constexpr int kOutSamples = kOutRate * kFrameMs / 1000;  // 320 (per channel)

    bool init();
    void deinit();

    /// Read one ~20ms codec frame, all input channels interleaved, resampled to
    /// 16kHz. On CoreS3 that is 2ch [mic, AEC-ref]; channels are preserved so the
    /// AFE keeps its reference input. `out` is resized to ~kOutSamples * channels.
    /// Returns false on read failure.
    bool captureFrame2ch16k(std::vector<int16_t>& out);

    /// Number of interleaved input channels the codec provides (1 or 2).
    int inputChannels() const { return inChannels_; }

    /// Queue one chunk of 24kHz mono s16le PCM for playback (bytes are copied).
    void enqueuePcm(const uint8_t* bytes, size_t len);

    /// True while there is audio queued or actively being written.
    bool isPlaying() const;

    /// Drop any queued/playing audio immediately (e.g. on disconnect).
    void flushPlayback();

private:
    void playTask();
    static void playTaskTrampoline(void* arg);

    AudioCodec* codec_ = nullptr;
    int inRate_        = 24000;
    int inChannels_    = 2;

    // Linear resampler state (24k -> 16k), carried across frames. The phase is
    // shared by every channel (so they stay sample-aligned); the last sample of
    // each channel is kept for interpolation across the block boundary.
    float resamplePos_ = 0.0f;
    std::vector<int16_t> prevSamples_;  // one per input channel

    // Scratch buffer reused across capture calls (interleaved codec read).
    std::vector<int16_t> readBuf_;

    // Playback
    QueueHandle_t playQueue_   = nullptr;
    TaskHandle_t playTaskHdl_  = nullptr;
    std::atomic<bool> writing_{false};
    std::atomic<bool> running_{false};
};

}  // namespace custom_agent
