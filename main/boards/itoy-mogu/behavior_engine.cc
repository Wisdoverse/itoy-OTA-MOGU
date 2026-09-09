#include "behavior_engine.h"
#if CONFIG_ITOY_HOME_POT

#include <esp_log.h>
#include <esp_random.h>
#include <stdio.h>

#define TAG "BEHAVIOR"

// ---- 速度 (巡航每步延时; 与 mood 的 SP_* 同值) ----
#define BEH_SP_SLOW   (MOTOR_STEP_DELAY_MS * 5)    // 10ms 慢 (起转稳/力矩大)
#define BEH_SP_NORMAL (MOTOR_STEP_DELAY_MS * 2)    // 4ms
#define BEH_SP_FAST   (MOTOR_STEP_DELAY_MS)        // 2ms
#define BEH_SP_TURN   (MOTOR_STEP_DELAY_MS + 1)    // 3ms (扭脖)

// ---- 呼吸联动 ----
#define BEH_BREATH_AMP_PCT  10    // 起伏幅度 (±% 行程)

// ---- 即时反应姿态 (% 行程; shake 由调用处乘 side 镜像) ----
#define REACT_NOD_PCT    (-15)    // 转向触摸侧时轻微仰头 (看你)
#define REACT_SHAKE_PCT  60       // 转向幅度
#define FRONT_NOD_PCT    (-10)    // 第二只手: 看正前

// ---- idle 性格档 (menuconfig ITOY_PERSONALITY) ----
#if defined(CONFIG_ITOY_PERSONALITY_QUIET)
#define IDLE_MIN_MS   60000
#define IDLE_MAX_MS   180000
#define IDLE_AMP_PCT  60
#define IDLE_LIVELY   0
#elif defined(CONFIG_ITOY_PERSONALITY_LIVELY)
#define IDLE_MIN_MS   10000
#define IDLE_MAX_MS   30000
#define IDLE_AMP_PCT  130
#define IDLE_LIVELY   1
#else   // 适中 (默认)
#define IDLE_MIN_MS   20000
#define IDLE_MAX_MS   60000
#define IDLE_AMP_PCT  100
#define IDLE_LIVELY   0
#endif

