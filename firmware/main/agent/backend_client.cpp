/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "backend_client.h"
#include <web_socket.h>
#include <network_interface.h>
#include <board.h>
#include <ArduinoJson.hpp>
#include <mooncake_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>

namespace custom_agent {

static constexpr const char* _tag = "agent-ws";

// The ml307 TCP layer (managed component) spawns its socket receive task at FreeRTOS
// priority 1 -- yet that task is what pulls decrypted bytes off the socket and hands
// the audio chunks to the playback queue. At prio 1 it loses both cores to the
// WireGuard manager (prio 7) and the LVGL face/text render (prio 3), starving the
// audio feed and causing playback dropouts during heavy RX / scrolling. We can't
// change the component's hard-coded priority, so after connecting we look the task up
// by name and lift it above LVGL but below the WG/net tasks (so WG still fills the
// socket first, then this drains it promptly). Best-effort: a silent no-op if the
// task name ever changes upstream.
static void raise_rx_task_priority()
{
#if (INCLUDE_xTaskGetHandle == 1)
    TaskHandle_t rx = xTaskGetHandle("tcp_receive");
    if (rx != nullptr) {
        vTaskPrioritySet(rx, 6);
        mclog::tagInfo(_tag, "raised tcp_receive priority -> 6 (audio feed)");
    }
#endif
}

static AudioMime parse_mime(const char* mime)
{
    if (mime == nullptr) {
        return AudioMime::Unknown;
    }
    // "audio/wav" -> Wav, "audio/pcm;rate=24000" -> Pcm, "audio/opus" -> Opus
    if (std::strstr(mime, "opus") != nullptr) {
        return AudioMime::Opus;
    }
    if (std::strstr(mime, "wav") != nullptr) {
        return AudioMime::Wav;
    }
    if (std::strstr(mime, "pcm") != nullptr) {
        return AudioMime::Pcm;
    }
    return AudioMime::Unknown;
}

BackendClient::BackendClient(Config cfg) : cfg_(std::move(cfg))
{
}

BackendClient::~BackendClient()
{
    close();
}

bool BackendClient::isConnected() const
{
    return connected_.load() && ws_ != nullptr && ws_->IsConnected();
}

void BackendClient::wireHandlers()
{
    ws_->OnConnected([this]() {
        if (!alive_.load()) {
            return;
        }
        mclog::tagInfo(_tag, "ws connected");
        connected_ = true;
        if (cb_.onConnected) {
            cb_.onConnected();
        }
    });

    ws_->OnDisconnected([this]() {
        if (!alive_.load()) {
            return;
        }
        mclog::tagWarn(_tag, "ws disconnected");
        connected_ = false;
        ready_     = false;
        if (cb_.onDisconnected) {
            cb_.onDisconnected();
        }
    });

    ws_->OnError([this](int err) {
        if (!alive_.load()) {
            return;
        }
        mclog::tagError(_tag, "ws error: {}", err);
    });

    ws_->OnData([this](const char* data, size_t len, bool binary) {
        if (!alive_.load()) {
            return;
        }
        if (binary) {
            // Audio bytes of the currently open output.audio segment.
            if (cb_.onAudioChunk && len > 0) {
                cb_.onAudioChunk(reinterpret_cast<const uint8_t*>(data), len);
            }
        } else {
            handleText(data, len);
        }
    });
}

bool BackendClient::connect()
{
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        mclog::tagError(_tag, "no network interface");
        return false;
    }

    ws_ = network->CreateWebSocket(1);
    if (!ws_) {
        mclog::tagError(_tag, "failed to create websocket");
        return false;
    }

    // Auth can also be passed as a header; the URL already carries ?token= so this
    // is belt-and-suspenders for servers that prefer the header form.
    if (!cfg_.token.empty()) {
        ws_->SetHeader("Authorization", (std::string("Bearer ") + cfg_.token).c_str());
    }

    wireHandlers();

    std::string url = build_ws_url(cfg_);
    mclog::tagInfo(_tag, "connecting to {}", url);
    if (!ws_->Connect(url.c_str())) {
        mclog::tagError(_tag, "connect failed (err {})", ws_->GetLastError());
        ws_.reset();
        return false;
    }
    // Lift the just-spawned socket receive task out of its prio-1 starvation so it can
    // keep the audio playback queue fed under WG/LVGL load (see helper comment).
    raise_rx_task_priority();
    connected_ = true;
    return true;
}

void BackendClient::close()
{
    ready_ = false;
    // Stop dispatching into cb_ before tearing down ws_. ws_->Close() then joins the
    // receive task, so by the time we reset() no handler is in flight. (Residual
    // safety relies on Close() joining; this flag shrinks the window further.)
    alive_ = false;
    if (ws_) {
        ws_->Close();
        ws_.reset();
    }
    connected_ = false;
}

