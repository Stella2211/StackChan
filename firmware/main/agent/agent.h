/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

namespace custom_agent {

/**
 * @brief Entry point for the custom AI agent engine. Brings up the network,
 *        audio I/O, the StackChan avatar/face, and the backend WebSocket, then
 *        runs the capture/response loop forever. Never returns.
 *
 * This is the drop-in replacement for GetHAL().startXiaozhi(): it reuses the
 * StackChan face / motion / head-pet subsystem unchanged and only swaps the AI
 * conversation engine for our own backend (see backend/docs/websocket-protocol.md).
 */
void start();

}  // namespace custom_agent