namespace {

// 目标读数 = 中位 + pct% × 该方向满行程 (点头>0=低头/摇头>0=右; 行程不对称各自算)
// (自 mood_controller.cc 迁入, Task 5 在原处删除)
int16_t NodPos(int pct) {
    int span = (pct >= 0) ? (POT_NOD_DOWN_RAW - POT_NOD_CENTER_RAW)
                          : (POT_NOD_CENTER_RAW - POT_NOD_UP_RAW);
    return (int16_t)(POT_NOD_CENTER_RAW + pct * span / 100);
}
int16_t ShakePos(int pct) {
    int span = (pct >= 0) ? (POT_SHAKE_RIGHT_RAW - POT_SHAKE_CENTER_RAW)
                          : (POT_SHAKE_CENTER_RAW - POT_SHAKE_LEFT_RAW);
    return (int16_t)(POT_SHAKE_CENTER_RAW + pct * span / 100);
}

const char* SideName(TouchSide s) {
    return s == SIDE_LEFT ? "left" : (s == SIDE_RIGHT ? "right" : "none");
}

// ============================================================
// 情绪变体脚本 (PoseStep{nod, shake, cruise, ease}; 127 = 该轴不动)
// 约定: 首摆向右编写, side=左时整体镜像; 回中型脚本末段显式 {0,0}
// ============================================================

// POWER_ON: 轻微仰头 -> 回中
const BehaviorEngine::PoseStep kPowerOn[] = {
    {-30, 127, BEH_SP_SLOW, 1},
    {0,   127, BEH_SP_SLOW, 1},
};
// HAPPY ①: 点头 -> 摆±65(偏 side) -> 回中
const BehaviorEngine::PoseStep kHappy1[] = {
    {30,  127, BEH_SP_NORMAL, 1},
    {127, 65,  BEH_SP_NORMAL, 1},
    {127, -65, BEH_SP_NORMAL, 1},
    {0,   0,   BEH_SP_NORMAL, 1},
};
// HAPPY ②: 双点头 -> 单侧摆 -> 回中
const BehaviorEngine::PoseStep kHappy2[] = {
    {30,  127, BEH_SP_NORMAL, 1},
    {0,   127, BEH_SP_NORMAL, 1},
    {30,  127, BEH_SP_NORMAL, 1},
    {0,   127, BEH_SP_NORMAL, 1},
    {127, 65,  BEH_SP_NORMAL, 1},
    {0,   0,   BEH_SP_NORMAL, 1},
};
// HAPPY ③: 仰头 -> 摆 -> 回中
const BehaviorEngine::PoseStep kHappy3[] = {
    {-25, 127, BEH_SP_NORMAL, 1},
    {127, 65,  BEH_SP_NORMAL, 1},
    {127, -65, BEH_SP_NORMAL, 1},
    {0,   0,   BEH_SP_NORMAL, 1},
};
// DISTURBED ①: 快低头55 + 甩±70 -> 回中
const BehaviorEngine::PoseStep kDisturbed1[] = {
    {55,  127, BEH_SP_FAST, 1},
    {127, -70, BEH_SP_FAST, 1},
    {127, 70,  BEH_SP_FAST, 1},
    {0,    0,  BEH_SP_NORMAL, 1},
};
// DISTURBED ②: 快甩±75 -> 仰头-20 -> 回中
const BehaviorEngine::PoseStep kDisturbed2[] = {
    {127, -75, BEH_SP_FAST, 1},
    {127, 75,  BEH_SP_FAST, 1},
    {-20, 127, BEH_SP_NORMAL, 1},
    {0,   0,   BEH_SP_NORMAL, 1},
};
// DISTURBED ③: 缩(快低头40) -> 猛回中
const BehaviorEngine::PoseStep kDisturbed3[] = {
    {40, 127, BEH_SP_FAST, 1},
    {0,  0,  BEH_SP_NORMAL, 1},
};
// COMFORT (保持, 接呼吸联动): ①前倾35 ②前倾25+微偏 side 15
const BehaviorEngine::PoseStep kComfort1[] = { {35, 127, BEH_SP_SLOW, 1} };
const BehaviorEngine::PoseStep kComfort2[] = { {25, 15,  BEH_SP_SLOW, 1} };
// SLEEPY (保持, 联动即打盹微动): ①低头45 ②低头35+微偏随机侧10
const BehaviorEngine::PoseStep kSleepy1[] = { {45, 127, BEH_SP_SLOW, 1} };
const BehaviorEngine::PoseStep kSleepy2[] = { {35, 10,  BEH_SP_SLOW, 1} };
// DEEP_BREATH: 前倾/回中 x2 (本身已是呼吸动作, 不叠联动)
const BehaviorEngine::PoseStep kDeepBreath[] = {
    {45,  127, BEH_SP_SLOW, 1},
    {0,   127, BEH_SP_SLOW, 1},
    {45,  127, BEH_SP_SLOW, 1},
    {0,   127, BEH_SP_SLOW, 1},
};
// LOW_BATTERY (保持, 接呼吸联动)
const BehaviorEngine::PoseStep kLowBattery[] = { {20, 127, BEH_SP_SLOW, 1} };
// HEAD_TURN (调试演示): 低头+摇头对角往返
const BehaviorEngine::PoseStep kHeadTurn[] = {
    {30,  -70, BEH_SP_TURN, 1},
    {-30, 70,  BEH_SP_TURN, 1},
    {30,  -70, BEH_SP_TURN, 1},
    {0,   0,   BEH_SP_TURN, 1},
};

// idle 动作库 (与 kIdleWeights 对应; 幅度经 IDLE_AMP_PCT 缩放, 侧向随机)
// 0 转头看一边(停1s回中) 1 轻点头 2 换姿势(保持) 3 抬头张望(停0.3s回中) 4 摇头晃脑(仅活泼)
const BehaviorEngine::PoseStep kIdle0[] = { {127, 35, BEH_SP_NORMAL, 1} };
const BehaviorEngine::PoseStep kIdle1[] = {
    {20,  127, BEH_SP_NORMAL, 1},
    {0,   127, BEH_SP_NORMAL, 1},
};
const BehaviorEngine::PoseStep kIdle2[] = { {15, 20, BEH_SP_SLOW, 1} };
const BehaviorEngine::PoseStep kIdle3[] = { {-20, 25, BEH_SP_NORMAL, 1} };
const BehaviorEngine::PoseStep kIdle4[] = {
    {127, -30, BEH_SP_NORMAL, 1},
    {127, 30,  BEH_SP_NORMAL, 1},
    {0,   0,   BEH_SP_NORMAL, 1},
};
const uint8_t kIdleWeights[] = {3, 2, 1, 1, 2};

}  // namespace

