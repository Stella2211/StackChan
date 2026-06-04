/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "agent.h"
#include "agent_config.h"
#include "backend_client.h"
#include "agent_tools.h"
#include "audio_pipeline.h"
#include "tailscale.h"

#include <hal/hal.h>
#include <hal/board/stackchan_display.h>
#include <stackchan/stackchan.h>
#include <board.h>
#include <assets/lang_config.h>
#include <apps/common/common.h>

#include <mooncake_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>
#include <cstdlib>
#include <cstring>

namespace custom_agent {

static constexpr const char* _tag = "agent";

/* -------------------------------------------------------------------------- */
/*                                   State                                     */
/* -------------------------------------------------------------------------- */

enum class State {
    Idle,        // listening for speech onset
    Capturing,   // streaming the user's utterance
    Responding,  // waiting for / playing the server response
};

namespace {

Config g_cfg;
std::unique_ptr<BackendClient> g_client;
AudioPipeline g_audio;

std::atomic<State> g_state{State::Idle};
std::atomic<bool> g_turnComplete{false};
std::atomic<bool> g_speakingShown{false};
std::atomic<bool> g_needHeaderStrip{false};

// g_client is created/reset on the capture-loop task but is also touched by the
// action worker task (to send results / image frames). This mutex guards those
// cross-task accesses so a reconnect can't free the client out from under a send.
std::mutex g_clientMutex;

// One pending device tool ("action.start"). Queued as a heap pointer because a
// FreeRTOS queue byte-copies its items and std::string isn't trivially copyable.
struct ActionItem {
    std::string id;
    std::string name;
    std::string args;
};
QueueHandle_t g_actionQueue = nullptr;

}  // namespace

/* -------------------------------------------------------------------------- */
/*                               Face helpers                                  */
/* -------------------------------------------------------------------------- */

static StackChanAvatarDisplay* face()
{
    return static_cast<StackChanAvatarDisplay*>(Board::GetInstance().GetDisplay());
}

static void faceStatus(const char* status)
{
    face()->SetStatus(status);
}

static void faceSpeech(const char* role, const char* content)
{
    face()->SetChatMessage(role, content);
}

/* -------------------------------------------------------------------------- */
/*                       StackChan per-frame update task                       */
/* -------------------------------------------------------------------------- */

// Mirrors hal.cpp's _stackchan_update_task: drives the avatar / motion / modifiers
// (blink, breath, head-pet, speaking, idle) at ~50Hz and keeps the status bar /
// home indicator alive so the user can swipe back to the launcher.
static void update_task(void*)
{
    bool setup_done = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));

        LvglLockGuard lock;

        GetStackChan().update();

        if (!setup_done) {
            view::create_home_indicator([]() { GetHAL().requestWarmReboot(0); }, 0x81DBBD, 0x134233);
            view::create_status_bar(0x81DBBD, 0x134233);
            setup_done = true;
        }

        view::update_home_indicator();
        view::update_status_bar();
    }
}

/* -------------------------------------------------------------------------- */
/*                          WAV header handling (route2)                       */
/* -------------------------------------------------------------------------- */

// Locate the start of PCM samples inside the first chunk of a WAV segment by
// walking the RIFF chunk list to the "data" sub-chunk. Returns 0 if it doesn't
// look like a WAV (RIFF/WAVE) so the caller plays the bytes unchanged. Robust to
// LIST/fact chunks preceding "data".
static size_t wav_data_offset(const uint8_t* data, size_t len)
{
    if (len < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
        return 0;  // not a WAV
    }
    // Each sub-chunk is [4-byte id][4-byte LE size][payload], word-aligned.
    for (size_t i = 12; i + 8 <= len;) {
        const uint32_t sz = static_cast<uint32_t>(data[i + 4]) | (static_cast<uint32_t>(data[i + 5]) << 8) |
                            (static_cast<uint32_t>(data[i + 6]) << 16) | (static_cast<uint32_t>(data[i + 7]) << 24);
        if (std::memcmp(data + i, "data", 4) == 0) {
            return i + 8;  // PCM payload starts right after the data header
        }
        // Advance to the next (word-aligned) chunk. advance is >= 8 for any valid
        // size, so i strictly increases and the loop always terminates (bounded by
        // i + 8 <= len). A sub-8 result can only come from a 32-bit overflow on a
        // garbage size field; stop in that case (defensive — the backend's WAVs are
        // well-formed).
        const size_t advance = static_cast<size_t>(8) + sz + (sz & 1u);
        if (advance < 8) {
            break;
        }
        i += advance;
    }
    return 44;  // valid WAV but 'data' not in this chunk: assume canonical header
}

