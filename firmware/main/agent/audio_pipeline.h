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
 * Capture: reads raw frames from the duplex codec (24kHz, 2ch interleaved
 *          [mic, aec-ref]), extracts the mic channel and resamples to 16kHz mono
 *          (the format the backend expects for input).
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
    static constexpr int kOutRate    = 16000;  // backend input rate
    static constexpr int kFrameMs    = 20;     // capture frame size
    static constexpr int kOutSamples = kOutRate * kFrameMs / 1000;  // 320

    bool init();
    void deinit();

    /// Read one ~20ms frame, mic-only, resampled to 16kHz mono.
    /// `out` is resized to ~kOutSamples. Returns false on read failure.
    bool captureFrame16k(std::vector<int16_t>& out);

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

    // Linear resampler state (24k -> 16k), carried across frames.
    float resamplePos_  = 0.0f;
    int16_t prevSample_ = 0;

    // Scratch buffers reused across capture calls.
    std::vector<int16_t> readBuf_;
    std::vector<int16_t> micBuf_;

    // Playback
    QueueHandle_t playQueue_   = nullptr;
    TaskHandle_t playTaskHdl_  = nullptr;
    std::atomic<bool> writing_{false};
    std::atomic<bool> running_{false};
};

}  // namespace custom_agent