void BehaviorEngine::Initialize(MotorControl* motor, RgbLed* rgb) {
    motor_ = motor;
    rgb_ = rgb;
    const char* persona = "moderate";
#if defined(CONFIG_ITOY_PERSONALITY_QUIET)
    persona = "quiet";
#elif defined(CONFIG_ITOY_PERSONALITY_LIVELY)
    persona = "lively";
#endif
    ESP_LOGI(TAG, "init (personality=%s, idle %d~%ds, amp x%d%%)",
             persona, IDLE_MIN_MS / 1000, IDLE_MAX_MS / 1000, IDLE_AMP_PCT);
}

// 百分比脚本条目 -> 闭环位置步 (幅度缩放 + 侧向镜像 + POSE_KEEP 哨兵)
PosStep BehaviorEngine::MakeStep(const PoseStep& p, int side_sign, int amp_pct) {
    auto clamp100 = [](int v) { return v < -100 ? -100 : (v > 100 ? 100 : v); };
    PosStep s = {};
    s.nod_target = (p.nod == POSE_KEEP) ? -1
                   : NodPos(clamp100(p.nod * amp_pct / 100));
    s.shake_target = (p.shake == POSE_KEEP) ? -1
                     : ShakePos(clamp100(p.shake * side_sign * amp_pct / 100));
    s.cruise_delay_ms = p.cruise_ms;
    s.ease = p.ease;
    return s;
}

// 优先级调度: 新请求 > 当前 -> 播 (PlayPosGesture 内部自会取消一切在播动作);
// <= 当前 -> 丢弃。REACTIVE 同侧幂等在 OnTouchDown 处理。
void BehaviorEngine::PlayPri(uint8_t pri, const char* label, TouchSide side,
                             const PosStep* steps, int n, bool auto_center) {
    if (pri <= cur_pri_ && cur_pri_ != 0) {
        ESP_LOGI(TAG, "drop pri=%d kind=%s (cur pri=%d)", pri, label, cur_pri_);
        return;
    }
    motor_->PlayPosGesture(steps, n, auto_center);
    cur_pri_ = pri;
    ESP_LOGI(TAG, "play pri=%d kind=%s dir=%s ease=%d",
             pri, label, SideName(side), n > 0 ? (int)steps[0].ease : 0);
}

void BehaviorEngine::PlayEmotion(EmoKind kind, TouchSide side) {
    kind_ = kind;
    breath_follow_ = false;   // 新情绪清旧联动; 保持态变体在 PlayVariant 里再设
    follow_up_ = false;
    switch (kind) {
        case EMO_OFF:
            motor_->StopGesture();
            motor_->Home();
            cur_pri_ = 0;
            ESP_LOGI(TAG, "play kind=off (stop + home)");
            return;
        case EMO_NIGHT_LIGHT:
            motor_->StopGesture();
            cur_pri_ = 0;
            ESP_LOGI(TAG, "play kind=night_light (stop, 静止)");
            return;
        default:
            break;
    }
    if (kind == EMO_SLEEPY && side == SIDE_NONE)
        side = (esp_random() & 1) ? SIDE_LEFT : SIDE_RIGHT;   // 困倦变体② 微偏随机侧
    switch (kind) {
        case EMO_POWER_ON: {
            static const EmoVariant v[] = {{kPowerOn, 2, 0}};
            PlayVariant("power_on", v, 1, side);
            break;
        }
        case EMO_HAPPY: {
            static const EmoVariant v[] = {{kHappy1, 4, 0}, {kHappy2, 6, 0}, {kHappy3, 4, 0}};
            PlayVariant("happy", v, 3, side);
            break;
        }
        case EMO_DISTURBED: {
            static const EmoVariant v[] = {
                {kDisturbed1, 4, 0}, {kDisturbed2, 4, 0}, {kDisturbed3, 2, 0}};
            PlayVariant("disturbed", v, 3, side);
            break;
        }
        case EMO_COMFORT: {
            static const EmoVariant v[] = {{kComfort1, 1, 35}, {kComfort2, 1, 25}};
            PlayVariant("comfort", v, 2, side);
            break;
        }
        case EMO_SLEEPY: {
            static const EmoVariant v[] = {{kSleepy1, 1, 45}, {kSleepy2, 1, 35}};
            PlayVariant("sleepy", v, 2, side);
            break;
        }
        case EMO_DEEP_BREATH: {
            static const EmoVariant v[] = {{kDeepBreath, 4, 0}};
            PlayVariant("deep_breath", v, 1, side);
            break;
        }
        case EMO_LOW_BATTERY: {
            static const EmoVariant v[] = {{kLowBattery, 1, 20}};
            PlayVariant("low_battery", v, 1, side);
            break;
        }
        case EMO_HEAD_TURN: {
            static const EmoVariant v[] = {{kHeadTurn, 4, 0}};
            PlayVariant("head_turn", v, 1, side);
            break;
        }
        default:
            break;
    }
}