/* -------------------------------------------------------------------------- */
/*                         Device tool worker (action.*)                       */
/* -------------------------------------------------------------------------- */

// Send wrappers used by the worker. Each grabs g_clientMutex so it never races a
// reconnect: if the client was just torn down it returns false instead of touching
// freed memory. The heavy work (camera grab / JPEG encode) happens in the tool
// before any of these are called, so the lock is only ever held briefly.
static bool client_send_action_result(const std::string& id, bool ok, const std::string& summary)
{
    std::lock_guard<std::mutex> lk(g_clientMutex);
    return g_client ? g_client->sendActionResult(id, ok, summary) : false;
}
static bool client_send_image_start(const std::string& id, const std::string& actionId, const std::string& fmt)
{
    std::lock_guard<std::mutex> lk(g_clientMutex);
    return g_client ? g_client->sendImageStart(id, actionId, fmt) : false;
}
static bool client_send_image_chunk(const uint8_t* data, size_t len)
{
    std::lock_guard<std::mutex> lk(g_clientMutex);
    return g_client ? g_client->sendImageChunk(data, len) : false;
}
static bool client_send_image_end(const std::string& id)
{
    std::lock_guard<std::mutex> lk(g_clientMutex);
    return g_client ? g_client->sendImageEnd(id) : false;
}

// Drains pending actions (e.g. on disconnect) so a reconnect doesn't replay tools
// the previous session asked for.
static void drain_action_queue()
{
    if (g_actionQueue == nullptr) {
        return;
    }
    ActionItem* item = nullptr;
    while (xQueueReceive(g_actionQueue, &item, 0) == pdTRUE) {
        delete item;
    }
}

