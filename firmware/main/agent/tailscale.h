/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "agent_config.h"
#include <functional>

namespace custom_agent {

/**
 * @brief Bring up the Tailscale tunnel (via MicroLink) if it has been
 *        provisioned, so a tailnet-only backend (e.g. 100.x.y.z) is reachable
 *        over WireGuard.
 *
 * Provisioned through NVS namespace "tscale":
 *   - en       (bool)   enable Tailscale (default false)
 *   - authkey  (string) Tailscale auth key ("tskey-auth-...")
 *   - devname  (string) tailnet hostname (empty -> MAC-derived default)
 *   - peer     (string) backend VPN IP to prime/prioritize (empty -> derived
 *                       from the host part of `cfg.host`)
 * Compile-time fallbacks: TAILSCALE_DEFAULT_{ENABLED,AUTHKEY,DEVNAME}.
 *
 * Behaviour:
 *   - When disabled / unprovisioned this is a no-op and the backend is reached
 *     directly over the LAN (existing behaviour, unchanged).
 *   - When enabled it starts MicroLink, blocks until the tunnel is up (or a
 *     timeout), and primes the WireGuard session to the backend peer so the
 *     subsequent plain WebSocket connect() — a standard lwIP socket — routes
 *     through the tunnel immediately. lwIP routes 100.64.0.0/10 to the WG netif
 *     automatically, so the WebSocket transport needs no changes.
 *
 * @param cfg  Agent config; its `host` ("ip:port") supplies the backend peer.
 * @param onStatus  Optional UI/status sink (e.g. wired to the face chat line).
 *         It is called ONLY on the enabled path, so the disabled/unprovisioned
 *         case stays completely silent (no spurious "Tailscale…" message for
 *         existing LAN users).
 * @return Always true (the agent proceeds to connect regardless; tunnel
 *         failures surface as WebSocket connect failures + retry).
 *
 * Must be called AFTER the Wi-Fi network is up (GetHAL().startNetwork()).
 */
bool tailscale_bring_up(const Config& cfg, const std::function<void(const char*)>& onStatus = {});

}  // namespace custom_agent
