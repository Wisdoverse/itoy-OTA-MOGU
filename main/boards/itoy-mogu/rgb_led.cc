#include "rgb_led.h"
#include <esp_log.h>
#include <esp_err.h>

#define TAG "RgbLed"

RgbLed::RgbLed() {}

RgbLed::~RgbLed() {
    if (task_) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
    if (strip_) {
        led_strip_clear(strip_);
        led_strip_del(strip_);
        strip_ = nullptr;
    }
}

void RgbLed::Initialize() {
    if (strip_) return;

    count_ = CONFIG_ITOY_RGB_LED_COUNT;
    if (count_ <= 0) count_ = 1;

    led_strip_config_t sc = {};
    sc.strip_gpio_num = RGB_LED_GPIO;
    sc.max_leds = (uint32_t)count_;
    sc.led_model = LED_MODEL_WS2812;
    sc.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    sc.flags.invert_out = false;

    led_strip_rmt_config_t rc = {};
    rc.clk_src = RMT_CLK_SRC_DEFAULT;
    rc.resolution_hz = 10 * 1000 * 1000;   // 10 MHz
    rc.mem_block_symbols = 0;
    rc.flags.with_dma = false;

    if (led_strip_new_rmt_device(&sc, &rc, &strip_) != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed");
        return;
    }
    led_strip_clear(strip_);

    xTaskCreate(AnimTaskFunc, "rgb_anim", 2560, this, 5, &task_);
    ESP_LOGI(TAG, "WS2812 init: %d LED(s) GPIO%d", count_, (int)RGB_LED_GPIO);
}

void RgbLed::SetBrightness(uint8_t cap_pct) {
    if (cap_pct > 100) cap_pct = 100;
    cap_pct_ = cap_pct;
    dirty_ = true;
}

void RgbLed::Off() {
    effect_ = RGB_OFF;
    dirty_ = true;
    ESP_LOGI(TAG, "rgb OFF");
}

void RgbLed::Solid(uint8_t r, uint8_t g, uint8_t b, uint8_t bright_pct) {
    if (bright_pct > 100) bright_pct = 100;
    r_ = r; g_ = g; b_ = b;
    cur_pct_ = bright_pct;
    effect_ = RGB_SOLID;
    dirty_ = true;
    ESP_LOGI(TAG, "rgb SOLID (%d,%d,%d) %d%%", r, g, b, bright_pct);
}

void RgbLed::Breath(uint8_t r, uint8_t g, uint8_t b, uint32_t period_ms,
                    uint8_t lo_pct, uint8_t hi_pct) {
    if (lo_pct > 100) lo_pct = 100;
    if (hi_pct > 100) hi_pct = 100;
    if (period_ms < 200) period_ms = 200;
    r_ = r; g_ = g; b_ = b;
    period_ms_ = period_ms;
    lo_pct_ = lo_pct;
    hi_pct_ = hi_pct;
    effect_ = RGB_BREATH;
    dirty_ = true;
    ESP_LOGI(TAG, "rgb BREATH (%d,%d,%d) %d%%<->%d%% per %lums",
             r, g, b, lo_pct, hi_pct, (unsigned long)period_ms);
}

void RgbLed::FadeTo(uint8_t r, uint8_t g, uint8_t b, uint8_t target_pct, uint32_t dur_ms) {
    if (target_pct > 100) target_pct = 100;
    r_ = r; g_ = g; b_ = b;
    fade_from_pct_ = cur_pct_;        // 从当前亮度渐变
    cur_pct_ = target_pct;            // 终点
    fade_dur_ms_ = dur_ms;
    fade_start_tick_ = tick_;
    effect_ = RGB_FADE;
    dirty_ = true;
    ESP_LOGI(TAG, "rgb FADE (%d,%d,%d) ->%d%% over %lums",
             r, g, b, target_pct, (unsigned long)dur_ms);
}

uint8_t RgbLed::ComputeBrightPct() {
    uint8_t pct;
    switch (effect_) {
        case RGB_SOLID:
            pct = cur_pct_;
            break;
        case RGB_FADE: {
            uint32_t elapsed = (tick_ - fade_start_tick_) * 10;
            if (elapsed >= fade_dur_ms_) {
                pct = cur_pct_;
                effect_ = RGB_SOLID;   // 渐变完成 -> 保持
            } else {
                int32_t span = (int32_t)cur_pct_ - (int32_t)fade_from_pct_;
                pct = (uint8_t)((int32_t)fade_from_pct_ + span * (int32_t)elapsed / (int32_t)fade_dur_ms_);
            }
            break;
        }
        case RGB_BREATH: {
            uint32_t pos = (tick_ * 10) % period_ms_;
            uint32_t half = period_ms_ / 2;
            if (half == 0) half = 1;
            int32_t frac;   // 0..1000 三角波
            if (pos < half) frac = (int32_t)(pos * 1000 / half);
            else            frac = (int32_t)(1000 - (pos - half) * 1000 / half);
            int32_t v = (int32_t)lo_pct_ + (int32_t)(hi_pct_ - lo_pct_) * frac / 1000;
            pct = (uint8_t)v;
            break;
        }
        default:
            pct = 0;
            break;
    }
    return pct;
}

void RgbLed::AnimLoop() {
    while (true) {
        bool continuous = (effect_ == RGB_BREATH || effect_ == RGB_FADE);
        if (continuous) dirty_ = true;

        if (dirty_ && strip_) {
            uint8_t pct = ComputeBrightPct();
            uint32_t applied = (uint32_t)pct * 255 / 100;          // 目标 0..255
            applied = applied * (uint32_t)cap_pct_ / 100;           // 全局上限
            uint32_t rr = (uint32_t)r_ * applied / 255;
            uint32_t gg = (uint32_t)g_ * applied / 255;
            uint32_t bb = (uint32_t)b_ * applied / 255;
            for (int i = 0; i < count_; i++) {
                led_strip_set_pixel(strip_, (uint32_t)i, rr, gg, bb);
            }
            led_strip_refresh(strip_);
            if (!continuous) dirty_ = false;
        }

        tick_++;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void RgbLed::AnimTaskFunc(void* arg) {
    static_cast<RgbLed*>(arg)->AnimLoop();
}
