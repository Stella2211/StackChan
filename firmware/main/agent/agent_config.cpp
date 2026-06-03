/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "agent_config.h"
#include <settings.h>
#include <mooncake_log.h>

// Compile-time defaults. Override in build flags if you want to bake in a
// backend without provisioning over NVS, e.g.:
//   idf.py build -DAGENT_DEFAULT_HOST=\"192.168.1.10:8787\"
#ifndef AGENT_DEFAULT_HOST
#define AGENT_DEFAULT_HOST ""
#endif
#ifndef AGENT_DEFAULT_TLS
#define AGENT_DEFAULT_TLS false
#endif
#ifndef AGENT_DEFAULT_TOKEN
#define AGENT_DEFAULT_TOKEN ""
#endif
#ifndef AGENT_DEFAULT_ROUTE
#define AGENT_DEFAULT_ROUTE custom_agent::Route::Agent
#endif

namespace custom_agent {

static constexpr const char* _tag    = "agent-config";
static constexpr const char* _nvs_ns = "cagent";

const char* route_name(Route route)
{
    switch (route) {
        case Route::Live:
            return "live";
        case Route::Agent:
        default:
            return "agent";
    }
}

Config load_config()
{
    Config cfg;
    cfg.host  = AGENT_DEFAULT_HOST;
    cfg.tls   = AGENT_DEFAULT_TLS;
    cfg.token = AGENT_DEFAULT_TOKEN;
    cfg.route = AGENT_DEFAULT_ROUTE;

    Settings settings(_nvs_ns, false);
    cfg.host  = settings.GetString("host", cfg.host);
    cfg.tls   = settings.GetBool("tls", cfg.tls);
    cfg.token = settings.GetString("token", cfg.token);
    cfg.route = static_cast<Route>(settings.GetInt("route", static_cast<int>(cfg.route)));

    cfg.vadStartRms       = settings.GetInt("vad_start", cfg.vadStartRms);
    cfg.vadKeepRms        = settings.GetInt("vad_keep", cfg.vadKeepRms);
    cfg.vadSilenceMs      = settings.GetInt("vad_sil", cfg.vadSilenceMs);
    cfg.vadMinSpeechMs    = settings.GetInt("vad_min", cfg.vadMinSpeechMs);
    cfg.vadMaxUtteranceMs = settings.GetInt("vad_max", cfg.vadMaxUtteranceMs);

    mclog::tagInfo(_tag, "loaded: host='{}' tls={} route={} token={}", cfg.host, cfg.tls, route_name(cfg.route),
                   cfg.token.empty() ? "(none)" : "(set)");
    // A host without an explicit port makes the WebSocket fall back to the scheme
    // default (ws=80 / wss=443), which won't match the backend (default 8787) and
    // is easy to overlook. Warn so a missing ":port" doesn't look like a silent
    // connect failure.
    if (!cfg.host.empty() && cfg.host.find(':') == std::string::npos) {
        mclog::tagWarn(_tag, "host '{}' has no port; WS will use {} (set 'ip:port', e.g. ...:8787)", cfg.host,
                       cfg.tls ? "443" : "80");
    }
    return cfg;
}

void save_config(const Config& cfg)
{
    Settings settings(_nvs_ns, true);
    settings.SetString("host", cfg.host);
    settings.SetBool("tls", cfg.tls);
    settings.SetString("token", cfg.token);
    settings.SetInt("route", static_cast<int>(cfg.route));
    settings.SetInt("vad_start", cfg.vadStartRms);
    settings.SetInt("vad_keep", cfg.vadKeepRms);
    settings.SetInt("vad_sil", cfg.vadSilenceMs);
    settings.SetInt("vad_min", cfg.vadMinSpeechMs);
    settings.SetInt("vad_max", cfg.vadMaxUtteranceMs);
    mclog::tagInfo(_tag, "saved config");
}

std::string build_ws_url(const Config& cfg)
{
    std::string url = cfg.tls ? "wss://" : "ws://";
    url += cfg.host;
    url += "/ws/";
    url += route_name(cfg.route);
    if (!cfg.token.empty()) {
        url += "?token=";
        url += cfg.token;
    }
    return url;
}

}  // namespace custom_agent