void BackendClient::handleText(const char* data, size_t len)
{
    ArduinoJson::JsonDocument doc;
    auto err = ArduinoJson::deserializeJson(doc, data, len);
    if (err) {
        mclog::tagWarn(_tag, "bad json frame: {}", err.c_str());
        return;
    }

    const char* type = doc["type"].as<const char*>();
    if (type == nullptr) {
        return;
    }

    if (std::strcmp(type, "ready") == 0) {
        ready_ = true;
        mclog::tagInfo(_tag, "server ready (route {})", route_name(cfg_.route));
        if (cb_.onReady) {
            cb_.onReady();
        }
    } else if (std::strcmp(type, "output.audio.start") == 0) {
        const char* id   = doc["id"].as<const char*>();
        const char* mime = doc["format"]["mimeType"].as<const char*>();
        current_mime_    = parse_mime(mime);
        if (cb_.onAudioStart) {
            cb_.onAudioStart(id ? id : "", current_mime_);
        }
    } else if (std::strcmp(type, "output.audio.end") == 0) {
        const char* id = doc["id"].as<const char*>();
        if (cb_.onAudioEnd) {
            cb_.onAudioEnd(id ? id : "");
        }
    } else if (std::strcmp(type, "output.text") == 0) {
        const char* text = doc["text"].as<const char*>();
        if (cb_.onText && text) {
            cb_.onText(text);
        }
    } else if (std::strcmp(type, "turn.complete") == 0) {
        if (cb_.onTurnComplete) {
            cb_.onTurnComplete();
        }
    } else if (std::strcmp(type, "tool.call") == 0) {
        const char* name    = doc["name"].as<const char*>();
        const char* status  = doc["status"].as<const char*>();
        const char* summary = doc["summary"].as<const char*>();
        if (cb_.onToolCall) {
            cb_.onToolCall(name ? name : "", status ? status : "", summary ? summary : "");
        }
    } else if (std::strcmp(type, "action.start") == 0) {
        // Built-in device tool execution request. Hand id/name/args to the agent,
        // which queues it to a worker task (camera/JPEG must not run on this task).
        const char* id   = doc["id"].as<const char*>();
        const char* name = doc["name"].as<const char*>();
        std::string args;
        if (doc["args"].isNull()) {
            args = "{}";
        } else {
            ArduinoJson::serializeJson(doc["args"], args);
        }
        if (cb_.onAction) {
            cb_.onAction(id ? id : "", name ? name : "", args);
        }
    } else if (std::strcmp(type, "session.started") == 0) {
        const char* sid = doc["sessionId"].as<const char*>();
        if (cb_.onSessionStarted) {
            cb_.onSessionStarted(sid ? sid : "");
        }
    } else if (std::strcmp(type, "session.closed") == 0) {
        const char* sid    = doc["sessionId"].as<const char*>();
        const char* reason = doc["reason"].as<const char*>();
        if (cb_.onSessionClosed) {
            cb_.onSessionClosed(sid ? sid : "", reason ? reason : "");
        }
    } else if (std::strcmp(type, "error") == 0) {
        const char* msg = doc["message"].as<const char*>();
        bool fatal      = doc["fatal"].as<bool>();
        mclog::tagError(_tag, "server error: {} (fatal={})", msg ? msg : "", fatal);
        if (cb_.onError) {
            cb_.onError(msg ? msg : "", fatal);
        }
    } else {
        mclog::tagInfo(_tag, "ignoring message type '{}'", type);
    }
}

bool BackendClient::sendSessionStart(const std::string& sessionId)
{
    if (!isConnected()) {
        return false;
    }
    ArduinoJson::JsonDocument doc;
    doc["type"] = "session.start";
    if (!sessionId.empty()) {
        doc["sessionId"] = sessionId;  // omit -> server assigns one
    }
    std::string out;
    ArduinoJson::serializeJson(doc, out);
    return ws_->Send(out);
}

bool BackendClient::sendSessionEnd()
{
    if (!isConnected()) {
        return false;
    }
    static const std::string msg = R"({"type":"session.end"})";
    return ws_->Send(msg);
}

bool BackendClient::sendInputAudioStart()
{
    if (!isConnected()) {
        return false;
    }
    static const std::string msg =
        R"({"type":"input.audio.start","format":{"codec":"pcm_s16le","sampleRate":16000,"channels":1}})";
    return ws_->Send(msg);
}

bool BackendClient::sendAudioChunk(const int16_t* pcm16k, size_t samples)
{
    if (!isConnected() || pcm16k == nullptr || samples == 0) {
        return false;
    }
    return ws_->Send(reinterpret_cast<const void*>(pcm16k), samples * sizeof(int16_t), /*binary=*/true);
}

bool BackendClient::sendInputAudioEnd()
{
    if (!isConnected()) {
        return false;
    }
    static const std::string msg = R"({"type":"input.audio.end"})";
    return ws_->Send(msg);
}

/* ------------------------- device tool results / images ------------------------- */

bool BackendClient::sendActionResult(const std::string& id, bool ok, const std::string& summary)
{
    if (!isConnected()) {
        return false;
    }
    ArduinoJson::JsonDocument doc;
    doc["type"] = "action.result";
    doc["id"]   = id;
    doc["ok"]   = ok;
    if (!summary.empty()) {
        doc["summary"] = summary;
    }
    std::string out;
    ArduinoJson::serializeJson(doc, out);
    return ws_->Send(out);
}

bool BackendClient::sendImageStart(const std::string& id, const std::string& actionId, const std::string& format)
{
    if (!isConnected()) {
        return false;
    }
    ArduinoJson::JsonDocument doc;
    doc["type"]     = "input.image.start";
    doc["id"]       = id;
    doc["actionId"] = actionId;
    doc["format"]   = format;
    std::string out;
    ArduinoJson::serializeJson(doc, out);
    return ws_->Send(out);
}

bool BackendClient::sendImageChunk(const uint8_t* data, size_t len)
{
    if (!isConnected() || data == nullptr || len == 0) {
        return false;
    }
    return ws_->Send(reinterpret_cast<const void*>(data), len, /*binary=*/true);
}

bool BackendClient::sendImageEnd(const std::string& id)
{
    if (!isConnected()) {
        return false;
    }
    ArduinoJson::JsonDocument doc;
    doc["type"] = "input.image.end";
    doc["id"]   = id;
    std::string out;
    ArduinoJson::serializeJson(doc, out);
    return ws_->Send(out);
}

}  // namespace custom_agent
