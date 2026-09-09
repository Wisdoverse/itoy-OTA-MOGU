#ifndef RGB_LED_H_
#define RGB_LED_H_

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "config.h"

// RGB 效果类型
enum RgbEffect : uint8_t {
    RGB_OFF = 0,
    RGB_SOLID,   // 稳定色
    RGB_BREATH,  // 呼吸 (三角波, lo%<->hi%)
    RGB_FADE,    // 渐变到目标亮度后保持
};

// WS2812B 全彩灯带驱动 + 动画器
// DIN = GPIO38 (U13 连接器), 灯珠数 = CONFIG_ITOY_RGB_LED_COUNT
// 内部 10ms 任务持续驱动灯带; 调用 Solid/Breath/FadeTo/Off 切换效果
class RgbLed {
public:
    RgbLed();
    ~RgbLed();

    // 建 strip + 启动 10ms 动画任务, 默认熄灭
    void Initialize();

    // ---- 效果 API (各情绪状态调用) ----
    void Off();
    // 稳定色, bright_pct 0~100
    void Solid(uint8_t r, uint8_t g, uint8_t b, uint8_t bright_pct);
    // 呼吸, period_ms 周期, lo_pct/hi_pct 亮度范围
    void Breath(uint8_t r, uint8_t g, uint8_t b, uint32_t period_ms,
                uint8_t lo_pct, uint8_t hi_pct);
    // 呼吸 times 次后自动熄灭 (开机/关机提示); 阻塞 times*period_ms,
    // 播放期间调用方让出灯光控制权, 返回时灯已灭
    void BreathBurst(uint8_t r, uint8_t g, uint8_t b, uint32_t period_ms,
                     uint8_t lo_pct, uint8_t hi_pct, uint8_t times);
    // 渐变到 target_pct 亮度, 时长 dur_ms, 完成后保持
    void FadeTo(uint8_t r, uint8_t g, uint8_t b, uint8_t target_pct, uint32_t dur_ms);

    // 全局亮度上限 (0~100), 缩放所有效果输出 (限流/限亮)
    void SetBrightness(uint8_t cap_pct);
    uint8_t GetBrightness() const { return cap_pct_; }

    // 呼吸相位 (供身体起伏等外部联动): BREATH 态返回三角波相位 0~1
    // (谷=0/1.0 最暗, 峰=0.5 最亮, 时间基准与 ComputeBrightPct 完全一致, 不错拍);
    // 非 BREATH 态返回 -1。
    // 注: 各字段非原子快照 — 跨任务读最多差一个动画 tick (10ms);
    // Breath() 切换瞬间的一次采样可能混读新旧参数, 表现为单次采样偏差, 可接受
    float GetBreathPhase() const;

    int count() const { return count_; }

private:
    static void AnimTaskFunc(void* arg);
    void AnimLoop();
    uint8_t ComputeBrightPct();   // 当前 tick 各效果的目标亮度%

    led_strip_handle_t strip_ = nullptr;
    TaskHandle_t task_ = nullptr;
    int count_ = 0;

    RgbEffect effect_ = RGB_OFF;
    uint8_t r_ = 0, g_ = 0, b_ = 0;
    uint8_t cap_pct_ = 100;          // 全局亮度上限

    // breath / solid / fade 共用
    uint32_t period_ms_ = 4000;
    uint8_t lo_pct_ = 40, hi_pct_ = 60;
    uint32_t breath_start_tick_ = 0;   // 呼吸相位起点 (每次 Breath 从 lo 升起)
    uint8_t breath_times_ = 0;         // >0 = 呼吸 N 次后自动转 OFF (提示灯用)
    uint8_t cur_pct_ = 0;            // 当前保持亮度 (SOLID / FADE 终点)
    uint8_t fade_from_pct_ = 0;
    uint32_t fade_dur_ms_ = 2000;
    uint32_t fade_start_tick_ = 0;

    uint32_t tick_ = 0;              // 10ms 计数
    bool dirty_ = true;             // 是否需要重写 strip
};

#endif // RGB_LED_H_
