/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "tailscale.h"

#include <hal/hal.h>
#include <settings.h>
#include <mooncake_log.h>

#include <microlink.h>

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>

// Compile-time defaults. Override in build flags to bake provisioning in without
// NVS, e.g.:
//   idf.py build -DTAILSCALE_DEFAULT_ENABLED=true -DTAILSCALE_DEFAULT_AUTHKEY='"tskey-..."'
#ifndef TAILSCALE_DEFAULT_ENABLED
#define TAILSCALE_DEFAULT_ENABLED false
#endif
#ifndef TAILSCALE_DEFAULT_AUTHKEY
#define TAILSCALE_DEFAULT_AUTHKEY ""
#endif
#ifndef TAILSCALE_DEFAULT_DEVNAME
#define TAILSCALE_DEFAULT_DEVNAME ""
#endif

namespace custom_agent {

namespace {

constexpr const char* _tag    = "agent-ts";
constexpr const char* _nvs_ns = "tscale";

// Kept alive for the lifetime of the process: MicroLink owns the WG netif and
// keeps shallow pointers into the config strings, so neither the handle nor the
// backing config may be destroyed while the tunnel is in use.
microlink_t* g_ml = nullptr;

struct TsConfig {
    bool enabled = TAILSCALE_DEFAULT_ENABLED;
    std::string authKey    = TAILSCALE_DEFAULT_AUTHKEY;
    std::string deviceName = TAILSCALE_DEFAULT_DEVNAME;
    std::string peerIp;  // explicit backend VPN IP; empty -> derive from host
};

// Lives forever and MUST NOT be reassigned while g_ml is non-null: microlink_init()
// shallow-copies the config (it keeps our c_str() pointers, it does not deep-copy
// the strings), so the backing storage has to outlive the handle. tailscale_bring_up()
// enforces this with an early g_ml guard before it touches g_ts.
TsConfig g_ts;

TsConfig load_ts_config()
{
    TsConfig c;
    Settings settings(_nvs_ns, false);
    c.enabled    = settings.GetBool("en", c.enabled);
    c.authKey    = settings.GetString("authkey", c.authKey);
    c.deviceName = settings.GetString("devname", c.deviceName);
    c.peerIp     = settings.GetString("peer", c.peerIp);
    return c;
}

// Split "ip:port" into its parts. IPv4 host:port only — the backend is an IPv4
// tailnet address and peer priming runs through microlink_parse_ip() (IPv4-only),
// so an IPv6 literal (which would mis-split on rfind(':')) is out of scope.
// Defaults the port to 8787 (the backend's default) when absent or unparseable.
// Returns false only if there's no host.
bool split_host_port(const std::string& host, std::string& ip, uint16_t& port)
{
    port = 8787;
    const auto pos = host.rfind(':');
    if (pos == std::string::npos) {
        ip = host;
    } else {
        ip                 = host.substr(0, pos);
        const unsigned long p = std::strtoul(host.c_str() + pos + 1, nullptr, 10);
        if (p > 0 && p < 65536) {
            port = static_cast<uint16_t>(p);
        }
    }
    return !ip.empty();
}

void state_cb(microlink_t* /*ml*/, microlink_state_t state, void* /*user*/)
{
    static const char* names[] = {"IDLE",         "WIFI_WAIT", "CONNECTING", "REGISTERING",
                                  "CONNECTED",    "RECONNECTING", "ERROR"};
    const unsigned u = static_cast<unsigned>(state);
    mclog::tagInfo(_tag, "microlink state: {}", u < (sizeof(names) / sizeof(names[0])) ? names[u] : "?");
}

}  // namespace

bool tailscale_bring_up(const Config& cfg, const std::function<void(const char*)>& onStatus)
{
    auto status = [&](const char* m) {
        if (onStatus) {
            onStatus(m);
        }
    };

    // Re-entry guard FIRST: while g_ml is live, microlink holds shallow pointers
    // into g_ts's strings, so g_ts must not be reassigned. Bail before touching it.
    if (g_ml != nullptr) {
        mclog::tagInfo(_tag, "tailscale already up");
        return true;
    }

    g_ts = load_ts_config();

    if (!g_ts.enabled || g_ts.authKey.empty()) {
        // Stay silent here: existing LAN users (Tailscale off) must see no change.
        mclog::tagInfo(_tag, "tailscale disabled/unprovisioned -> direct connection");
        return true;
    }

    status("Tailscale 接続中...");

    // Resolve the backend peer (used both as the priority WG slot and as the
    // prime target). Prefer an explicit "peer" override, else the host part of
    // the agent's backend URL.
    std::string hostIp;
    uint16_t peerPort = 8787;
    if (split_host_port(cfg.host, hostIp, peerPort) && g_ts.peerIp.empty()) {
        g_ts.peerIp = hostIp;
    }
    const uint32_t peerIpHost = g_ts.peerIp.empty() ? 0 : microlink_parse_ip(g_ts.peerIp.c_str());

    if (g_ts.deviceName.empty()) {
        g_ts.deviceName = microlink_default_device_name();
    }

    microlink_config_t mlc = {};
    mlc.auth_key          = g_ts.authKey.c_str();
    mlc.device_name       = g_ts.deviceName.c_str();
    mlc.enable_derp       = true;
    mlc.enable_stun       = true;
    mlc.enable_disco      = true;
    // We only ever talk to the backend (the priority peer). disco_priority_only makes
    // microlink admit/probe ONLY that peer and ignore the rest of the tailnet, so a
    // multi-device tailnet can't trigger a DISCO/handshake storm (+ peer-slot eviction
    // thrashing) that drains internal RAM and preempts the audio RX path -> choppy TTS.
    // max_peers stays at the full tailnet size on purpose: peers[] lives in PSRAM (no
    // SRAM cost) and a small table risks the backend landing on a reused WG slot with
    // stale handshake state -> backend handshake fails on boot (see sdkconfig.defaults).
    mlc.max_peers          = 16;
    mlc.disco_priority_only = (peerIpHost != 0);
    mlc.wifi_tx_power_dbm  = 0;  // keep the board's default TX power
    mlc.priority_peer_ip   = peerIpHost;

    mclog::tagInfo(_tag, "starting tailscale (device='{}' peer='{}')", g_ts.deviceName,
                   g_ts.peerIp.empty() ? "(none)" : g_ts.peerIp);

    g_ml = microlink_init(&mlc);
    if (g_ml == nullptr) {
        mclog::tagError(_tag, "microlink_init failed -> falling back to direct connect");
        status("Tailscale 起動失敗");
        return true;
    }
    microlink_set_state_callback(g_ml, state_cb, nullptr);

    if (microlink_start(g_ml) != ESP_OK) {
        mclog::tagError(_tag, "microlink_start failed -> falling back to direct connect");
        // Don't leave a half-initialized handle behind: the re-entry guard keys off
        // g_ml != nullptr, so a stale handle would later report "already up".
        microlink_destroy(g_ml);
        g_ml = nullptr;
        status("Tailscale 起動失敗");
        return true;
    }

    // Wait for the tunnel: control-plane registration + first MapResponse.
    constexpr int kTimeoutMs = 90000;
    int waited               = 0;
    while (!microlink_is_connected(g_ml) && waited < kTimeoutMs) {
        GetHAL().delay(500);
        waited += 500;
    }
    if (!microlink_is_connected(g_ml)) {
        mclog::tagWarn(_tag, "tailscale not connected after {}ms; proceeding anyway", kTimeoutMs);
        status("Tailscale 未接続のまま続行");
        return true;
    }

    char ipstr[16] = {0};
    microlink_ip_to_str(microlink_get_vpn_ip(g_ml), ipstr);
    mclog::tagInfo(_tag, "tailscale up, vpn ip {}", ipstr);
    status("Tailscale 接続完了");

    // Prime the WireGuard session to the backend peer. WireGuard is lazy: a cold
    // connect() to a peer fails with EHOSTUNREACH until the handshake completes.
    // microlink_tcp_connect() triggers the handshake + DISCO and waits, so after
    // this throwaway probe the plain WebSocket connect() routes straight through.
    if (peerIpHost != 0) {
        mclog::tagInfo(_tag, "priming WG tunnel to {}:{}", g_ts.peerIp, peerPort);
        microlink_tcp_socket_t* probe = microlink_tcp_connect(g_ml, peerIpHost, peerPort, 15000);
        if (probe != nullptr) {
            microlink_tcp_close(probe);
            mclog::tagInfo(_tag, "tunnel primed");
        } else {
            mclog::tagWarn(_tag, "tunnel prime failed; relying on WebSocket retry");
        }
    }

    return true;
}

}  // namespace custom_agent
