/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "agent_config.h"
#include <functional>
#include <memory>
#include <string>
#include <cstdint>
#include <atomic>

class WebSocket;  // from <web_socket.h>

namespace custom_agent {

/**
 * @brief Audio container of a server audio segment, taken from
 *        `output.audio.start.format.mimeType`.
 *        Pcm = "audio/pcm;rate=16000" (route1/live), Wav = "audio/wav" (route2/agent).
 *        NOTE: the device plays at the codec's fixed 16kHz output rate and does not
 *        resample; the format's sampleRate is informational only.
 */
enum class AudioMime : uint8_t {
    Unknown = 0,
    Pcm,
    Wav,
    Opus,  // "audio/opus" (route2/agent, compressed downlink) -- length-prefixed Opus packets
};

/**
 * @brief Callbacks fired from the WebSocket receive task. Keep handlers light;
 *        do not block (the receive task also drains the socket).
 */
struct BackendCallbacks {
    std::function<void()> onReady;                                       // "ready"
    std::function<void()> onConnected;                                   // ws handshake done
    std::function<void()> onDisconnected;                               // ws closed
    std::function<void(const std::string& text)> onText;               // "output.text"
    std::function<void(const std::string& id, AudioMime mime)> onAudioStart;  // "output.audio.start"
    std::function<void(const uint8_t* data, size_t len)> onAudioChunk; // binary frame of current segment
    std::function<void(const std::string& id)> onAudioEnd;             // "output.audio.end"
    std::function<void()> onTurnComplete;                              // "turn.complete"
    std::function<void(const std::string& message, bool fatal)> onError;  // "error"
    std::function<void(const std::string& name, const std::string& status, const std::string& summary)>
        onToolCall;  // "tool.call" (display only)
    // "action.start": a built-in device tool the LLM asked us to run (move_head /
    // play_gesture / set_expression / capture_image). `args` is the JSON object of
    // the tool arguments, serialized to a string (or "{}" when absent). Keep the
    // handler light: it fires from the WS receive task, so it must NOT execute the
    // tool inline (camera capture/JPEG encode would block the socket) -- enqueue it
    // to a worker instead. See agent.cpp / agent_tools.cpp.
    std::function<void(const std::string& id, const std::string& name, const std::string& args)> onAction;
    // "session.started": the server confirmed a logical session (and assigned its id
    // if we omitted one). Display/log only -- keeping the id is optional.
    std::function<void(const std::string& sessionId)> onSessionStarted;
    // "session.closed": the server ended the session. `reason` is one of
    // tool / client / idle / restart (treat as an opaque string). The device should
    // return to the sleeping (wake-word) state.
    std::function<void(const std::string& sessionId, const std::string& reason)> onSessionClosed;
};

/**
 * @brief WebSocket client speaking the StackChan<->backend protocol
 *        (raw PCM binary frames + JSON text control), see
 *        backend/docs/websocket-protocol.md.
 */
class BackendClient {
public:
    explicit BackendClient(Config cfg);
    ~BackendClient();

    void setCallbacks(BackendCallbacks cb)
    {
        cb_ = std::move(cb);
    }

    /// Connect (blocking handshake). Returns true on success.
    bool connect();
    bool isConnected() const;
    bool isReady() const
    {
        return ready_.load();
    }
    void close();

    /* ----------------------------- client -> server ----------------------------- */
    /// Begin a logical (multi-turn) session, sent on tap / wake word before the first
    /// utterance. An empty `sessionId` lets the server assign one (returned via
    /// `session.started`).
    bool sendSessionStart(const std::string& sessionId = "");
    /// Request session end (e.g. the device's "1 minute of silence after playback"
    /// timer). The server replies with `session.closed { reason: "client" }`.
    bool sendSessionEnd();
    /// Declare the start of an utterance (pcm_s16le / 16000 / mono).
    bool sendInputAudioStart();
    /// Send one chunk of raw 16k mono PCM (binary frame).
    bool sendAudioChunk(const int16_t* pcm16k, size_t samples);
    /// Terminate the utterance; the server starts generating the response.
    bool sendInputAudioEnd();

    /* ------------------------- device tool results / images ------------------------- */
    /// Acknowledge a device tool (`action.start`). `summary` is optional (display/log).
    bool sendActionResult(const std::string& id, bool ok, const std::string& summary = "");
    /// Begin a camera image stream. `actionId` echoes the `capture_image` action id.
    bool sendImageStart(const std::string& id, const std::string& actionId,
                        const std::string& format = "image/jpeg");
    /// Send one chunk of the JPEG payload (binary frame).
    bool sendImageChunk(const uint8_t* data, size_t len);
    /// Terminate the image stream.
    bool sendImageEnd(const std::string& id);

private:
    void wireHandlers();
    void handleText(const char* data, size_t len);

    Config cfg_;
    std::unique_ptr<WebSocket> ws_;
    BackendCallbacks cb_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> ready_{false};
    // Cleared at the start of close() so the WS receive task stops dispatching into
    // cb_ before ws_ is torn down (defense-in-depth against a teardown/callback race
    // on reconnect). Checked at the top of every ws_ handler.
    std::atomic<bool> alive_{true};
    AudioMime current_mime_ = AudioMime::Unknown;
};

}  // namespace custom_agent