// 抽变体 -> 转换 -> 播放; hold_nod>0 的保持态自动接呼吸联动
void BehaviorEngine::PlayVariant(const char* name, const EmoVariant* v, int count,
                                 TouchSide side) {
    int idx = (int)(esp_random() % (uint32_t)count);
    const EmoVariant& var = v[idx];
    PosStep out[MAX_POS_STEPS];
    for (int i = 0; i < var.n; i++)
        out[i] = MakeStep(var.steps[i], (side == SIDE_LEFT) ? -1 : 1, 100);
    char label[40];
    snprintf(label, sizeof(label), "emotion/%s#%d", name, idx + 1);
    PlayPri(PRI_EMOTION, label, side, out, var.n, /*auto_center=*/false);
    if (var.hold_nod > 0) {
        breath_follow_ = true;
        breath_nod_base_ = var.hold_nod;
        breath_amp_ = BEH_BREATH_AMP_PCT;
        last_phase_ = -1.0f;
        ESP_LOGI(TAG, "breath follow on: base=%d%% amp=%d%%", breath_nod_base_, breath_amp_);
    }
}

void BehaviorEngine::OnTouchDown(TouchSide side) {
    if (side == SIDE_NONE) return;
    if (cur_pri_ > PRI_REACTIVE) return;   // 情绪播放中不打断 (几秒内会被新情绪接管)
    bool repeat = touching_ && side == last_down_side_;   // 同侧重复边沿 (触摸抖动) -> 幂等
    bool second = touching_ && !repeat;                   // 第二只手 -> 看正前
    touching_ = true;
    follow_up_ = false;
    if (repeat) return;
    last_down_side_ = side;
    PosStep g[1];
    g[0] = second
               ? MakeStep({FRONT_NOD_PCT, 0, BEH_SP_FAST, 1}, 1, 100)
               : MakeStep({REACT_NOD_PCT, (int8_t)(REACT_SHAKE_PCT * side), BEH_SP_FAST, 1}, 1, 100);
    PlayPri(PRI_REACTIVE, second ? "reactive/front" : "reactive/turn", side, g, 1,
            /*auto_center=*/false);
}

void BehaviorEngine::OnAllTouchUp() {
    touching_ = false;
    follow_up_ = false;
    last_down_side_ = SIDE_NONE;
    if (cur_pri_ == PRI_REACTIVE) {
        cur_pri_ = 0;          // 无情绪接管 -> 慢速缓动回中 (HomeTick 固定慢速)
        motor_->Home();
        ESP_LOGI(TAG, "release -> home");
    }
}

void BehaviorEngine::EnterCalm(uint32_t now_ms) {
    idle_on_ = true;
    // 进场先等半程随机间隔再开始 (开机不乱动)
    uint32_t half = (IDLE_MIN_MS + esp_random() % (IDLE_MAX_MS - IDLE_MIN_MS)) / 2;
    idle_next_ms_ = now_ms + half;
    ESP_LOGI(TAG, "calm idle on (first in ~%lus)", (unsigned long)(half / 1000));
}

void BehaviorEngine::LeaveCalm() {
    if (!idle_on_) return;
    idle_on_ = false;
    follow_up_ = false;
    ESP_LOGI(TAG, "calm idle off");
}

void BehaviorEngine::ForceIdle() {
    if (cur_pri_ != 0) {
        ESP_LOGI(TAG, "force idle skipped (busy pri=%d)", cur_pri_);
        return;
    }
    PlayIdleAction(0);
}

void BehaviorEngine::ScheduleNextIdle(uint32_t now_ms) {
    idle_next_ms_ = now_ms + IDLE_MIN_MS + esp_random() % (IDLE_MAX_MS - IDLE_MIN_MS);
}

int BehaviorEngine::PickIdleAction() {
    int total = 0;
    for (int i = 0; i < (int)(sizeof(kIdleWeights) / sizeof(kIdleWeights[0])); i++)
        if (i != 4 || IDLE_LIVELY) total += kIdleWeights[i];
    uint32_t r = esp_random() % (uint32_t)total;
    for (int i = 0; i < 5; i++) {
        if (i == 4 && !IDLE_LIVELY) continue;
        if ((int)r < kIdleWeights[i]) return i;
        r -= kIdleWeights[i];
    }
    return 0;
}

