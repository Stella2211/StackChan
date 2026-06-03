/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "agent.h"
#include "agent_config.h"
#include "backend_client.h"
#include "audio_pipeline.h"

#include <hal/hal.h>
#include <hal/board/stackchan_display.h>
#include <stackchan/stackchan.h>
#include <board.h>
#include <assets/lang_config.h>
#include <apps/common/common.h>

#include <mooncake_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <deque>
#include <memory>
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
// finding the "data" sub-chunk. Returns 0 if it doesn't look like a WAV.
static size_t wav_data_offset(const uint8_t* data, size_t len)
{
    if (len < 12 || std::memcmp(data, "RIFF", 4) != 0) {
        return 0;
    }
    for (size_t i = 12; i + 8 <= len; ++i) {
        if (std::memcmp(data + i, "data", 4) == 0) {
            return i + 8;  // skip "data" + 4-byte size
        }
    }
    return 44;  // canonical WAV header fallback
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
        g_state = State::Idle;
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
        g_client = std::make_unique<BackendClient>(g_cfg);
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
        g_client.reset();
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
    constexpr int kFrameMs       = AudioPipeline::kFrameMs;  // 20
    constexpr int kOnsetDebounce = 80;                       // ms over threshold to commit onset
    const int prerollFrames      = 300 / kFrameMs;           // ~300ms pre-roll

    std::deque<std::vector<int16_t>> preroll;
    std::vector<int16_t> frame;
    int onsetMs   = 0;
    int speechMs  = 0;
    int silenceMs = 0;

    while (1) {
        // Reading paces this loop at ~one frame per kFrameMs (codec is real-time).
        if (!g_audio.captureFrame16k(frame)) {
            GetHAL().delay(kFrameMs);
            continue;
        }

        if (g_client == nullptr || !g_client->isConnected()) {
            if (!connect_with_retry()) {
                continue;
            }
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
                while (static_cast<int>(preroll.size()) > prerollFrames) {
                    preroll.pop_front();
                }

                if (energy >= g_cfg.vadStartRms) {
                    onsetMs += kFrameMs;
                } else {
                    onsetMs = 0;
                }

                if (onsetMs >= kOnsetDebounce) {
                    // Speech onset confirmed: open the utterance and flush pre-roll.
                    if (!g_client->sendInputAudioStart()) {
                        onsetMs = 0;
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
                    speechMs  = flushed * kFrameMs;
                    silenceMs = 0;
                    onsetMs   = 0;
                    g_state   = State::Capturing;
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
                    onsetMs = 0;
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

    // 3) Per-frame avatar update task (blink/breath/head-pet/speaking/idle).
    xTaskCreatePinnedToCore(update_task, "agent_face", 4096, nullptr, 3, nullptr, 1);

    // 4) Idle face while we connect.
    faceStatus(Lang::Strings::STANDBY);

    // 5) Network (blocks until Wi-Fi is up; enters config mode if unprovisioned).
    faceSpeech("system", "ネットワーク接続中...");
    GetHAL().startNetwork([](std::string_view msg) { mclog::tagInfo(_tag, "net: {}", msg); });

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
