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

// xiaozhi-esp32 ESP-SR AFE engines (wake word + voice processing) reused as-is.
#include <wake_words/afe_wake_word.h>
#include <processors/afe_audio_processor.h>
#include <assets.h>
#include <audio_codec.h>
#include <model_path.h>
#include <ArduinoJson.hpp>

#include <hal/hal.h>
#include <hal/board/stackchan_display.h>
#include <stackchan/stackchan.h>
#include <stackchan/avatar/avatar.h>  // DefaultAvatar::getPanel() for tap-to-start
#include <board.h>
#include <assets/lang_config.h>
#include <apps/common/common.h>

#include <mooncake_log.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_bt.h>
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

// Session state machine (multi-turn). Sleeping is the only state in which nothing
// is sent to the backend; a tap / wake word opens a session and the device then
// loops Listening -> Capturing -> Responding -> Listening for as many turns as the
// user wants, until a 1-minute silence, an LLM end_session, or a server idle/restart
// closes it and we fall back to Sleeping.
enum class State {
    Sleeping,    // wake-word listening; no session, nothing sent
    Listening,   // session open, waiting for the user's next utterance (VAD onset)
    Capturing,   // streaming the user's utterance (VAD says speaking)
    Responding,  // waiting for / playing the server response (mic ignored, half-duplex)
};