void BehaviorEngine::PlayIdleAction(uint32_t now_ms) {
    int act = PickIdleAction();
    int side_sign = (esp_random() & 1) ? 1 : -1;   // 随机侧
    PosStep out[MAX_POS_STEPS];
    int n = 0;
    const char* name = "idle/?";
    switch (act) {
        case 0:   // 转头看一边 -> (Tick 定时) 停 1s 回中
            out[n++] = MakeStep(kIdle0[0], side_sign, IDLE_AMP_PCT);
            follow_step_ = MakeStep({0, 127, BEH_SP_NORMAL, 1}, 1, 100);
            follow_up_ = true;
            follow_up_ms_ = now_ms + 1000;
            name = "idle/look";
            break;
        case 1:   // 轻点头 -> 回中
            out[n++] = MakeStep(kIdle1[0], 1, IDLE_AMP_PCT);
            out[n++] = MakeStep(kIdle1[1], 1, IDLE_AMP_PCT);
            name = "idle/nod";
            break;
        case 2:   // 换姿势: 保持, 不回中
            out[n++] = MakeStep(kIdle2[0], side_sign, IDLE_AMP_PCT);
            name = "idle/shift";
            break;
        case 3:   // 抬头张望 -> 停 0.3s 回中
            out[n++] = MakeStep(kIdle3[0], side_sign, IDLE_AMP_PCT);
            follow_step_ = MakeStep({0, 0, BEH_SP_NORMAL, 1}, 1, 100);
            follow_up_ = true;
            follow_up_ms_ = now_ms + 300;
            name = "idle/gaze";
            break;
        case 4:   // 摇头晃脑 (仅活泼)
            for (int i = 0; i < 3; i++) out[n++] = MakeStep(kIdle4[i], 1, IDLE_AMP_PCT);
            name = "idle/wiggle";
            break;
    }
    PlayPri(PRI_IDLE, name, SIDE_NONE, out, n, /*auto_center=*/false);
}

void BehaviorEngine::FireBreathSeg(bool exhale) {
    int pct = exhale ? (breath_nod_base_ + breath_amp_)   // 呼气: 低下
                     : (breath_nod_base_ - breath_amp_);  // 吸气: 仰起 (赶在灯峰前到位)
    PosStep g[1];
    g[0] = MakeStep({(int8_t)pct, 127, BEH_SP_SLOW, 1}, 1, 100);
    PlayPri(PRI_IDLE, exhale ? "breath/exhale" : "breath/inhale", SIDE_NONE, g, 1,
            /*auto_center=*/false);
}

void BehaviorEngine::Tick(uint32_t now_ms) {
    // 一次性动作 (情绪/低优先级) 播完 -> 优先级回落 (REACTIVE 保持到松手, 不回落)
    if ((cur_pri_ == PRI_EMOTION || cur_pri_ == PRI_IDLE) && motor_->IsGestureDone())
        cur_pri_ = 0;

    // idle 尾段回中 ("转头看一边/张望" 停顿后; 被触摸/情绪打断则作废)
    if (follow_up_ && cur_pri_ == 0 && now_ms >= follow_up_ms_) {
        follow_up_ = false;
        PosStep g[1] = {follow_step_};
        PlayPri(PRI_IDLE, "idle/center", SIDE_NONE, g, 1, /*auto_center=*/false);
    }

    // idle 小动作 (仅 CALM, 没在摸, 空闲)
    if (idle_on_ && !touching_ && cur_pri_ == 0 && !follow_up_ && now_ms >= idle_next_ms_) {
        PlayIdleAction(now_ms);
        ScheduleNextIdle(now_ms);
    }

    // 呼吸联动 (仅保持态): 相位直接取自灯 (无漂移) —
    // 过谷 (相位回绕 1.0->0, 灯最暗, 吸气起) -> 仰 (base-amp);
    // 过峰 (上行过 0.5, 灯最亮, 呼气起) -> 低 (base+amp)。每段走完断电。
    if (breath_follow_) {
        float ph = rgb_ ? rgb_->GetBreathPhase() : -1.0f;
        if (ph >= 0.0f && last_phase_ >= 0.0f) {
            bool cross_trough = last_phase_ > ph;
            bool cross_peak = last_phase_ < 0.5f && ph >= 0.5f;
            if (cur_pri_ == 0 && (cross_trough || cross_peak))
                FireBreathSeg(cross_peak);
        }
        last_phase_ = ph;
    }
}

#endif  // CONFIG_ITOY_HOME_POT
