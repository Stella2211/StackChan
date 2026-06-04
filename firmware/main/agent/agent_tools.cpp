/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "agent_tools.h"

#include <hal/hal.h>
#include <hal/board/stackchan_display.h>
#include <hal/board/hal_bridge.h>
#include <stackchan/stackchan.h>
#include <board.h>

#include <ArduinoJson.hpp>
#include <mooncake_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#ifndef CONFIG_IDF_TARGET_ESP32
#include "image_to_jpeg.h"  // jpg/ is on the include path (see main/CMakeLists.txt)
#endif

namespace custom_agent {

static constexpr const char* _tag = "agent-tools";

namespace {

// Servo angles are in units of 0.1 degree (the MCP head tool uses deg*10 too).
constexpr int kDegToUnits = 10;

// Documented tool ranges (degrees). The servo layer clamps to its own physical
// limits on top of this; clamping here keeps us inside the contract the LLM sees.
constexpr int kPanMaxDeg  = 90;
constexpr int kTiltMaxDeg = 30;

StackChanAvatarDisplay* face()
{
    return static_cast<StackChanAvatarDisplay*>(Board::GetInstance().GetDisplay());
}

// Map the optional normalized speed (0..1) to a servo speed (100..1000). A natural
// mid speed is used when the field is absent (matches the MCP head tool's ~150-300).
int servo_speed(ArduinoJson::JsonVariantConst v)
{
    if (v.isNull()) {
        return 300;
    }
    float s = v.as<float>();
    if (s <= 0.0f) {
        return 300;
    }
    s = std::min(1.0f, s);
    return static_cast<int>(std::lround(100.0f + s * 900.0f));
}

/* -------------------------------- move_head -------------------------------- */

void do_move_head(const ArduinoJson::JsonDocument& args, const ActionSink& sink, const std::string& id)
{
    const int speed = servo_speed(args["speed"]);
    const bool hasPan  = !args["pan"].isNull();
    const bool hasTilt = !args["tilt"].isNull();

    int pan  = std::clamp(args["pan"].as<int>(), -kPanMaxDeg, kPanMaxDeg);
    int tilt = std::clamp(args["tilt"].as<int>(), -kTiltMaxDeg, kTiltMaxDeg);

    {
        LvglLockGuard lock;  // StackChan motion runs under the lvgl lock
        auto& motion = GetStackChan().motion();
        // Only drive the axes the model actually specified, so a pan-only request
        // doesn't snap the pitch to centre (mirrors the MCP head tool).
        if (hasPan) {
            motion.yawServo().moveWithSpeed(pan * kDegToUnits, speed);
        }
        if (hasTilt) {
            motion.pitchServo().moveWithSpeed(tilt * kDegToUnits, speed);
        }
    }

    mclog::tagInfo(_tag, "move_head pan={} tilt={} speed={}", hasPan ? pan : 0, hasTilt ? tilt : 0, speed);
    if (sink.result) {
        sink.result(id, true, "");
    }
}

/* ------------------------------ play_gesture ------------------------------- */

// One keyframe write. Lock per step (not across the sleeps) so the 50 Hz avatar
// update task can keep taking the lvgl lock between keyframes.
void gesture_step(int yawUnits, int pitchUnits, int speed)
{
    LvglLockGuard lock;
    GetStackChan().motion().moveWithSpeed(yawUnits, pitchUnits, speed);
}

void gesture_wait(int ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void do_play_gesture(const ArduinoJson::JsonDocument& args, const ActionSink& sink, const std::string& id)
{
    const char* g = args["gesture"].as<const char*>();
    std::string gesture = g ? g : "";

    // Baseline = where the head is now, so gestures look natural relative to the
    // current pose and return there when done. The servo clamps anything out of range.
    int baseYaw   = 0;
    int basePitch = 0;
    {
        LvglLockGuard lock;
        auto& motion = GetStackChan().motion();
        baseYaw   = motion.getCurrentYawAngle();
        basePitch = motion.getCurrentPitchAngle();
    }

    bool ok = true;
    if (gesture == "nod") {
        gesture_step(baseYaw, basePitch - 200, 600);
        gesture_wait(220);
        gesture_step(baseYaw, basePitch + 150, 600);
        gesture_wait(220);
        gesture_step(baseYaw, basePitch - 150, 600);
        gesture_wait(220);
        gesture_step(baseYaw, basePitch, 500);
    } else if (gesture == "shake") {
        gesture_step(baseYaw - 300, basePitch, 700);
        gesture_wait(200);
        gesture_step(baseYaw + 300, basePitch, 700);
        gesture_wait(200);
        gesture_step(baseYaw - 200, basePitch, 700);
        gesture_wait(200);
        gesture_step(baseYaw, basePitch, 500);
    } else if (gesture == "look_around") {
        gesture_step(baseYaw - 450, basePitch, 400);
        gesture_wait(500);
        gesture_step(baseYaw + 450, basePitch, 400);
        gesture_wait(500);
        gesture_step(baseYaw, basePitch, 400);
    } else if (gesture == "tilt") {
        // No roll axis on stack-chan; approximate a quizzical head-cock with a
        // combined yaw+pitch offset held briefly, then return.
        gesture_step(baseYaw + 200, basePitch + 150, 500);
        gesture_wait(700);
        gesture_step(baseYaw, basePitch, 500);
    } else {
        ok = false;
        mclog::tagWarn(_tag, "unknown gesture '{}'", gesture);
    }

    if (sink.result) {
        sink.result(id, ok, ok ? gesture : (std::string("unknown gesture: ") + gesture));
    }
}

/* ----------------------------- set_expression ------------------------------ */

void do_set_expression(const ArduinoJson::JsonDocument& args, const ActionSink& sink, const std::string& id)
{
    const char* e = args["expression"].as<const char*>();
    std::string expr = e ? e : "neutral";

    // The tool vocabulary matches StackChanAvatarDisplay::SetEmotion(), except the
    // server says "doubt" where the firmware mapping expects "doubtful".
    std::string mapped = (expr == "doubt") ? "doubtful" : expr;

    face()->SetEmotion(mapped.c_str());  // takes the display (lvgl) lock internally

    mclog::tagInfo(_tag, "set_expression {}", expr);
    if (sink.result) {
        sink.result(id, true, expr);
    }
}

/* ------------------------------ capture_image ------------------------------ */

void do_capture_image(const ActionSink& sink, const std::string& actionId)
{
#ifndef CONFIG_IDF_TARGET_ESP32
    auto* cam = hal_bridge::board_get_camera();
    if (cam == nullptr) {
        mclog::tagError(_tag, "capture_image: no camera");
        if (sink.result) {
            sink.result(actionId, false, "no camera");
        }
        return;
    }

    // play_shutter=false: the shutter sfx routes through the xiaozhi AudioService,
    // which the custom agent never initialises (it has its own audio path). Playing
    // it here previously crashed in AudioService::PlaySound (null codec). The agent
    // also wouldn't want the shutter mixing into the TTS I2S stream anyway.
    if (!cam->Capture(false)) {
        mclog::tagError(_tag, "capture_image: capture failed");
        if (sink.result) {
            sink.result(actionId, false, "capture failed");
        }
        return;
    }

    // Encode the raw sensor frame to JPEG. This is the heavy step (~hundreds of ms,
    // ~8KB SRAM) -- it runs on the worker task, off the WS receive path, and writes
    // to a heap buffer the caller owns. image_to_jpeg() allocates via malloc/
    // heap_caps_malloc(SPIRAM) depending on the path, and ESP-IDF's plain free()
    // releases either (the encoder itself frees its scratch with free()), so we
    // free(jpeg) regardless of which path produced it.
    uint8_t* jpeg   = nullptr;
    size_t jpeg_len = 0;
    const bool ok   = image_to_jpeg(const_cast<uint8_t*>(cam->GetFrameData()), cam->GetFrameSize(),
                                    static_cast<uint16_t>(cam->GetFrameWidth()),
                                    static_cast<uint16_t>(cam->GetFrameHeight()),
                                    static_cast<v4l2_pix_fmt_t>(cam->GetFrameFormat()), /*quality=*/80, &jpeg,
                                    &jpeg_len);
    if (!ok || jpeg == nullptr || jpeg_len == 0) {
        mclog::tagError(_tag, "capture_image: jpeg encode failed");
        if (jpeg) {
            free(jpeg);
        }
        if (sink.result) {
            sink.result(actionId, false, "encode failed");
        }
        return;
    }

    mclog::tagInfo(_tag, "capture_image: {} bytes jpeg", jpeg_len);

    const std::string imageId = "img-" + actionId;
    if (sink.imageStart) {
        sink.imageStart(imageId, actionId, "image/jpeg");
    }
    constexpr size_t kChunk = 4096;
    for (size_t off = 0; off < jpeg_len; off += kChunk) {
        const size_t n = std::min(kChunk, jpeg_len - off);
        if (sink.imageChunk) {
            sink.imageChunk(jpeg + off, n);
        }
    }
    if (sink.imageEnd) {
        sink.imageEnd(imageId);
    }

    free(jpeg);
#else
    if (sink.result) {
        sink.result(actionId, false, "camera unsupported");
    }
#endif  // ndef CONFIG_IDF_TARGET_ESP32
}

}  // namespace

/* --------------------------------- dispatch -------------------------------- */

void execute_action(const ActionSink& sink, const std::string& id, const std::string& name,
                    const std::string& argsJson)
{
    ArduinoJson::JsonDocument args;
    if (!argsJson.empty()) {
        auto err = ArduinoJson::deserializeJson(args, argsJson);
        if (err) {
            mclog::tagWarn(_tag, "action '{}' bad args: {}", name, err.c_str());
        }
    }

    if (name == "move_head") {
        do_move_head(args, sink, id);
    } else if (name == "play_gesture") {
        do_play_gesture(args, sink, id);
    } else if (name == "set_expression") {
        do_set_expression(args, sink, id);
    } else if (name == "capture_image") {
        do_capture_image(sink, id);
    } else {
        mclog::tagWarn(_tag, "unknown tool '{}'", name);
        if (sink.result) {
            sink.result(id, false, std::string("unknown tool: ") + name);
        }
    }
}

}  // namespace custom_agent
