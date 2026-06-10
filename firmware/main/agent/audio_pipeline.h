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
#include <freertos/task.h>
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

    /// Read one ~20ms codec frame, all input channels interleaved, at 16kHz. The
    /// board runs the codec at 16kHz natively (AUDIO_INPUT_SAMPLE_RATE), so this is
    /// a straight passthrough; the linear resampler below only kicks in if a board
    /// reports a different input rate. On CoreS3 that is 2ch [mic, AEC-ref]; channels
    /// are preserved so the AFE keeps its reference input. `out` is resized to
    /// ~kOutSamples * channels. Returns false on read failure.
    bool captureFrame2ch16k(std::vector<int16_t>& out);

    /// Number of interleaved input channels the codec provides (1 or 2).
    int inputChannels() const { return inChannels_; }

    /// Queue one chunk of 16kHz mono s16le PCM for playback (bytes are copied).
    /// The codec output runs at AUDIO_OUTPUT_SAMPLE_RATE (16kHz) with no resampling,
    /// so the server must send PCM at this rate (output.audio.start.format is
    /// informational only -- it is not honored on playback).
    void enqueuePcm(const uint8_t* bytes, size_t len);

    /// Queue one WS binary frame of the Opus-compressed downlink (mimeType
    /// "audio/opus", route2/agent). The frame is a concatenation of
    /// `[uint16LE packetLen][opus packet]` records (packets never span a WS frame);
    /// each packet is decoded to 16kHz mono PCM and queued for playback. The Opus
    /// decoder (esp_audio_codec) is opened lazily on first use. Compressed bytes are
    /// counted toward the segment receive stats so the SEG throughput/ratio reflect the
    /// wire, while playback duration is measured from the decoded packet count.
    ///
    /// IMPORTANT: this runs on the WS receive task ("tcp_receive", 4KB stack). Opus
    /// decode needs more stack than that (decoding inline overflowed it), so this only
    /// copies the frame and hands it to a dedicated decode task (PSRAM stack); the actual
    /// esp_opus_dec_decode happens there.
    void enqueueOpusMessage(const uint8_t* data, size_t len);

    /// True while there is audio queued or actively being written.
    bool isPlaying() const;

    /// Drop any queued/playing audio immediately (e.g. on disconnect).
    void flushPlayback();

    /* --------------------------- diagnostics (temporary) ---------------------------
     * Localize the "slow / choppy response audio" complaint WITHOUT per-chunk logging
     * (which would itself throttle the WG/RX path and reproduce the very dropouts we
     * are chasing -- see the audio-dropout-throughput note). Everything below is
     * emitted only at turn / segment boundaries: a handful of lines per response.
     *
     *   noteTurnStart() -- call when input.audio.end is sent (utterance committed).
     *                      Anchors the end-to-end "time to first audible sample".
     *   beginSegment()  -- output.audio.start: reset the per-segment receive counters.
     *   endSegment()    -- output.audio.end: log one summary line -- network throughput,
     *                      intrinsic audio length vs receive wall-clock, server
     *                      time-to-first-byte, play-queue low-watermark and playback
     *                      underruns -- so a network bottleneck (ratio>100%, low KB/s)
     *                      is distinguishable from a CPU/scheduling one (ratio<100% yet
     *                      the queue still drained). All three callers run on the single
     *                      WS receive task, so the per-segment counters need no locking.
     */
    void noteTurnStart();
    void beginSegment();
    void endSegment();

private:
    void playTask();
    static void playTaskTrampoline(void* arg);

    /// Copy `count` 16kHz mono samples into a PSRAM buffer and queue them for playback.
    /// Shared by the raw-PCM path (WS-rx task) and the Opus decode task.
    void pushPcmSamples(const int16_t* samples, size_t count);
    /// Per-segment receive accounting for one arriving WS audio frame (`bytes` = the
    /// bytes that crossed the wire: raw PCM, or compressed Opus). WS-rx task only.
    void noteRxBytes(size_t bytes);
    /// Lazily create the Opus decode task + input queue on first use (WS-rx task).
    /// False (and Opus audio dropped) on failure.
    bool ensureOpusPipeline();
    /// Open the Opus decoder on first use (called on the decode task). False on failure.
    bool ensureOpusDecoder();
    /// Decode task body: drains opusInQueue_, decodes packets, feeds the play queue.
    void opusDecodeTask();
    static void opusDecodeTaskTrampoline(void* arg);

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

    // Opus downlink decoder (esp_audio_codec). Decode runs on a dedicated task because
    // esp_opus_dec_decode needs more stack than the 4KB WS receive task ("tcp_receive")
    // has -- decoding inline there overflowed it. The WS-rx task only copies each frame
    // into opusInQueue_; the decode task (PSRAM stack, so it doesn't eat the tight
    // internal-RAM budget) decodes and feeds the play queue. All Opus members except the
    // queue/task handles are touched only on the decode task.
    void* opusDec_           = nullptr;  // esp_opus_dec handle (decode task)
    bool opusOpenTried_      = false;    // don't retry a hard open failure (decode task)
    int16_t* opusFrameBuf_   = nullptr;  // PSRAM scratch for one decoded Opus frame (decode task)
    unsigned opusErrCount_   = 0;        // rate-limit decode-error logging (decode task)
    QueueHandle_t opusInQueue_ = nullptr;  // raw Opus WS frames (WS-rx -> decode task)
    TaskHandle_t opusTaskHdl_  = nullptr;
    StaticTask_t* opusTcb_     = nullptr;  // internal RAM (scheduler/ISR touch it)
    StackType_t* opusStack_    = nullptr;  // PSRAM (decode task is compute-only, never runs cache-disabled)
    bool opusPipelineTried_  = false;      // create task+queue once (WS-rx task)

    // --- diagnostics (see noteTurnStart / beginSegment / endSegment) ---
    int outRate_ = kOutRate;                   // codec playback rate, for duration math
    std::atomic<int64_t> turnStartUs_{0};      // input.audio.end time (AFE-VC writes, play reads)
    std::atomic<bool> firstPlayLogged_{true};  // play task logs first-write latency once per turn
    // Per-segment receive accounting -- touched only on the WS receive task.
    int64_t segStartUs_     = 0;                // output.audio.start time
    int64_t segFirstRxUs_   = 0;                // first chunk arrival
    int64_t segLastRxUs_    = 0;                // last chunk arrival
    size_t segRxBytes_      = 0;                // bytes over the wire (raw PCM or compressed Opus)
    uint32_t segRxChunks_   = 0;
    std::atomic<uint32_t> segDrops_{0};        // play-queue-full drops (decode task writes, WS-rx reads)
    uint32_t segPlaySamples_ = 0;              // 16k samples queued for playback (decoded); = audio length
    bool segOpus_           = false;            // this segment arrived as Opus (vs raw PCM/WAV)
    // Playback-side accounting -- written on the play task, read at endSegment().
    std::atomic<int> playQueueMin_{0};         // play-queue depth low-watermark within the segment
    std::atomic<uint32_t> underruns_{0};       // times the queue ran dry mid-stream (audible gap)
    std::atomic<uint32_t> starvedMs_{0};       // cumulative ms the play task waited mid-stream
};

}  // namespace custom_agent
