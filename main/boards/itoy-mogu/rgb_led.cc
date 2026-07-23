#include "rgb_led.h"
#include <esp_log.h>
#include <esp_err.h>

#define TAG "RgbLed"

RgbLed::RgbLed() {}

RgbLed::~RgbLed() {
    if (strip_) {
        led_strip_del(strip_);
        strip_ = nullptr;
    }
}

void RgbLed::Initialize() {
    if (initialized_) return;

    count_ = CONFIG_ITOY_RGB_LED_COUNT;
    if (count_ <= 0) count_ = 1;

    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num = RGB_LED_GPIO;
    strip_cfg.max_leds = (uint32_t)count_;
    strip_cfg.led_model = LED_MODEL_WS2812;
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_cfg.flags.invert_out = false;

    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_cfg.resolution_hz = 10 * 1000 * 1000;   // 10 MHz, WS2812 标准时序
    rmt_cfg.mem_block_symbols = 0;
    rmt_cfg.flags.with_dma = false;

    esp_err_t ret = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(ret));
        return;
    }
    led_strip_clear(strip_);   // 上电默认熄灭

    initialized_ = true;
    ESP_LOGI(TAG, "WS2812 strip init: %d LED(s) on GPIO%d", count_, (int)RGB_LED_GPIO);
}

void RgbLed::SetBrightness(uint8_t b) {
    brightness_ = b;
}

void RgbLed::SetPixel(int index, uint8_t r, uint8_t g, uint8_t b) {
    if (!strip_ || index < 0 || index >= count_) return;
    uint32_t rr = (uint32_t)r * brightness_ / 255;
    uint32_t gg = (uint32_t)g * brightness_ / 255;
    uint32_t bb = (uint32_t)b * brightness_ / 255;
    led_strip_set_pixel(strip_, (uint32_t)index, rr, gg, bb);
}

void RgbLed::Fill(uint8_t r, uint8_t g, uint8_t b) {
    if (!strip_) return;
    for (int i = 0; i < count_; i++) {
        SetPixel(i, r, g, b);
    }
}

void RgbLed::Clear() {
    if (!strip_) return;
    led_strip_clear(strip_);
}

void RgbLed::Refresh() {
    if (!strip_) return;
    led_strip_refresh(strip_);
}
