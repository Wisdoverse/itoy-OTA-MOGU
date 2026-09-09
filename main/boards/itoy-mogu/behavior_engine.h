#ifndef BEHAVIOR_ENGINE_H_
#define BEHAVIOR_ENGINE_H_

#include <stdint.h>
#include "config.h"

#if CONFIG_ITOY_HOME_POT

#include "motor_control.h"
#include "rgb_led.h"

// 情绪种类 — 行为层自持 (MoodController 负责 MoodState -> EmoKind 映射,
// 避免 behavior_engine.h <-> mood_controller.h 循环 include)
enum EmoKind : uint8_t {
    EMO_NONE = 0,
    EMO_POWER_ON,
    EMO_HAPPY,
    EMO_COMFORT,
    EMO_DEEP_BREATH,
    EMO_SLEEPY,
    EMO_DISTURBED,
    EMO_LOW_BATTERY,
    EMO_OFF,          // 关机: 停手势 + 回中
    EMO_NIGHT_LIGHT,  // 夜灯: 停手势静止 (静止是特性)
    EMO_HEAD_TURN,    // 调试演示: 扭脖
};

// 触摸侧 (LEFT=-1 / RIGHT=+1, 数值也用作摆动镜像系数)
enum TouchSide : int8_t {
    SIDE_NONE = 0,
    SIDE_LEFT = -1,
    SIDE_RIGHT = 1,
};

// 行为优先级: 新请求 > 当前 -> 打断重播; <= 当前 -> 丢弃
enum BehaviorPri : uint8_t {
    PRI_IDLE = 1,      // idle 小动作 / 呼吸起伏 (随时可被打断)
    PRI_REACTIVE = 2,  // 即时反应 (转向触摸侧+保持, 到松手)
    PRI_EMOTION = 3,   // 情绪入场 (一次性, 播完即止)
};

// 行为层: 持有全部身体动作脚本 (情绪变体 / idle 库 / 即时反应 / 呼吸起伏),
// 统一决定"现在放谁", 全部经 MotorControl::PlayPosGesture 执行。
// 无独立任务 — 由 MoodController 的 100ms 循环驱动 Tick()。
class BehaviorEngine {
public:
    // 脚本词汇 (公开: .cc 的静态脚本表要用)
    static constexpr int8_t POSE_KEEP = 127;   // 该轴本段不动 (正常范围 -100~100)

    // 脚本条目 (百分比表达, 播放时换算为电位器目标读数; 与 PosStep 分离,
    // 便于幅度缩放 / 侧向镜像 / "不动"哨兵独立于原始读数约定)
    struct PoseStep {
        int8_t nod;        // 低头+ / 仰头- (%)
        int8_t shake;      // 右+ / 左- (%); 脚本按"首摆向右"编写, side=左时整体镜像
        uint8_t cruise_ms; // 巡航每步延时
        uint8_t ease;      // 0=恒速 1=S 缓动
    };
    // 情绪变体: hold_nod > 0 = 末段保持该俯仰并接呼吸联动; 0 = 播完即止
    struct EmoVariant {
        const PoseStep* steps;
        uint8_t n;
        int8_t hold_nod;
    };

    void Initialize(MotorControl* motor, RgbLed* rgb);

    // 100ms 心跳: 优先级回落 / idle 调度 / idle 尾段回中 / 呼吸联动发段
    void Tick(uint32_t now_ms);

    // 情绪入场 (pri 3): 每次调用重新抽变体; side 让摆动偏向触摸侧。
    // 保持态情绪 (COMFORT/SLEEPY/LOW_BATTERY) 自动接呼吸联动 (灯须为 Breath)
    void PlayEmotion(EmoKind kind, TouchSide side = SIDE_NONE);

    // 即时反应 (pri 2): 按下边沿 -> 转向触摸侧保持; 第二只手 -> 看正前; 同侧幂等
    void OnTouchDown(TouchSide side);
    // 全部松开: 若无情绪接管 -> 慢速缓动回中
    void OnAllTouchUp();

    // CALM 进/出场: 开/关 idle 小动作 (进场先等半程随机间隔, 开机不乱动)
    void EnterCalm(uint32_t now_ms);
    void LeaveCalm();
    void ForceIdle();   // 调试网页: 立即播一条 idle

private:
    void PlayVariant(const char* name, const EmoVariant* v, int count, TouchSide side);
    void PlayPri(uint8_t pri, const char* label, TouchSide side,
                 const PosStep* steps, int n, bool auto_center);
    void PlayIdleAction(uint32_t now_ms);
    void ScheduleNextIdle(uint32_t now_ms);
    int PickIdleAction();
    void FireBreathSeg(bool exhale);
    PosStep MakeStep(const PoseStep& p, int side_sign, int amp_pct);

    MotorControl* motor_ = nullptr;
    RgbLed* rgb_ = nullptr;

    uint8_t cur_pri_ = 0;        // 当前播放优先级 (0=空闲)
    EmoKind kind_ = EMO_NONE;

    bool touching_ = false;
    TouchSide last_down_side_ = SIDE_NONE;

    // idle (仅 CALM)
    bool idle_on_ = false;
    uint32_t idle_next_ms_ = 0;

    // idle 尾段回中 ("转头看一边/张望" 停顿后补发)
    bool follow_up_ = false;
    uint32_t follow_up_ms_ = 0;
    PosStep follow_step_ = {};

    // 呼吸联动 (仅保持态)
    bool breath_follow_ = false;
    int breath_nod_base_ = 0;    // 保持俯仰基准 (%)
    int breath_amp_ = 10;        // 起伏幅度 (±%)
    float last_phase_ = -1.0f;
};

#endif  // CONFIG_ITOY_HOME_POT
#endif  // BEHAVIOR_ENGINE_H_
