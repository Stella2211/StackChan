/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <mooncake_log.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <ota.h>
#include <settings.h>
#include <agent/agent_config.h>
#include <agent/tailscale.h>

static const std::string_view _tag = "HAL-OTA";

#ifdef CONFIG_STACKCHAN_OTA_URL
// Extract "host" or "host:port" from a URL like "http://host:port/path", so the
// Tailscale bring-up can prime the WireGuard session to the distribution host.
static std::string ota_host_from_url(const std::string& url)
{
    std::string s     = url;
    const auto scheme = s.find("://");
    if (scheme != std::string::npos) {
        s.erase(0, scheme + 3);
    }
    const auto slash = s.find('/');
    if (slash != std::string::npos) {
        s.erase(slash);
    }
    return s;
}

// Reconcile the build-time self-hosted endpoint with the "wifi"/"ota_url" setting
// that the upstream Ota::GetCheckVersionUrl() reads (NVS takes priority over
// CONFIG_OTA_URL). Returns the host[:port] to route over Tailscale, or "" when no
// self-hosted endpoint is active.
//
// The marker namespace records the URL WE pushed so that flashing a build with an
// empty CONFIG_STACKCHAN_OTA_URL cleanly restores the CONFIG_OTA_URL fallback --
// by undoing only our own override, never a value provisioned by hand. Writes are
// guarded so a steady state (and the default empty build) commits nothing.
static std::string apply_self_hosted_ota_endpoint()
{
    const char* baked = CONFIG_STACKCHAN_OTA_URL;
    Settings wifi("wifi", true);
    Settings marker("scota", true);  // remembers the URL this option last applied
    const std::string last    = marker.GetString("last");
    const std::string current = wifi.GetString("ota_url");

    if (std::strlen(baked) > 0) {
        if (current != baked) {
            wifi.SetString("ota_url", baked);
            mclog::tagInfo(_tag, "OTA endpoint -> self-hosted: {}", baked);
        }
        if (last != baked) {
            marker.SetString("last", baked);
        }
        return ota_host_from_url(baked);
    }

    // No baked endpoint: restore the CONFIG_OTA_URL fallback, but only by undoing
    // our own previous override -- leave a hand-provisioned ota_url untouched.
    if (!last.empty()) {
        if (current == last) {
            wifi.SetString("ota_url", "");
            mclog::tagInfo(_tag, "self-hosted OTA endpoint cleared; using CONFIG_OTA_URL");
        }
        marker.SetString("last", "");
    }
    return "";
}
#endif

bool Hal::updateFirmware(std::function<void(std::string_view)> onLog)
{
#ifdef CONFIG_STACKCHAN_OTA_URL
    // Self-hosted firmware distribution. Reconcile the build-time endpoint with the
    // "wifi"/"ota_url" setting the upstream Ota reads (so the version check hits our
    // server instead of CONFIG_OTA_URL), then bring up Tailscale so a tailnet-hosted
    // endpoint is reachable. With an empty CONFIG_STACKCHAN_OTA_URL this is a no-op
    // and the default Xiaozhi-cloud path is unchanged.
    const std::string ota_host = apply_self_hosted_ota_endpoint();
    if (!ota_host.empty()) {
        // No ":port" -> tailscale_bring_up can't prime the WireGuard session, so the
        // first cold connect to the tailnet host may fail (lazy handshake) and the
        // single-shot version check has no retry. Warn; the fix is to set host:port.
        if (ota_host.find(':') == std::string::npos) {
            mclog::tagWarn(_tag,
                           "OTA host '{}' has no :port; WireGuard priming skipped -- first "
                           "tailnet connect may fail. Configure host:port.",
                           ota_host);
        }

        // Self-gates on NVS "tscale" provisioning: a silent no-op when Tailscale is
        // off, so a public self-hosted endpoint still works over Wi-Fi. When on, it
        // primes the peer derived from the OTA host; lwIP then routes 100.64.0.0/10
        // over WireGuard so the OTA HTTP client (check + binary download) needs no
        // changes. Network is already up here (the sole caller, SystemUpdateWorker,
        // runs startNetwork() first).
        //
        // Note: if the check finds no update we return to the setup menu with the
        // tunnel still up -- MicroLink exposes no teardown, this matches the agent
        // path's lifetime, and the common success path reboots anyway.
        custom_agent::Config otaCfg;
        otaCfg.host = ota_host;
        custom_agent::tailscale_bring_up(otaCfg, [&](const char* m) { onLog(m); });
    }
#endif

    onLog("Checking firmware updates...");

    Ota ota;
    esp_err_t err = ota.CheckVersion();
    if (err != ESP_OK) {
        mclog::tagError(_tag, "failed to check firmware version: {}", esp_err_to_name(err));
        onLog("Failed to check firmware updates");
        return false;
    }

    if (!ota.HasNewVersion()) {
        ota.MarkCurrentVersionValid();
        mclog::tagInfo(_tag, "no new firmware version available");
        onLog("Already up to date");
        return true;
    }

    const std::string &firmware_url     = ota.GetFirmwareUrl();
    const std::string &firmware_version = ota.GetFirmwareVersion();
    if (firmware_url.empty()) {
        mclog::tagError(_tag, "firmware update available but url is empty");
        onLog("Invalid firmware update info");
        return false;
    }

    mclog::tagInfo(_tag, "new firmware available: version={}, url={}", firmware_version, firmware_url);
    if (!firmware_version.empty()) {
        onLog(std::string("New firmware found: ") + firmware_version);
    } else {
        onLog("New firmware found");
    }

    onLog("Starting firmware upgrade...");
    int last_reported_progress = -1;
    bool upgrade_success       = Ota::Upgrade(firmware_url, [&](int progress, size_t speed) {
        if (progress == last_reported_progress) {
            return;
        }

        last_reported_progress = progress;

        char msg[48];
        std::snprintf(msg, sizeof(msg), "Upgrading firmware: %d%% at %uKB/s", progress,
                            static_cast<unsigned>(speed / 1024));
        onLog(msg);
          });

    if (!upgrade_success) {
        mclog::tagError(_tag, "firmware upgrade failed: version={}, url={}", firmware_version, firmware_url);
        onLog("Firmware upgrade failed, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        reboot();
        return false;
    }

    mclog::tagInfo(_tag, "firmware upgrade successful, rebooting");
    onLog("Upgrade successful, rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    reboot();
    return true;
}
