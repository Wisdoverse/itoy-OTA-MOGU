#include "touch_pad.h"
#include "config.h"
#include <esp_log.h>

#define TAG "TouchPad"

TouchPad::TouchPad() {
    for (int i = 0; i < TOUCH_PAD_COUNT; i++) {
        thresholds_[i] = 0;
        baselines_[i] = 0;
        last_state_[i] = false;
    }
}

TouchPad::~TouchPad() {
    if (task_handle_) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
    touch_pad_deinit();
}

void TouchPad::Initialize() {
    if (initialized_) return;

    // ESP32-S3 touch 初始化 (legacy API, S3 版本)
    touch_pad_init();
    touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);
    for (int i = 0; i < TOUCH_PAD_COUNT; i++) {
        touch_pad_config(kTouchChannels[i]);   // S3 legacy: 仅通道号, 无 threshold 参数
    }

    // 软件滤波 (S3 legacy: filter_set_config + filter_enable)
    touch_filter_config_t filter_cfg = {
        .mode = TOUCH_PAD_FILTER_IIR_16,
        .debounce_cnt = 1,
        .noise_thr = 0,
        .jitter_step = 4,
        .smh_lvl = TOUCH_PAD_SMOOTH_IIR_2,
    };
    touch_pad_filter_set_config(&filter_cfg);
    touch_pad_filter_enable();

    // 启动测量状态机 (S3 需显式启动才会持续采样)
    touch_pad_fsm_start();

    // 等待稳定
    vTaskDelay(pdMS_TO_TICKS(100));

    // 先读取一次基准值用于阈值
    for (int i = 0; i < TOUCH_PAD_COUNT; i++) {
        uint32_t val = 0;
        touch_pad_read_raw_data(kTouchChannels[i], &val);
        baselines_[i] = val;
        thresholds_[i] = (uint32_t)(val * 0.8f);
        ESP_LOGI(TAG, "Touch ch%d (GPIO%d): baseline=%lu, threshold=%lu",
                 i, i + 1, baselines_[i], thresholds_[i]);
    }

    // 设置硬件阈值 (轮询模式下主要用软件阈值, 这里同步一份)
    for (int i = 0; i < TOUCH_PAD_COUNT; i++) {
        touch_pad_set_thresh(kTouchChannels[i], thresholds_[i]);
    }

    initialized_ = true;
    ESP_LOGI(TAG, "Touch pad initialized (%d channels)", TOUCH_PAD_COUNT);
}

void TouchPad::Calibrate(float ratio) {
    if (!initialized_) Initialize();

    const int samples = 100;
    ESP_LOGI(TAG, "Calibrating... keep fingers off the pads");

    std::array<uint64_t, TOUCH_PAD_COUNT> sums{};
    for (int s = 0; s < samples; s++) {
        for (int i = 0; i < TOUCH_PAD_COUNT; i++) {
            uint32_t val = 0;
            touch_pad_filter_read_smooth(kTouchChannels[i], &val);
            sums[i] += val;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    for (int i = 0; i < TOUCH_PAD_COUNT; i++) {
        baselines_[i] = (uint32_t)(sums[i] / samples);
        thresholds_[i] = (uint32_t)(baselines_[i] * ratio);
        touch_pad_set_thresh(kTouchChannels[i], thresholds_[i]);
        ESP_LOGI(TAG, "Calibrated ch%d: baseline=%lu, threshold=%lu",
                 i, baselines_[i], thresholds_[i]);
    }
}

uint32_t TouchPad::GetRawValue(int channel) const {
    if (channel < 0 || channel >= TOUCH_PAD_COUNT) return 0;
    uint32_t val = 0;
    if (touch_pad_filter_read_smooth(kTouchChannels[channel], &val) != ESP_OK) {
        touch_pad_read_raw_data(kTouchChannels[channel], &val);
    }
    return val;
}

bool TouchPad::IsPressed(int channel) const {
    if (channel < 0 || channel >= TOUCH_PAD_COUNT) return false;
    return GetRawValue(channel) < thresholds_[channel];
}

void TouchPad::SetThreshold(int channel, uint32_t threshold) {
    if (channel < 0 || channel >= TOUCH_PAD_COUNT) return;
    thresholds_[channel] = threshold;
    touch_pad_set_thresh(kTouchChannels[channel], threshold);
}

uint32_t TouchPad::GetThreshold(int channel) const {
    if (channel < 0 || channel >= TOUCH_PAD_COUNT) return 0;
    return thresholds_[channel];
}

void TouchPad::ScanOnce() {
    for (int i = 0; i < TOUCH_PAD_COUNT; i++) {
        bool pressed = IsPressed(i);
        if (pressed != last_state_[i]) {
            last_state_[i] = pressed;
            if (callback_) {
                callback_(i, pressed);
            }
        }
    }
}

void TouchPad::StartScanTask(int interval_ms, int stack_size) {
    scan_interval_ms_ = interval_ms;
    xTaskCreate(ScanTaskFunc, "touch_scan", stack_size, this, 5, &task_handle_);
}

void TouchPad::ScanTaskFunc(void* arg) {
    auto* self = static_cast<TouchPad*>(arg);
    while (true) {
        self->ScanOnce();
        vTaskDelay(pdMS_TO_TICKS(self->scan_interval_ms_));
    }
}