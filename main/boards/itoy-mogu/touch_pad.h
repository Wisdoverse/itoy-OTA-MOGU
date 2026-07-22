#ifndef TOUCH_PAD_H_
#define TOUCH_PAD_H_

#include <driver/touch_sensor.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <functional>
#include <array>

#include "config.h"

// 4 路 ESP32-S3 touch 通道 (触摸铜片)
// Touch1=GPIO1, Touch2=GPIO2, Touch3=GPIO3, Touch4=GPIO4 (GPIO0 非 touch)
static constexpr touch_pad_t kTouchChannels[TOUCH_PAD_COUNT] = {
    TOUCH_PAD_NUM1,  // GPIO1
    TOUCH_PAD_NUM2,  // GPIO2
    TOUCH_PAD_NUM3,  // GPIO3
    TOUCH_PAD_NUM4,  // GPIO4
};

class TouchPad {
public:
    using Callback = std::function<void(int channel, bool pressed)>;

    TouchPad();
    ~TouchPad();

    // 初始化 touch 子系统
    void Initialize();

    // 获取当前触摸值 (0~N, 值越小说明越接近被触摸)
    uint32_t GetRawValue(int channel) const;

    // 是否正在被触摸 (基于阈值)
    bool IsPressed(int channel) const;

    // 设置回调: channel 0~4, pressed=true 表示触摸按下
    void SetCallback(Callback cb) { callback_ = std::move(cb); }

    // 设置阈值 (小于此值认为被触摸)
    void SetThreshold(int channel, uint32_t threshold);
    uint32_t GetThreshold(int channel) const;

    // 校准: 采样空闲状态下的基准值, 阈值 = 基准值 * ratio
    void Calibrate(float ratio = 0.8f);

    // 启动周期性扫描任务
    void StartScanTask(int interval_ms = 30, int stack_size = 2048);

private:
    static void ScanTaskFunc(void* arg);
    void ScanOnce();

    Callback callback_;
    std::array<uint32_t, TOUCH_PAD_COUNT> thresholds_{};
    std::array<uint32_t, TOUCH_PAD_COUNT> baselines_{};
    bool initialized_ = false;
    TaskHandle_t task_handle_ = nullptr;
    int scan_interval_ms_ = 30;
    bool last_state_[TOUCH_PAD_COUNT] = {};
};

#endif // TOUCH_PAD_H_