namespace {

Config g_cfg;
std::unique_ptr<BackendClient> g_client;
AudioPipeline g_audio;

// ESP-SR AFE engines. Both are created once and kept alive; only one is "running"
// at a time (Start/Stop toggle, gated by state) so the CPU cost is one instance --
// the extra cost is PSRAM for the second instance. `g_models` is the model list we
// load from the assets partition and inject into both (see load_sr_models()).
srmodel_list_t* g_models = nullptr;
std::unique_ptr<AfeWakeWord> g_wake;        // sleeping: wake-word detection
std::unique_ptr<AfeAudioProcessor> g_vc;    // session:  NS + VAD + mono extraction

std::atomic<State> g_state{State::Sleeping};
std::atomic<bool> g_sessionActive{false};   // true between session.start and session end
std::atomic<bool> g_startRequested{false};  // tap / wake / head-pet entry (honored when sleeping)
std::atomic<bool> g_closeRequested{false};  // server sent session.closed; loop returns to sleep
std::atomic<bool> g_turnComplete{false};
std::atomic<bool> g_speakingShown{false};
std::atomic<bool> g_needHeaderStrip{false};

// Wake-word ("ハイ、スタックチャン") detection. DISABLED: it was not reliably triggering,
// and more importantly the WakeNet AFE instance it requires holds ~25KB of *internal*
// (DMA-capable) RAM permanently -- the exact pool WiFi/lwIP/WireGuard TX buffers need,
// which bottomed out at session start and broke audio/the tunnel (see git history /
// the HEAP[*] probes). Dropping it frees that ~25KB and de-fragments the DMA heap, which
// is the actual fix for the no-audio/no-response bug. Sessions still start via screen tap
// and head-pet. Flip back to true (and debug detection) once the RAM budget allows it.
constexpr bool kWakeWordEnabled = false;

// End the session after this much continuous post-playback silence (fixed, per spec).
constexpr int kSessionIdleMs = 60'000;
// AFE-VC OnOutput chunk size (OPUS_FRAME_DURATION_MS-equivalent). We do NOT Opus-encode;
// this just sets how much processed mono 16k PCM is handed to us per callback.
constexpr int kVcFrameMs = 60;

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

// Snapshot internal/DMA/PSRAM heap at a named point. Used to pinpoint which start()
// init step drains internal (DMA-capable) RAM -- the pool WiFi/lwIP/WireGuard need and
// that bottoms out at session start. Temporary diagnostic.
static void log_heap(const char* where)
{
    mclog::tagWarn(_tag, "HEAP[{}] int_free={} int_max_blk={} dma_free={} dma_max_blk={} psram_free={}",
                   where, (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                   (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA),
                   (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                   (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
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
/*                        SR models + AFE + session control                    */
/* -------------------------------------------------------------------------- */

// Load the ESP-SR model list (WakeNet / NS / VAD) from the 'assets' partition.
//
// This build embeds the models in 'assets' (there is no 'model' partition, so
// esp_srmodel_init("model") would fail), and the custom-agent path deliberately
// bypasses xiaozhi's Assets::Apply() (which is what would normally call
// srmodel_load + AudioService::SetModelsList). So we replicate just the model load
// here, using the same Assets singleton the speech-bubble font already reads from,
// and inject the result into the two AFE engines. Returns nullptr on failure
// (voice features are then disabled but the rest of the agent still runs).
static srmodel_list_t* load_sr_models()
{
    auto& assets = Assets::GetInstance();

    void* ptr   = nullptr;
    size_t size = 0;
    if (!assets.GetAssetData("index.json", ptr, size)) {
        mclog::tagError(_tag, "assets index.json not found; SR models unavailable");
        return nullptr;
    }

    ArduinoJson::JsonDocument doc;
    if (ArduinoJson::deserializeJson(doc, static_cast<const char*>(ptr), size)) {
        mclog::tagError(_tag, "assets index.json parse failed");
        return nullptr;
    }
    const char* name = doc["srmodels"].as<const char*>();
    if (name == nullptr) {
        mclog::tagError(_tag, "assets index.json has no srmodels entry");
        return nullptr;
    }
    // Copy the filename before the next GetAssetData() reuses ptr/size.
    const std::string srmodels_file = name;

    if (!assets.GetAssetData(srmodels_file, ptr, size)) {
        mclog::tagError(_tag, "srmodels file '{}' not found in assets", srmodels_file.c_str());
        return nullptr;
    }
    // srmodel_load references the data in place; the assets partition stays mmap'd
    // for the lifetime of the process, so the pointer remains valid.
    srmodel_list_t* models = srmodel_load(ptr);
    if (models == nullptr) {
        mclog::tagError(_tag, "srmodel_load failed");
    }
    return models;
}

// All three of the following run only on the session loop task, so they own the
// state machine without locking. The AFE Start/Stop calls are event-bit toggles
// (thread-safe), and feeding a stopped AFE is an internal no-op.

static void enter_sleeping()
{
    g_sessionActive  = false;
    g_closeRequested = false;
    g_startRequested = false;  // drop any tap queued during the session
    if (g_vc) {
        g_vc->Stop();
    }
    if (g_wake) {
        g_wake->Start();
    }
    g_state = State::Sleeping;
    faceStatus(Lang::Strings::STANDBY);
}

static void start_session()
{
    if (g_client) {
        g_client->sendSessionStart();  // empty id -> server assigns one
    }
    // Clear any close left over from the previous session (a late
    // session.closed{client} for our own end can land after we slept).
    g_closeRequested = false;
    g_sessionActive  = true;
    g_turnComplete   = false;
    g_speakingShown  = false;
    if (g_wake) {
        g_wake->Stop();
    }
    if (g_vc) {
        g_vc->Start();
    }
    g_state = State::Listening;
    faceStatus(Lang::Strings::LISTENING);
    faceSpeech("system", "");
}

static void end_session(bool notifyServer)
{
    if (notifyServer && g_client) {
        g_client->sendSessionEnd();  // -> server replies session.closed{reason:"client"}
    }
    enter_sleeping();
}

// Wire the AFE callbacks. These fire from the AFE engines' own internal tasks.
// We keep them light: the wake callback only raises a flag, and the voice
// callbacks frame the utterance. Framing (input.audio.start / chunks / end) is all
// driven from the single AFE-VC task, so the three stay correctly ordered without
// extra synchronization.
static void wire_afe()
{
    if (g_wake) {
        g_wake->OnWakeWordDetected([](const std::string& /*wake_word*/) {
            // AfeWakeWord stops itself on detection; just ask the loop to start.
            g_startRequested = true;
        });
    }

    if (!g_vc) {
        return;
    }

    g_vc->OnVadStateChange([](bool speaking) {
        if (!g_sessionActive.load()) {
            return;  // only meaningful inside an active session
        }
        if (speaking) {
            // Utterance onset -> begin streaming this turn.
            if (g_state.load() == State::Listening && g_client && g_client->sendInputAudioStart()) {
                g_state = State::Capturing;
                faceStatus(Lang::Strings::LISTENING);
            }
        } else {
            // Utterance end -> let the server generate the response.
            if (g_state.load() == State::Capturing) {
                if (g_client) {
                    g_client->sendInputAudioEnd();
                }
                g_turnComplete  = false;
                g_speakingShown = false;
                g_state         = State::Responding;
            }
        }
    });

    g_vc->OnOutput([](std::vector<int16_t>&& pcm) {
        // Processed mono 16k PCM. Send only while capturing: during Responding the
        // device is half-duplex (no audio frames), so this never interleaves with
        // the image binary frames the action worker may be sending.
        if (g_state.load() == State::Capturing && g_client) {
            g_client->sendAudioChunk(pcm.data(), pcm.size());
        }
    });
}

/* -------------------------------------------------------------------------- */
/*                           Backend event callbacks                          */
/* -------------------------------------------------------------------------- */

static void wire_callbacks()
{
    BackendCallbacks cb;

    cb.onReady = []() {
        // The session loop drives the actual Sleeping reset (AFE start) after the
        // connect handshake; here we only refresh the flags and the idle face.
        g_turnComplete  = false;
        g_speakingShown = false;
        faceStatus(Lang::Strings::STANDBY);
        faceSpeech("system", kWakeWordEnabled ? "「ハイ、スタックチャン」と呼びかけるか画面をタップしてください"
                                              : "画面をタップして話しかけてください");
    };

    cb.onConnected = []() { mclog::tagInfo(_tag, "connected"); };

    cb.onDisconnected = []() {
        g_audio.flushPlayback();
        drain_action_queue();
        // Drop the session immediately so the AFE-VC callbacks stop sending; the loop
        // detects the disconnect, reconnects, and resets to Sleeping.
        g_sessionActive = false;
        g_turnComplete  = false;
        g_speakingShown = false;
        faceSpeech("system", "切断されました");
    };

    cb.onSessionStarted = [](const std::string& sessionId) {
        mclog::tagInfo(_tag, "session started: {}", sessionId.c_str());
    };

    cb.onSessionClosed = [](const std::string& sessionId, const std::string& reason) {
        mclog::tagInfo(_tag, "session closed: {} (reason={})", sessionId.c_str(), reason.c_str());
        // Server-driven end (tool / idle / restart). The loop returns us to Sleeping
        // without sending session.end (the server already closed it). Only act while a
        // session is active: the server also echoes session.closed{reason:"client"}
        // for our OWN session.end, which arrives AFTER we already slept -- honoring a
        // stale close would tear down the *next* session right after it starts.
        if (g_sessionActive.load()) {
            g_closeRequested = true;
        }
    };

    cb.onText = [](const std::string& text) { faceSpeech("assistant", text.c_str()); };

    cb.onAudioStart = [](const std::string& /*id*/, AudioMime mime) {
        g_needHeaderStrip = (mime == AudioMime::Wav);
        // Go half-duplex for playback. Only force Responding from Listening; if we are
        // still Capturing, leave the Capturing->Responding transition to the AFE-VC
        // task so its input.audio.end is still sent (the utterance framing stays on a
        // single task -- no dropped boundary, no cross-task chunk-after-end race).
        State expected = State::Listening;
        g_state.compare_exchange_strong(expected, State::Responding);
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
/*                                Session loop                                 */
/* -------------------------------------------------------------------------- */

// Reads the codec and feeds the active AFE, owns the session state machine, and
// keeps the connection alive. Utterance framing (Listening -> Capturing ->
// Responding) is driven by the AFE-VC callbacks; this loop handles session
// open/close, the 1-minute idle timer, and the Responding -> Listening hand-back.
static void session_loop()
{
    constexpr int kFrameMs = AudioPipeline::kFrameMs;  // 20
    std::vector<int16_t> frame;
    int idleMs = 0;

    // Heap probe (temporary diagnostic). WiFi dynamic TX buffers + lwIP pbufs come
    // from internal/DMA-capable RAM, so a send-time ENOMEM (e.g. "STUN send failed:
    // 12") means INTERNAL RAM ran out -- not PSRAM. Log it every 5s so it interleaves
    // with ml_derp's HEARTBEAT and shows the level *at the moment of failure*. The
    // min_free water-mark catches a dip even if heap has recovered by the next print.
    TickType_t lastHeapLog = 0;

    while (1) {
        {
            const TickType_t now = xTaskGetTickCount();
            if ((now - lastHeapLog) * portTICK_PERIOD_MS >= 5000) {
                lastHeapLog = now;
                mclog::tagWarn(_tag,
                               "HEAP int_free={} int_min={} int_max_blk={} dma_free={} dma_max_blk={} psram_free={}",
                               (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                               (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                               (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                               (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA),
                               (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                               (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            }
        }

        // Reading paces this loop at ~one frame per kFrameMs (codec is real-time).
        // The frame is 16k with every input channel interleaved (mic + AEC-ref).
        if (!g_audio.captureFrame2ch16k(frame)) {
            GetHAL().delay(kFrameMs);
            continue;
        }

        if (g_client == nullptr || !g_client->isConnected()) {
            connect_with_retry();
            enter_sleeping();  // fresh connection: wake-word listening, no session
            idleMs = 0;
            continue;
        }
        if (!g_client->isReady()) {
            continue;
        }

        // ---- Sleeping: feed the wake-word engine; a tap / wake / head-pet opens a
        //      session. In-session taps are ignored (start_session only runs here).
        if (!g_sessionActive.load()) {
            if (g_wake) {
                g_wake->Feed(frame);
            }
            // Ignore (and clear) start requests when voice is disabled (no models /
            // codec): without the voice processor a session would just sit in
            // Listening with nothing to send. The face already shows the error.
            if (g_startRequested.exchange(false) && g_vc) {
                start_session();
                idleMs = 0;
            }
            continue;
        }

        // ---- Active session.
        // Server-driven close (tool / idle / restart) takes priority.
        if (g_closeRequested.exchange(false)) {
            end_session(/*notifyServer=*/false);
            idleMs = 0;
            continue;
        }

        // Feed the voice processor unless a response is playing (half-duplex: the mic
        // is dropped while Responding so the speaker output isn't picked up).
        if (g_state.load() != State::Responding) {
            if (g_vc) {
                g_vc->Feed(std::move(frame));
            }
        }

        switch (g_state.load()) {
            case State::Listening:
                // 1 minute of continuous post-playback silence -> client end. A VAD
                // onset (-> Capturing) or any response playback resets the timer.
                if (!g_audio.isPlaying()) {
                    idleMs += kFrameMs;
                    if (idleMs >= kSessionIdleMs) {
                        end_session(/*notifyServer=*/true);
                        idleMs = 0;
                    }
                }
                break;

            case State::Capturing:
                idleMs = 0;  // utterance framing handled by the AFE-VC callbacks
                break;

            case State::Responding:
                idleMs = 0;
                // Turn done and all audio drained -> listen for the next turn
                // (multi-turn: no re-wake needed within a session).
                if (g_turnComplete.load() && !g_audio.isPlaying()) {
                    g_state = State::Listening;
                    faceStatus(Lang::Strings::LISTENING);
                }
                break;

            case State::Sleeping:
                break;  // guarded out above
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                                   start()                                   */
/* -------------------------------------------------------------------------- */

void start()
{
    mclog::tagInfo(_tag, "start custom agent");

    // Release BLE's reserved RAM. Agent mode never uses BLE (only the Dance app calls
    // startBleServer; NimBLE is never initialized on this path) and agent mode is
    // terminal (Mooncake is destroyed before we get here -- no path back to Dance
    // without a reboot), so this is safe. NOTE: this only reclaims ~2KB in practice
    // (the controller was never init'd, so there is little dynamic RAM to free) -- it is
    // NOT the fix for the session-start internal-RAM collapse. The real drain is the
    // ~160KB consumed by the init steps below (WiFi / microlink / AFE); the log_heap()
    // probes there pinpoint it. Kept anyway as a cheap, safe reclaim.
    {
        const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const esp_err_t err = esp_bt_mem_release(ESP_BT_MODE_BLE);
        const size_t after  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        mclog::tagWarn(_tag, "BT mem release: err={} int_free {} -> {} (+{} bytes)", (int)err,
                       (unsigned long)before, (unsigned long)after, (unsigned long)(after - before));
    }

    g_cfg = load_config();

    // Silence microlink's per-packet INFO logging. At INFO these tags emit ~3 lines
    // per received WG packet (UDP RX / WG RX / ACK TX); over the slow console UART
    // that blocks the network tasks long enough to throttle tunnel throughput below
    // the audio playback rate -> dropouts. WARN keeps real problems visible. (The raw
    // "[WG_RX]" printf in wireguard_lwip is silenced separately, in the patch.)
    esp_log_level_set("ml_net_io", ESP_LOG_WARN);
    esp_log_level_set("ml_wg_mgr", ESP_LOG_WARN);
    esp_log_level_set("ml_derp", ESP_LOG_WARN);

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

    // 2a) Tap-to-start. The avatar panel created by SetupUI covers the whole screen
    //     and has its own onClick (a no-op in agent mode -- is_xiaozhi_ready() is
    //     false), and LVGL clicks don't bubble to the screen, so a screen-level
    //     handler would never see a tap on the avatar. Subscribe to the panel's
    //     onClick instead (uitk::Signal allows multiple subscribers). Honored only
    //     while sleeping; in-session taps are ignored by the loop / start_session.
    if (GetStackChan().hasAvatar()) {
        auto* panel = static_cast<stackchan::avatar::DefaultAvatar&>(GetStackChan().avatar()).getPanel();
        if (panel != nullptr) {
            panel->onClick().connect([]() {
                if (!g_sessionActive.load()) {
                    g_startRequested = true;
                }
            });
        }
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

    log_heap("pre-net");

    // 5) Network (blocks until Wi-Fi is up; enters config mode if unprovisioned).
    faceSpeech("system", "ネットワーク接続中...");
    GetHAL().startNetwork([](std::string_view msg) { mclog::tagInfo(_tag, "net: {}", msg); });
    log_heap("post-wifi");

    // 5b) Tailscale (MicroLink) tunnel, if provisioned. No-op AND silent otherwise
    //     (the status sink fires only on the enabled path). Makes a tailnet-only
    //     backend reachable; the WebSocket transport is unchanged because lwIP
    //     routes 100.64.0.0/10 through the WireGuard netif.
    tailscale_bring_up(g_cfg, [](const char* msg) { faceSpeech("system", msg); });
    log_heap("post-tailscale");

    // 6) Audio I/O.
    if (!g_audio.init()) {
        faceSpeech("system", "オーディオ初期化失敗");
        mclog::tagError(_tag, "audio init failed");
    }
    log_heap("post-audio");

    // 6b) Speech engines. Load the ESP-SR models from the assets partition and bring up
    //      the voice-processing AFE (NS + VAD + mono extraction) used during a session.
    //      The wake-word AFE is created only when kWakeWordEnabled (currently off -- it
    //      is the ~25KB internal-RAM hog behind the session-start memory collapse). With
    //      it off, sessions start by tap / head-pet, the loop's g_wake guards no-op, and
    //      the freed RAM keeps WiFi able to allocate TX buffers during audio streaming.
    g_models = load_sr_models();
    log_heap("post-srmodels");
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (g_models != nullptr && codec != nullptr) {
        // Voice processor: it only borrows g_models (reads NS/VAD model names) and never
        // frees it.
        g_vc = std::make_unique<AfeAudioProcessor>();
        g_vc->Initialize(codec, kVcFrameMs, g_models);  // half-duplex: device AEC stays off
        log_heap("post-vc-afe");

        if (kWakeWordEnabled) {
            // ~AfeWakeWord esp_srmodel_deinit()s the shared g_models list, so on init
            // failure we release() (leak the small zombie) instead of reset()ing, which
            // would tear g_models out from under the voice processor mid-run.
            g_wake = std::make_unique<AfeWakeWord>();
            if (!g_wake->Initialize(codec, g_models)) {
                mclog::tagError(_tag, "wake word init failed; tap/head-pet start only");
                g_wake.release();
            }
            log_heap("post-wake-afe");
        }
        wire_afe();
    } else {
        mclog::tagError(_tag, "SR models/codec unavailable; voice session disabled");
        faceSpeech("system", "音声モデル未検出");
    }

    // 6c) Head-pet (optional) shares the session-start entry: a press while sleeping
    //      opens a session, same as a tap or the wake word. In-session petting is left
    //      to the avatar's own head-pet behaviour.
    GetHAL().onHeadPetGesture.connect([](HeadPetGesture gesture) {
        if (gesture == HeadPetGesture::Press && !g_sessionActive.load()) {
            g_startRequested = true;
        }
    });

    // 7) Connect to the backend and wait for ready, then drop into the sleeping state so
    //    we are immediately ready for a session-start tap / head-pet (wake word too when
    //    kWakeWordEnabled).
    connect_with_retry();
    enter_sleeping();

    // 8) Run forever.
    session_loop();
}

}  // namespace custom_agent
