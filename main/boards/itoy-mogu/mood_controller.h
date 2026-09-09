#ifndef MOOD_CONTROLLER_H_
#define MOOD_CONTROLLER_H_

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "touch_pad.h"
#include "motor_control.h"
#include "rgb_led.h"
#include "power_control.h"

class BehaviorEngine;   // 行为层 (前置声明; 电位器闭环模式由板级注入, 开环为 nullptr)

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
    MOOD_HEAD_TURN,      // 转头 (调试演示用: 点头+摇头联动扭脖)
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

    // engine: 行为层指针 (电位器闭环模式注入; 默认 nullptr = 开环旧手势路径)
    void Initialize(TouchPad* touch, MotorControl* motor, RgbLed* rgb, PowerControl* power,
                    BehaviorEngine* engine = nullptr);
    BehaviorEngine* engine() { return engine_; }   // 调试网页触发即时反应/idle 用
    void Start();                 // 启动状态机任务, 自动进入 POWER_ON
    void RequestPowerOff();       // 请求关机 (渐灭后断电)
    void SetNightLight(bool on);  // 夜灯开关 (app 调用)
    // 后端下发情绪 -> 请求切到某状态 (线程安全: 仅置标志, 由 mood 任务下一 tick 应用)
    void RequestExternalMood(MoodState s);
    // 调试网页命令 (生产调试模式: httpd 线程投递, mood 任务下一 tick 消费 —
    // 与 RequestExternalMood 同款标志路由, 不跨任务直碰状态机/engine。
    // 功能验证模式不 Start 本任务, 这些接口不被消费, 网页走直接调用路径)
    void PostDebugDemo(MoodState s);   // 强制重放某情绪入场 (可连点重抽变体)
    void PostDebugTouch(int8_t side);  // 模拟一次短摸 (side: -1左/+1右; 走完整事件链)
    void PostDebugIdle();              // 立即播一条 idle (需电位器闭环)
    // 调试用: 直接进入某状态, 重放入场 RGB + 手势; 不跑自动状态机/触摸轮询。
    // 绕过 ChangeState 的去重 (state_ 先置非法值), 故可重复触发同一情绪。网页调试用。
    void DemoState(MoodState s);

    MoodState state() const { return state_; }
    const char* state_name() const;    // 状态名 (调试网页显示)

private:
    static void TaskFunc(void* arg);
    void Loop();
    void ChangeState(MoodState s);
    void HandleEvent(MoodEvent ev);
    void PollTouch(uint32_t now_ms);
    void PollBattery(uint32_t now_ms);
    void OnShortTap(uint32_t now_ms);
    void PlayMoodGesture();   // 当前状态入场手势/行为 (闭环: engine; 开环: GestureXxx)

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
    void GestureHeadTurn();

    TouchPad* touch_ = nullptr;
    MotorControl* motor_ = nullptr;
    RgbLed* rgb_ = nullptr;
    PowerControl* power_ = nullptr;
    BehaviorEngine* engine_ = nullptr;

    int8_t last_side_ = 0;                     // 最近按下边沿的侧别 (-1 左/+1 右; 情绪摆动偏向)
    uint32_t comfort_breath_pending_ms_ = 0;    // COMFORT 渐变完成后切慢呼吸的时刻 (0=无)
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

    // 后端下发情绪的待处理请求 (网络线程写, mood 任务读+清)
    volatile bool pending_external_mood_ = false;
    MoodState external_mood_ = MOOD_CALM;

    // 调试网页命令的待处理请求 (httpd 线程写, mood 任务读+清; 生产调试模式用)
    volatile bool pending_dbg_demo_ = false;
    MoodState dbg_demo_state_ = MOOD_CALM;
    volatile bool pending_dbg_touch_ = false;
    int8_t dbg_touch_side_ = 0;
    volatile bool pending_dbg_idle_ = false;
    int8_t synth_side_ = 0;            // 模拟触摸侧 (-1左/+1右; 0 无) — 仅 mood 任务读写
    uint32_t synth_until_ms_ = 0;      // 模拟触摸有效期 (PollTouch 在期内并入该侧读数)
};

#endif // MOOD_CONTROLLER_H_
