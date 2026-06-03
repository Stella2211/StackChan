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
#include <cstring>

namespace custom_agent {

static constexpr const char* _tag = "agent-ws";

static AudioMime parse_mime(const char* mime)
{
    if (mime == nullptr) {
        return AudioMime::Unknown;
    }
    // "audio/wav" -> Wav, "audio/pcm;rate=24000" -> Pcm
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

}  // namespace custom_agent
