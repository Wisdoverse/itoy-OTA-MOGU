#ifndef MOOD_CONTROLLER_H_
#define MOOD_CONTROLLER_H_

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "touch_pad.h"
#include "motor_control.h"
#include "rgb_led.h"
#include "power_control.h"

// 情绪状态 (按《情绪蘑菇》交互设计 md)
enum MoodState : uint8_t {
    MOOD_OFF = 0,
    MOOD_POWER_ON,
    MOOD_CALM,
    MOOD_HAPPY,
    MOOD_COMFORT,
    MOOD_DEEP_BREATH,
    MOOD_SLEEPY,
    MOOD_DISTURBED,
    MOOD_LOW_BATTERY,
    MOOD_NIGHT_LIGHT,
};

// 内部事件
enum MoodEvent : uint8_t {
    EVT_NONE = 0,
    EVT_POWER_ON,
    EVT_POWER_OFF,
    EVT_TOUCH_SHORT,       // 摸一下 (左/右短按)
    EVT_TOUCH_HOLD,        // 单手握 ≥3s
    EVT_TOUCH_BOTH_HOLD,   // 双手握 ≥5s
    EVT_TOUCH_END,         // 全部触摸结束
    EVT_NO_TOUCH_TIMEOUT,  // 10min 无互动
    EVT_RAPID_TOUCH,       // 5s 内频繁短按
    EVT_BATTERY_LOW,
    EVT_BATTERY_OK,
    EVT_NIGHT_LIGHT_TOGGLE,
};

// 情绪状态机: 100ms 任务轮询触摸/电池, 分类事件, 驱动状态转换,
// 各状态设置 RGB 效果 + 电机手势。
// 触摸分组: 左手 = GPIO1+2 (pad0||pad1), 右手 = GPIO3+4 (pad2||pad3)
class MoodController {
public:
    MoodController();
    ~MoodController();

    void Initialize(TouchPad* touch, MotorControl* motor, RgbLed* rgb, PowerControl* power);
    void Start();                 // 启动状态机任务, 自动进入 POWER_ON
    void RequestPowerOff();       // 请求关机 (渐灭后断电)
    void SetNightLight(bool on);  // 夜灯开关 (app 调用)

    MoodState state() const { return state_; }

private:
    static void TaskFunc(void* arg);
    void Loop();
    void ChangeState(MoodState s);
    void HandleEvent(MoodEvent ev);
    void PollTouch(uint32_t now_ms);
    void PollBattery(uint32_t now_ms);
    void OnShortTap(uint32_t now_ms);

    bool LeftPressed();
    bool RightPressed();

    // 状态入场的手势
    void GesturePowerOn();
    void GestureHappy();
    void GestureComfort();
    void GestureDeepBreath();
    void GestureSleepy();
    void GestureDisturbed();
    void GestureLowBattery();

    TouchPad* touch_ = nullptr;
    MotorControl* motor_ = nullptr;
    RgbLed* rgb_ = nullptr;
    PowerControl* power_ = nullptr;
    TaskHandle_t task_ = nullptr;

    MoodState state_ = MOOD_OFF;
    uint32_t state_enter_ms_ = 0;
    uint32_t tick_ms_ = 0;

    // 触摸跟踪
    bool prev_left_ = false, prev_right_ = false;
    uint32_t left_ms_ = 0, right_ms_ = 0, both_ms_ = 0;
    bool left_hold_fired_ = false, right_hold_fired_ = false, both_hold_fired_ = false;
    uint32_t last_touch_ms_ = 0;
    uint32_t short_count_ = 0;
    uint32_t short_window_ms_ = 0;
    uint32_t comfort_release_ms_ = 0;   // COMFORT 内触摸结束时刻

    // 电池
    uint32_t last_batt_ms_ = 0;
    bool low_batt_ = false;

    // 调试心跳
    uint32_t last_hb_ms_ = 0;

    // 关机流程
    bool off_requested_ = false;
};

#endif // MOOD_CONTROLLER_H_
