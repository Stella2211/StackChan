/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace custom_agent {

/**
 * @brief Outbound channel for a device tool ("action.start") to report back to the
 *        server. Decouples the tool logic (motion / avatar / camera) from the
 *        WebSocket client lifetime: the agent provides lambdas that send through
 *        whatever BackendClient is currently connected (guarded against reconnect),
 *        so tools never hold a raw client reference across a slow camera capture.
 *
 * Mirrors the server-side DeviceBridge surface (action.result + input.image.*).
 */
struct ActionSink {
    // action.result { id, ok, summary? } -- optional confirmation / log line.
    std::function<void(const std::string& id, bool ok, const std::string& summary)> result;
    // input.image.start { id, actionId, format } -- begins a camera image stream.
    std::function<void(const std::string& id, const std::string& actionId, const std::string& format)> imageStart;
    // one JPEG chunk (binary frame).
    std::function<void(const uint8_t* data, size_t len)> imageChunk;
    // input.image.end { id } -- ends the image stream.
    std::function<void(const std::string& id)> imageEnd;
};

/**
 * @brief Run one built-in device tool to completion. Call from a dedicated worker
 *        task, never from the WS receive task: capture_image performs a camera grab
 *        and a software JPEG encode (~hundreds of ms) and the others sleep between
 *        servo keyframes, all of which would otherwise stall the socket drain.
 *
 * Supported `name`s and `args`:
 *   - "move_head"      { pan:-90..90 deg, tilt:-30..30 deg, speed?:0..1 }
 *   - "play_gesture"   { gesture: "nod"|"shake"|"look_around"|"tilt" }
 *   - "set_expression" { expression: "neutral"|"happy"|"sad"|"angry"|"sleepy"|"doubt" }
 *   - "capture_image"  {} -> streams a JPEG back via ActionSink::image*
 *
 * Unknown tools reply with action.result{ ok:false }.
 */
void execute_action(const ActionSink& sink, const std::string& id, const std::string& name,
                    const std::string& argsJson);

}  // namespace custom_agent
