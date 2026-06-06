/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "workers.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <agent/agent_config.h>
#include <array>
#include <vector>

using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace setup_workers;

static std::string _tag = "Setup-AIAgent";

namespace {
static const std::array<const char*, 4> _idle_motion_level_labels = {{"Off", "Low", "Medium", "High"}};

// Index of the first level >= value, clamped to the last level. Used to map a stored
// config value onto the discrete slider positions below.
static int level_index_for(const std::vector<int>& levels, int value)
{
    for (size_t i = 0; i < levels.size(); ++i) {
        if (levels[i] >= value) {
            return static_cast<int>(i);
        }
    }
    return static_cast<int>(levels.size()) - 1;
}

}  // namespace

XiaozhiPowerSavingWorker::XiaozhiPowerSavingWorker()
{
    mclog::info("XiaozhiPowerSavingWorker start");

    for (uint32_t seconds = 0; seconds <= 3600; seconds += 300) {
        _idle_shutdown_levels.push_back(seconds);
    }

    _config = GetHAL().getXiaozhiConfig();

    int current_index = static_cast<int>(_idle_shutdown_levels.size()) - 1;
    for (size_t i = 0; i < _idle_shutdown_levels.size(); ++i) {
        if (_idle_shutdown_levels[i] >= _config.idleShutdownTimeSeconds) {
            current_index = static_cast<int>(i);
            break;
        }
    }

    _panel = std::make_unique<Container>(lv_screen_active());
    _panel->setBgColor(lv_color_hex(0xEDF4FF));
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setBorderWidth(0);
    _panel->setSize(320, 240);
    _panel->setRadius(0);
    _panel->setPadding(0, 50, 24, 18);
    _panel->setScrollDir(LV_DIR_VER);
    _panel->setScrollbarMode(LV_SCROLLBAR_MODE_ACTIVE);

    _panel_idle_shutdown = std::make_unique<Container>(_panel->get());
    _panel_idle_shutdown->setSize(296, 148);
    _panel_idle_shutdown->align(LV_ALIGN_TOP_MID, 0, 20);
    _panel_idle_shutdown->setBgColor(lv_color_hex(0xD2E3FF));
    _panel_idle_shutdown->setBorderWidth(0);
    _panel_idle_shutdown->setRadius(18);
    _panel_idle_shutdown->setPadding(0, 0, 0, 0);
    _panel_idle_shutdown->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _label_idle_title = std::make_unique<Label>(_panel_idle_shutdown->get());
    _label_idle_title->setText("Automatically power off after being idle for:");
    _label_idle_title->setWidth(280);
    _label_idle_title->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _label_idle_title->setTextFont(&lv_font_montserrat_16);
    _label_idle_title->setTextColor(lv_color_hex(0x26206A));
    _label_idle_title->align(LV_ALIGN_TOP_MID, 0, 18);

    _label_idle_value = std::make_unique<Label>(_panel_idle_shutdown->get());
    _label_idle_value->setTextFont(&lv_font_montserrat_24);
    _label_idle_value->setTextColor(lv_color_hex(0x26206A));
    _label_idle_value->align(LV_ALIGN_TOP_MID, 0, 64);

    _slider_idle_shutdown = std::make_unique<Slider>(_panel_idle_shutdown->get());
    _slider_idle_shutdown->align(LV_ALIGN_TOP_MID, 0, 106);
    _slider_idle_shutdown->setRange(0, _idle_shutdown_levels.size() - 1);
    _slider_idle_shutdown->setSize(250, 18);
    _slider_idle_shutdown->setBgColor(lv_color_hex(0x615B9E), LV_PART_KNOB);
    _slider_idle_shutdown->setBgColor(lv_color_hex(0x615B9E), LV_PART_INDICATOR);
    _slider_idle_shutdown->setBgColor(lv_color_hex(0xB8D3FD), LV_PART_MAIN);
    _slider_idle_shutdown->setBgOpa(255);
    _slider_idle_shutdown->setValue(current_index);
    _slider_idle_shutdown->onValueChanged().connect([this](int32_t value) { _pending_idle_index = value; });

    _panel_charging = std::make_unique<Container>(_panel->get());
    _panel_charging->setSize(296, 120);
    _panel_charging->align(LV_ALIGN_TOP_MID, 0, 188);
    _panel_charging->setBgColor(lv_color_hex(0xD2E3FF));
    _panel_charging->setBorderWidth(0);
    _panel_charging->setRadius(18);
    _panel_charging->setPadding(0, 0, 0, 0);
    _panel_charging->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _label_charging_title = std::make_unique<Label>(_panel_charging->get());
    _label_charging_title->setText("Allow auto power off while charging:");
    _label_charging_title->setTextFont(&lv_font_montserrat_16);
    _label_charging_title->setTextColor(lv_color_hex(0x26206A));
    _label_charging_title->setWidth(260);
    _label_charging_title->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _label_charging_title->align(LV_ALIGN_TOP_MID, 0, 18);

    _switch_charging = std::make_unique<Switch>(_panel_charging->get());
    _switch_charging->setSize(64, 36);
    _switch_charging->align(LV_ALIGN_TOP_MID, 0, 66);
    _switch_charging->setBgColor(lv_color_hex(0xB8D3FD), LV_PART_MAIN);
    _switch_charging->setBgColor(lv_color_hex(0x615B9E), LV_PART_INDICATOR | LV_STATE_CHECKED);
    _switch_charging->setBgColor(lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    if (_config.allowShutdownWhenCharging) {
        _switch_charging->addState(LV_STATE_CHECKED);
    }

    _btn_confirm = std::make_unique<Button>(_panel->get());
    apply_button_common_style(*_btn_confirm);
    _btn_confirm->align(LV_ALIGN_TOP_MID, 0, 326);
    _btn_confirm->setSize(290, 50);
    _btn_confirm->label().setText("Confirm");
    _btn_confirm->onClick().connect([this]() { _confirm_flag = true; });

    update_idle_label();
}

void XiaozhiPowerSavingWorker::update()
{
    if (_pending_idle_index != -1) {
        _config.idleShutdownTimeSeconds = _idle_shutdown_levels[_pending_idle_index];
        _pending_idle_index             = -1;
        update_idle_label();
    }

    if (_confirm_flag) {
        _confirm_flag                     = false;
        _config.allowShutdownWhenCharging = _switch_charging->getValue();
        GetHAL().setXiaozhiConfig(_config);
        mclog::tagInfo(_tag, "xiaozhi config updated: idleShutdownTimeSeconds={}, allowShutdownWhenCharging={}",
                       _config.idleShutdownTimeSeconds, _config.allowShutdownWhenCharging);
        _is_done = true;
    }
}

void XiaozhiPowerSavingWorker::update_idle_label()
{
    if (_config.idleShutdownTimeSeconds == 0) {
        _label_idle_value->setText("Off");
        return;
    }

    auto total_minutes = _config.idleShutdownTimeSeconds / 60;
    _label_idle_value->setText(fmt::format("{} min", total_minutes));
}

XiaozhiGeneralWorker::XiaozhiGeneralWorker()
{
    mclog::info("XiaozhiGeneralWorker start");

    _config = GetHAL().getXiaozhiConfig();

    for (uint8_t level = 0; level < _idle_motion_level_labels.size(); ++level) {
        _idle_motion_levels.push_back(level);
    }

    int current_index = static_cast<int>(_idle_motion_levels.size()) - 1;
    for (size_t i = 0; i < _idle_motion_levels.size(); ++i) {
        if (_idle_motion_levels[i] >= _config.idleRandomMovementLevel) {
            current_index = static_cast<int>(i);
            break;
        }
    }

    _panel = std::make_unique<Container>(lv_screen_active());
    _panel->setBgColor(lv_color_hex(0xEDF4FF));
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setBorderWidth(0);
    _panel->setSize(320, 240);
    _panel->setRadius(0);
    _panel->setPadding(0, 50, 24, 18);
    _panel->setScrollDir(LV_DIR_VER);
    _panel->setScrollbarMode(LV_SCROLLBAR_MODE_ACTIVE);

    _panel_general = std::make_unique<Container>(_panel->get());
    _panel_general->setSize(296, 156);
    _panel_general->align(LV_ALIGN_TOP_MID, 0, 20);
    _panel_general->setBgColor(lv_color_hex(0xD2E3FF));
    _panel_general->setBorderWidth(0);
    _panel_general->setRadius(18);
    _panel_general->setPadding(0, 0, 0, 0);
    _panel_general->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _label_idle_motion_title = std::make_unique<Label>(_panel_general->get());
    _label_idle_motion_title->setText("Idle movement frequency:");
    _label_idle_motion_title->setTextFont(&lv_font_montserrat_16);
    _label_idle_motion_title->setTextColor(lv_color_hex(0x26206A));
    _label_idle_motion_title->setWidth(260);
    _label_idle_motion_title->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _label_idle_motion_title->align(LV_ALIGN_TOP_MID, 0, 18);

    _label_idle_motion_value = std::make_unique<Label>(_panel_general->get());
    _label_idle_motion_value->setTextFont(&lv_font_montserrat_24);
    _label_idle_motion_value->setTextColor(lv_color_hex(0x26206A));
    _label_idle_motion_value->align(LV_ALIGN_TOP_MID, 0, 64);

    _slider_idle_motion = std::make_unique<Slider>(_panel_general->get());
    _slider_idle_motion->align(LV_ALIGN_TOP_MID, 0, 118);
    _slider_idle_motion->setRange(0, _idle_motion_levels.size() - 1);
    _slider_idle_motion->setSize(250, 18);
    _slider_idle_motion->setBgColor(lv_color_hex(0x615B9E), LV_PART_KNOB);
    _slider_idle_motion->setBgColor(lv_color_hex(0x615B9E), LV_PART_INDICATOR);
    _slider_idle_motion->setBgColor(lv_color_hex(0xB8D3FD), LV_PART_MAIN);
    _slider_idle_motion->setBgOpa(255);
    _slider_idle_motion->setValue(current_index);
    _slider_idle_motion->onValueChanged().connect([this](int32_t value) { _pending_idle_motion_index = value; });

    _panel_startup = std::make_unique<Container>(_panel->get());
    _panel_startup->setSize(296, 120);
    _panel_startup->align(LV_ALIGN_TOP_MID, 0, 196);
    _panel_startup->setBgColor(lv_color_hex(0xD2E3FF));
    _panel_startup->setBorderWidth(0);
    _panel_startup->setRadius(18);
    _panel_startup->setPadding(0, 0, 0, 0);
    _panel_startup->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _label_startup_title = std::make_unique<Label>(_panel_startup->get());
    _label_startup_title->setText("Start AI Agent on boot:");
    _label_startup_title->setTextFont(&lv_font_montserrat_16);
    _label_startup_title->setTextColor(lv_color_hex(0x26206A));
    _label_startup_title->setWidth(260);
    _label_startup_title->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _label_startup_title->align(LV_ALIGN_TOP_MID, 0, 18);

    _switch_start_ai_on_boot = std::make_unique<Switch>(_panel_startup->get());
    _switch_start_ai_on_boot->setSize(64, 36);
    _switch_start_ai_on_boot->align(LV_ALIGN_TOP_MID, 0, 66);
    _switch_start_ai_on_boot->setBgColor(lv_color_hex(0xB8D3FD), LV_PART_MAIN);
    _switch_start_ai_on_boot->setBgColor(lv_color_hex(0x615B9E), LV_PART_INDICATOR | LV_STATE_CHECKED);
    _switch_start_ai_on_boot->setBgColor(lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    if (_config.startAiAgentOnBoot) {
        _switch_start_ai_on_boot->addState(LV_STATE_CHECKED);
    }

    _btn_confirm = std::make_unique<Button>(_panel->get());
    apply_button_common_style(*_btn_confirm);
    _btn_confirm->align(LV_ALIGN_TOP_MID, 0, 326);
    _btn_confirm->setSize(290, 50);
    _btn_confirm->label().setText("Confirm");
    _btn_confirm->onClick().connect([this]() { _confirm_flag = true; });

    update_idle_motion_label();
}

void XiaozhiGeneralWorker::update()
{
    if (_pending_idle_motion_index != -1) {
        _config.idleRandomMovementLevel = _idle_motion_levels[_pending_idle_motion_index];
        _pending_idle_motion_index      = -1;
        update_idle_motion_label();
    }

    if (_confirm_flag) {
        _confirm_flag = false;
        _config.startAiAgentOnBoot = _switch_start_ai_on_boot->getValue();
        GetHAL().setXiaozhiConfig(_config);
        mclog::tagInfo(_tag, "xiaozhi config updated: idleRandomMovementLevel={} ({})", _config.idleRandomMovementLevel,
                       _idle_motion_level_labels[_config.idleRandomMovementLevel]);
        _is_done = true;
    }
}

void XiaozhiGeneralWorker::update_idle_motion_label()
{
    _label_idle_motion_value->setText(_idle_motion_level_labels[_config.idleRandomMovementLevel]);
}

AgentBackendWorker::AgentBackendWorker()
{
    mclog::info("AgentBackendWorker start");

    _config  = custom_agent::load_config();
    _live_on = (_config.route == custom_agent::Route::Live);

    _panel = std::make_unique<Container>(lv_screen_active());
    _panel->setBgColor(lv_color_hex(0xEDF4FF));
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setBorderWidth(0);
    _panel->setSize(320, 240);
    _panel->setRadius(0);
    _panel->setPadding(0, 50, 24, 18);
    _panel->setScrollDir(LV_DIR_VER);
    _panel->setScrollbarMode(LV_SCROLLBAR_MODE_ACTIVE);

    _panel_backend = std::make_unique<Container>(_panel->get());
    _panel_backend->setSize(296, 206);
    _panel_backend->align(LV_ALIGN_TOP_MID, 0, 20);
    _panel_backend->setBgColor(lv_color_hex(0xD2E3FF));
    _panel_backend->setBorderWidth(0);
    _panel_backend->setRadius(18);
    _panel_backend->setPadding(0, 0, 0, 0);
    _panel_backend->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _label_title = std::make_unique<Label>(_panel_backend->get());
    _label_title->setText("AI Agent backend:");
    _label_title->setTextFont(&lv_font_montserrat_16);
    _label_title->setTextColor(lv_color_hex(0x26206A));
    _label_title->setWidth(260);
    _label_title->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _label_title->align(LV_ALIGN_TOP_MID, 0, 16);

    _label_value = std::make_unique<Label>(_panel_backend->get());
    _label_value->setTextFont(&lv_font_montserrat_24);
    _label_value->setTextColor(lv_color_hex(0x26206A));
    _label_value->setWidth(280);
    _label_value->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _label_value->align(LV_ALIGN_TOP_MID, 0, 52);

    _switch_live = std::make_unique<Switch>(_panel_backend->get());
    _switch_live->setSize(64, 36);
    _switch_live->align(LV_ALIGN_TOP_MID, 0, 100);
    _switch_live->setBgColor(lv_color_hex(0xB8D3FD), LV_PART_MAIN);
    _switch_live->setBgColor(lv_color_hex(0x615B9E), LV_PART_INDICATOR | LV_STATE_CHECKED);
    _switch_live->setBgColor(lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    _switch_live->setValue(_live_on);
    _switch_live->onValueChanged().connect([this](bool value) {
        _live_on     = value;
        _label_dirty = true;
    });

    _label_hint = std::make_unique<Label>(_panel_backend->get());
    _label_hint->setText("Off: Agent (VoiceVox)\nOn: Live (Gemini)");
    _label_hint->setTextFont(&lv_font_montserrat_16);
    _label_hint->setTextColor(lv_color_hex(0x615B9E));
    _label_hint->setWidth(260);
    _label_hint->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _label_hint->align(LV_ALIGN_TOP_MID, 0, 146);

    _btn_confirm = std::make_unique<Button>(_panel->get());
    apply_button_common_style(*_btn_confirm);
    _btn_confirm->align(LV_ALIGN_TOP_MID, 0, 240);
    _btn_confirm->setSize(290, 50);
    _btn_confirm->label().setText("Confirm");
    _btn_confirm->onClick().connect([this]() { _confirm_flag = true; });

    update_value_label();
}

void AgentBackendWorker::update()
{
    if (_label_dirty) {
        _label_dirty = false;
        update_value_label();
    }

    if (_confirm_flag) {
        _confirm_flag = false;
        _config.route = _live_on ? custom_agent::Route::Live : custom_agent::Route::Agent;
        custom_agent::save_config(_config);
        mclog::tagInfo(_tag, "agent backend route updated: {} (/ws/{})", _live_on ? "Live" : "Agent",
                       custom_agent::route_name(_config.route));
        _is_done = true;
    }
}

void AgentBackendWorker::update_value_label()
{
    if (_live_on) {
        _label_value->setText("Live  /ws/live");
    } else {
        _label_value->setText("Agent  /ws/agent");
    }
}

AgentVoiceWorker::AgentVoiceWorker()
{
    mclog::info("AgentVoiceWorker start");

    _config = custom_agent::load_config();

    // Min. voice length to send: 0.0 .. 2.0 s in 0.1 s (100 ms) steps.
    for (int ms = 0; ms <= 2000; ms += 100) {
        _length_levels.push_back(ms);
    }
    // Min. voice volume (mean-abs amplitude): 0 .. 2000 in steps of 50.
    for (int rms = 0; rms <= 2000; rms += 50) {
        _volume_levels.push_back(rms);
    }

    const int length_index = level_index_for(_length_levels, _config.vadMinSpeechMs);
    const int volume_index = level_index_for(_volume_levels, _config.vadStartRms);

    _panel = std::make_unique<Container>(lv_screen_active());
    _panel->setBgColor(lv_color_hex(0xEDF4FF));
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setBorderWidth(0);
    _panel->setSize(320, 240);
    _panel->setRadius(0);
    _panel->setPadding(0, 50, 24, 18);
    _panel->setScrollDir(LV_DIR_VER);
    _panel->setScrollbarMode(LV_SCROLLBAR_MODE_ACTIVE);

    // ---- Minimum voice length panel ----
    _panel_length = std::make_unique<Container>(_panel->get());
    _panel_length->setSize(296, 148);
    _panel_length->align(LV_ALIGN_TOP_MID, 0, 20);
    _panel_length->setBgColor(lv_color_hex(0xD2E3FF));
    _panel_length->setBorderWidth(0);
    _panel_length->setRadius(18);
    _panel_length->setPadding(0, 0, 0, 0);
    _panel_length->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _label_length_title = std::make_unique<Label>(_panel_length->get());
    _label_length_title->setText("Minimum voice length to send:");
    _label_length_title->setWidth(280);
    _label_length_title->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _label_length_title->setTextFont(&lv_font_montserrat_16);
    _label_length_title->setTextColor(lv_color_hex(0x26206A));
    _label_length_title->align(LV_ALIGN_TOP_MID, 0, 18);

    _label_length_value = std::make_unique<Label>(_panel_length->get());
    _label_length_value->setTextFont(&lv_font_montserrat_24);
    _label_length_value->setTextColor(lv_color_hex(0x26206A));
    _label_length_value->align(LV_ALIGN_TOP_MID, 0, 64);

    _slider_length = std::make_unique<Slider>(_panel_length->get());
    _slider_length->align(LV_ALIGN_TOP_MID, 0, 106);
    _slider_length->setRange(0, _length_levels.size() - 1);
    _slider_length->setSize(250, 18);
    _slider_length->setBgColor(lv_color_hex(0x615B9E), LV_PART_KNOB);
    _slider_length->setBgColor(lv_color_hex(0x615B9E), LV_PART_INDICATOR);
    _slider_length->setBgColor(lv_color_hex(0xB8D3FD), LV_PART_MAIN);
    _slider_length->setBgOpa(255);
    _slider_length->setValue(length_index);
    _slider_length->onValueChanged().connect([this](int32_t value) { _pending_length_index = value; });

    // ---- Minimum voice volume panel ----
    _panel_volume = std::make_unique<Container>(_panel->get());
    _panel_volume->setSize(296, 148);
    _panel_volume->align(LV_ALIGN_TOP_MID, 0, 188);
    _panel_volume->setBgColor(lv_color_hex(0xD2E3FF));
    _panel_volume->setBorderWidth(0);
    _panel_volume->setRadius(18);
    _panel_volume->setPadding(0, 0, 0, 0);
    _panel_volume->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _label_volume_title = std::make_unique<Label>(_panel_volume->get());
    _label_volume_title->setText("Minimum voice volume (louder = stricter):");
    _label_volume_title->setWidth(280);
    _label_volume_title->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _label_volume_title->setTextFont(&lv_font_montserrat_16);
    _label_volume_title->setTextColor(lv_color_hex(0x26206A));
    _label_volume_title->align(LV_ALIGN_TOP_MID, 0, 18);

    _label_volume_value = std::make_unique<Label>(_panel_volume->get());
    _label_volume_value->setTextFont(&lv_font_montserrat_24);
    _label_volume_value->setTextColor(lv_color_hex(0x26206A));
    _label_volume_value->align(LV_ALIGN_TOP_MID, 0, 64);

    _slider_volume = std::make_unique<Slider>(_panel_volume->get());
    _slider_volume->align(LV_ALIGN_TOP_MID, 0, 106);
    _slider_volume->setRange(0, _volume_levels.size() - 1);
    _slider_volume->setSize(250, 18);
    _slider_volume->setBgColor(lv_color_hex(0x615B9E), LV_PART_KNOB);
    _slider_volume->setBgColor(lv_color_hex(0x615B9E), LV_PART_INDICATOR);
    _slider_volume->setBgColor(lv_color_hex(0xB8D3FD), LV_PART_MAIN);
    _slider_volume->setBgOpa(255);
    _slider_volume->setValue(volume_index);
    _slider_volume->onValueChanged().connect([this](int32_t value) { _pending_volume_index = value; });

    _btn_confirm = std::make_unique<Button>(_panel->get());
    apply_button_common_style(*_btn_confirm);
    _btn_confirm->align(LV_ALIGN_TOP_MID, 0, 356);
    _btn_confirm->setSize(290, 50);
    _btn_confirm->label().setText("Confirm");
    _btn_confirm->onClick().connect([this]() { _confirm_flag = true; });

    update_length_label();
    update_volume_label();
}

void AgentVoiceWorker::update()
{
    if (_pending_length_index != -1) {
        _config.vadMinSpeechMs = _length_levels[_pending_length_index];
        _pending_length_index  = -1;
        update_length_label();
    }

    if (_pending_volume_index != -1) {
        _config.vadStartRms   = _volume_levels[_pending_volume_index];
        _pending_volume_index = -1;
        update_volume_label();
    }

    if (_confirm_flag) {
        _confirm_flag = false;
        custom_agent::save_config(_config);
        mclog::tagInfo(_tag, "agent voice updated: vadMinSpeechMs={} vadStartRms={}", _config.vadMinSpeechMs,
                       _config.vadStartRms);
        _is_done = true;
    }
}

void AgentVoiceWorker::update_length_label()
{
    // Render ms as "X.X s" without floating point (e.g. 300 -> "0.3 s").
    const int tenths = _config.vadMinSpeechMs / 100;
    _label_length_value->setText(fmt::format("{}.{} s", tenths / 10, tenths % 10));
}

void AgentVoiceWorker::update_volume_label()
{
    _label_volume_value->setText(fmt::format("{}", _config.vadStartRms));
}
