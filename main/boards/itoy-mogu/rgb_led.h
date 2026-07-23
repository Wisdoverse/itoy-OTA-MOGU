#ifndef RGB_LED_H_
#define RGB_LED_H_

#include <stdint.h>
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "config.h"

// WS2812B 全彩灯带驱动
// DIN 接 GPIO38 (U13 连接器), 灯珠数由 Kconfig CONFIG_ITOY_RGB_LED_COUNT 配置
class RgbLed {
public:
    RgbLed();
    ~RgbLed();

    // 初始化 RMT + 灯带 (灯珠数取自 CONFIG_ITOY_RGB_LED_COUNT), 默认熄灭
    void Initialize();

    // 设置单个灯珠颜色 (index 0~count-1; r/g/b 0~255), 需 Refresh 才生效
    void SetPixel(int index, uint8_t r, uint8_t g, uint8_t b);
    // 全部灯珠同色
    void Fill(uint8_t r, uint8_t g, uint8_t b);
    // 全部熄灭
    void Clear();
    // 把缓冲推送到灯带 (设置颜色后必须调用才点亮)
    void Refresh();

    // 全局亮度 (0~255), 影响 SetPixel/Fill 的实际输出
    void SetBrightness(uint8_t b);
    uint8_t GetBrightness() const { return brightness_; }

    int count() const { return count_; }

private:
    led_strip_handle_t strip_ = nullptr;
    int count_ = 0;
    uint8_t brightness_ = 255;
    bool initialized_ = false;
};

#endif // RGB_LED_H_