// Runs built-in device tools off the WS receive task. Serialized (one tool at a
// time), at a lower priority than audio, so a camera capture/encode or a gesture's
// inter-keyframe sleeps never stall the socket or the voice path.
//
// Half-duplex note: capture_image streams image binary frames from THIS task while
// the capture loop sends mic-audio binary frames from its own. The server tells the
// two apart by framing (input.image.start..end => image, else audio), so they must
// not interleave. They don't today: capture_image only runs in the Responding state,
// during which the capture loop is half-duplex and sends no audio. If that half-
// duplex assumption ever changes, image and audio sends must be mutually excluded.
static void action_worker_task(void*)
{
    ActionSink sink;
    sink.result     = [](const std::string& id, bool ok, const std::string& s) { client_send_action_result(id, ok, s); };
    sink.imageStart  = [](const std::string& id, const std::string& a, const std::string& f) { client_send_image_start(id, a, f); };
    sink.imageChunk  = [](const uint8_t* d, size_t n) { client_send_image_chunk(d, n); };
    sink.imageEnd    = [](const std::string& id) { client_send_image_end(id); };

    while (1) {
        ActionItem* item = nullptr;
        if (xQueueReceive(g_actionQueue, &item, portMAX_DELAY) == pdTRUE && item != nullptr) {
            execute_action(sink, item->id, item->name, item->args);
            delete item;
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                           Backend event callbacks                          */
/* -------------------------------------------------------------------------- */

static void wire_callbacks()
{
    BackendCallbacks cb;

    cb.onReady = []() {
        g_state         = State::Idle;
        g_turnComplete  = false;
        g_speakingShown = false;
        faceStatus(Lang::Strings::STANDBY);
        faceSpeech("system", "どうぞ話しかけてください");
    };

    cb.onConnected = []() { mclog::tagInfo(_tag, "connected"); };

    cb.onDisconnected = []() {
        g_audio.flushPlayback();
        drain_action_queue();
        g_state         = State::Idle;
        g_turnComplete  = false;
        g_speakingShown = false;
        faceSpeech("system", "切断されました");
    };

    cb.onText = [](const std::string& text) { faceSpeech("assistant", text.c_str()); };

    cb.onAudioStart = [](const std::string& /*id*/, AudioMime mime) {
        g_state          = State::Responding;
        g_needHeaderStrip = (mime == AudioMime::Wav);
        if (!g_speakingShown.exchange(true)) {
            faceStatus(Lang::Strings::SPEAKING);
        }
    };

    cb.onAudioChunk = [](const uint8_t* data, size_t len) {
        if (g_needHeaderStrip.exchange(false)) {
            const size_t off = wav_data_offset(data, len);
            if (off < len) {
                g_audio.enqueuePcm(data + off, len - off);
            }
        } else {
            g_audio.enqueuePcm(data, len);
        }
    };

    cb.onAudioEnd = [](const std::string& /*id*/) {
        // Segment boundary only; the next output.audio.start resets header strip.
    };

    cb.onTurnComplete = []() { g_turnComplete = true; };

    cb.onToolCall = [](const std::string& name, const std::string& status, const std::string& summary) {
        if (status == "start") {
            faceSpeech("system", (std::string("処理中: ") + name).c_str());
        } else if (status == "result" && !summary.empty()) {
            faceSpeech("system", summary.c_str());
        }
    };

    // A built-in device tool the LLM invoked. Hand it to the worker task; never run
    // it here (this fires from the WS receive task, which must keep draining the
    // socket -- a camera capture would block it).
    cb.onAction = [](const std::string& id, const std::string& name, const std::string& args) {
        if (g_actionQueue == nullptr) {
            return;
        }
        auto* item = new ActionItem{id, name, args};
        if (xQueueSend(g_actionQueue, &item, 0) != pdTRUE) {
            mclog::tagWarn(_tag, "action queue full, dropping '{}'", name);
            delete item;
            // Fail the action fast so a dropped capture_image doesn't make the server
            // wait out its image timeout (~10s). Cheap to send from here: it's a tiny
            // frame and the WS send path is independent of this receive callback.
            client_send_action_result(id, false, "busy");
        }
    };

    cb.onError = [](const std::string& message, bool /*fatal*/) {
        // For fatal errors the server closes the connection itself; onDisconnected
        // then drives reconnect from the capture loop. Avoid tearing down the
        // client from inside its own callback thread.
        faceSpeech("system", message.empty() ? "エラー" : message.c_str());
    };

    g_client->setCallbacks(std::move(cb));
}

/* -------------------------------------------------------------------------- */
/*                              Connection bring-up                            */
/* -------------------------------------------------------------------------- */

static bool connect_with_retry()
{
    for (int attempt = 1;; ++attempt) {
        faceSpeech("system", "サーバーに接続中...");
        {
            // Guarded so the action worker never sends through a half-swapped client.
            std::lock_guard<std::mutex> lk(g_clientMutex);
            g_client = std::make_unique<BackendClient>(g_cfg);
        }
        wire_callbacks();

        if (g_client->connect()) {
            // Wait for the server "ready" handshake.
            for (int i = 0; i < 100; ++i) {
                if (g_client->isReady()) {
                    return true;
                }
                if (!g_client->isConnected()) {
                    break;
                }
                GetHAL().delay(100);
            }
        }

        mclog::tagWarn(_tag, "connect attempt {} failed, retrying", attempt);
        faceSpeech("system", "接続失敗。再試行します");
        {
            std::lock_guard<std::mutex> lk(g_clientMutex);
            g_client.reset();
        }
        GetHAL().delay(3000);
    }
}

/* -------------------------------------------------------------------------- */
/*                                Capture loop                                 */
/* -------------------------------------------------------------------------- */

static int mean_abs(const std::vector<int16_t>& frame)
{
    if (frame.empty()) {
        return 0;
    }
    int64_t sum = 0;
    for (int16_t s : frame) {
        sum += std::abs(static_cast<int>(s));
    }
    return static_cast<int>(sum / static_cast<int64_t>(frame.size()));
}

static void capture_loop()
{
    constexpr int kFrameMs  = AudioPipeline::kFrameMs;  // 20
    const int prerollFrames = 300 / kFrameMs;           // ~300ms pre-roll

    std::deque<std::vector<int16_t>> preroll;
    std::vector<int16_t> frame;
    int onsetMs    = 0;  // accumulated voiced time of a tentative onset
    int onsetGapMs = 0;  // trailing gap within a tentative onset (cancels it if long)
    int speechMs   = 0;
    int silenceMs  = 0;

    while (1) {
        // Reading paces this loop at ~one frame per kFrameMs (codec is real-time).
        if (!g_audio.captureFrame16k(frame)) {
            GetHAL().delay(kFrameMs);
            continue;
        }

        if (g_client == nullptr || !g_client->isConnected()) {
            connect_with_retry();
            // Fresh connection: drop any half-built capture state so the first
            // post-reconnect utterance doesn't inherit stale pre-roll / VAD counters.
            preroll.clear();
            onsetMs    = 0;
            onsetGapMs = 0;
            speechMs   = 0;
            silenceMs  = 0;
            continue;
        }
        if (!g_client->isReady()) {
            continue;
        }

        const int energy = mean_abs(frame);

        switch (g_state.load()) {
            case State::Idle: {
                // Don't listen while the tail of a response is still playing.
                if (g_audio.isPlaying()) {
                    break;
                }

                preroll.push_back(frame);

                // Accumulate voiced time of a tentative onset. Brief intra-word dips
                // (still above vadKeepRms) keep building; only a sustained gap
                // (>= vadSilenceMs) cancels the onset. This is what discards short
                // blips (coughs/clicks) that never reach vadMinSpeechMs.
                if (energy >= g_cfg.vadStartRms) {
                    onsetMs += kFrameMs;
                    onsetGapMs = 0;
                } else if (onsetMs > 0) {
                    if (energy >= g_cfg.vadKeepRms) {
                        onsetMs += kFrameMs;
                        onsetGapMs = 0;
                    } else {
                        onsetGapMs += kFrameMs;
                        if (onsetGapMs >= g_cfg.vadSilenceMs) {
                            onsetMs    = 0;  // tentative onset was just noise
                            onsetGapMs = 0;
                        }
                    }
                }

                // Until an onset is building, keep only the rolling pre-roll window.
                // While it builds, retain the whole head so nothing is lost on commit
                // (bounded so a never-committing case can't grow without limit).
                const int headCap = prerollFrames + g_cfg.vadMinSpeechMs / kFrameMs + 4;
                const int trimTo  = (onsetMs > 0) ? headCap : prerollFrames;
                while (static_cast<int>(preroll.size()) > trimTo) {
                    preroll.pop_front();
                }

                // Commit only once enough voiced audio has accumulated. Gating on
                // vadMinSpeechMs (not a short debounce) rejects sub-threshold noises.
                if (onsetMs >= g_cfg.vadMinSpeechMs) {
                    if (!g_client->sendInputAudioStart()) {
                        onsetMs    = 0;
                        onsetGapMs = 0;
                        break;
                    }
                    faceStatus(Lang::Strings::LISTENING);
                    faceSpeech("system", "");
                    const int flushed = static_cast<int>(preroll.size());
                    for (auto& f : preroll) {
                        g_client->sendAudioChunk(f.data(), f.size());
                    }
                    preroll.clear();
                    // The pre-roll already contains the onset frames.
                    speechMs   = flushed * kFrameMs;
                    silenceMs  = 0;
                    onsetMs    = 0;
                    onsetGapMs = 0;
                    g_state    = State::Capturing;
                }
                break;
            }

            case State::Capturing: {
                g_client->sendAudioChunk(frame.data(), frame.size());
                speechMs += kFrameMs;

                if (energy < g_cfg.vadKeepRms) {
                    silenceMs += kFrameMs;
                } else {
                    silenceMs = 0;
                }

                const bool tooLong   = speechMs >= g_cfg.vadMaxUtteranceMs;
                const bool endOfSpeech = silenceMs >= g_cfg.vadSilenceMs;

                if (endOfSpeech || tooLong) {
                    g_client->sendInputAudioEnd();
                    g_turnComplete  = false;
                    g_speakingShown = false;
                    g_state         = State::Responding;
                    speechMs        = 0;
                    silenceMs       = 0;
                }
                break;
            }

            case State::Responding: {
                // Mic is ignored while responding (half-duplex). When the server
                // says the turn is done and all audio has drained, go back to idle.
                if (g_turnComplete.load() && !g_audio.isPlaying()) {
                    g_state = State::Idle;
                    faceStatus(Lang::Strings::STANDBY);
                    // Start the next utterance from a clean slate (no stale pre-roll
                    // captured during the previous turn / response tail).
                    preroll.clear();
                    onsetMs    = 0;
                    onsetGapMs = 0;
                }
                break;
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                                   start()                                   */
/* -------------------------------------------------------------------------- */

void start()
{
    mclog::tagInfo(_tag, "start custom agent");

    g_cfg = load_config();

    // 1) Motion behaviour (same as the xiaozhi path).
    auto& motion = GetStackChan().motion();
    motion.setAutoAngleSyncEnabled(true);
    motion.setAutoTorqueReleaseEnabled(true);

    // 2) Create the avatar/face and hide the boot logo.
    {
        LvglLockGuard lock;
        face()->SetupUI();
        GetHAL().bootLogo.reset();
    }

    // 2b) Swap the speech-bubble font to the full-CJK cbin font embedded in the assets
    //     partition. The xiaozhi path does this via Assets::Apply(); we bypass xiaozhi,
    //     so without this the bubble keeps the compiled "basic" font and can't render
    //     most Japanese kana/kanji. Called outside the lock above (SetTheme locks itself).
    face()->ApplyAssetsTextFont();

    // 3) Per-frame avatar update task (blink/breath/head-pet/speaking/idle).
    xTaskCreatePinnedToCore(update_task, "agent_face", 4096, nullptr, 3, nullptr, 1);

    // 3b) Device-tool worker (move_head / play_gesture / set_expression /
    //     capture_image). Lower priority than the face task; unpinned so the
    //     ~hundreds-of-ms JPEG encode can land on whichever core is free. The
    //     software JPEG encoder runs on THIS task's stack (image_to_jpeg with
    //     task_enable=false), on top of ArduinoJson + libc, so the stack is sized
    //     generously (12 KB) to leave headroom.
    g_actionQueue = xQueueCreate(8, sizeof(ActionItem*));
    xTaskCreatePinnedToCore(action_worker_task, "agent_action", 12288, nullptr, 2, nullptr, tskNO_AFFINITY);

    // 4) Idle face while we connect.
    faceStatus(Lang::Strings::STANDBY);

    // 5) Network (blocks until Wi-Fi is up; enters config mode if unprovisioned).
    faceSpeech("system", "ネットワーク接続中...");
    GetHAL().startNetwork([](std::string_view msg) { mclog::tagInfo(_tag, "net: {}", msg); });

    // 5b) Tailscale (MicroLink) tunnel, if provisioned. No-op AND silent otherwise
    //     (the status sink fires only on the enabled path). Makes a tailnet-only
    //     backend reachable; the WebSocket transport is unchanged because lwIP
    //     routes 100.64.0.0/10 through the WireGuard netif.
    tailscale_bring_up(g_cfg, [](const char* msg) { faceSpeech("system", msg); });

    // 6) Audio I/O.
    if (!g_audio.init()) {
        faceSpeech("system", "オーディオ初期化失敗");
        mclog::tagError(_tag, "audio init failed");
    }

    // 7) Connect to the backend and wait for ready.
    connect_with_retry();

    // 8) Run forever.
    capture_loop();
}

}  // namespace custom_agent
