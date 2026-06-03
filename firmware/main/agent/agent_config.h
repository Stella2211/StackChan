/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <string>
#include <cstdint>

namespace custom_agent {

/**
 * @brief Backend route. Matches the two endpoints in the backend WS spec.
 *        Agent = /ws/agent (Agent + VoiceVox, WAV output)
 *        Live  = /ws/live  (Gemini Live, raw PCM stream output)
 *
 * The send/receive message format is identical for both routes, so the only
 * difference here is the endpoint path and (typically) the audio mime type of
 * the response. Playback itself is driven by the mimeType in `output.audio.start`,
 * so switching routes is just a config change.
 */
enum class Route : uint8_t {
    Agent = 0,
    Live  = 1,
};

/**
 * @brief Runtime configuration for the custom AI agent engine.
 *        Persisted in NVS (namespace "cagent"). Compile-time defaults below can
 *        be overridden via the AGENT_DEFAULT_* macros (see agent_config.cpp).
 */
struct Config {
    // Connection
    std::string host;          // "host:port", e.g. "192.168.1.10:8787"
    bool tls = false;          // false -> ws://, true -> wss://
    std::string token;         // auth token (empty -> no token query appended)
    Route route = Route::Agent;

    // On-device VAD (no server-side VAD per the protocol spec)
    int vadStartRms       = 600;    // mean-abs amplitude to consider as speech onset
    int vadKeepRms        = 350;    // below this for vadSilenceMs ends the utterance
    int vadSilenceMs      = 800;    // trailing silence that closes an utterance
    int vadMinSpeechMs    = 300;    // utterances shorter than this are discarded
    int vadMaxUtteranceMs = 15000;  // hard cap on a single utterance
};

/**
 * @brief Load config from NVS, falling back to compile-time defaults.
 */
Config load_config();

/**
 * @brief Persist config to NVS.
 */
void save_config(const Config& cfg);

/**
 * @brief Build the full WebSocket URL, e.g. "ws://host:port/ws/agent?token=XXX".
 */
std::string build_ws_url(const Config& cfg);

/**
 * @brief Route -> endpoint name ("agent" / "live").
 */
const char* route_name(Route route);

}  // namespace custom_agent
