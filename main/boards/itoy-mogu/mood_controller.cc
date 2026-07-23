#include "mood_controller.h"
#include "config.h"
#include <esp_log.h>

#define TAG "Mood"

// ---- 可调参数 (md 给范围, 取中值; 实测后微调) ----
// 暖光基色 (md 未给具体 RGB, 仅"暖光")
#define WARM_R 255
#define WARM_G 180
#define WARM_B 90
#define WARM WARM_R, WARM_G, WARM_B

#define CALM_BRIGHT_PCT   50      // 平静基础亮度

// 触摸时长阈值 (ms)
#define SHORT_TAP_MAX_MS  1000    // <1s 视为短按
#define HOLD_ONE_MS       3000    // 单手 ≥3s
#define HOLD_BOTH_MS      5000    // 双手 ≥5s
#define NO_TOUCH_SLEEP_MS 600000  // 10min 无互动 -> 困倦
#define RAPID_WINDOW_MS   5000    // 频繁触摸窗口
#define RAPID_COUNT       3       // 窗口内短按达此次数 -> 受扰

// 状态超时 (ms)
#define T_POWER_ON     3000
#define T_HAPPY        4000
#define T_DEEP_BREATH  45000
#define T_DISTURBED    7000
#define T_COMFORT_REL  3000   // 安抚触摸结束后返回
#define T_OFF_FADE     3000   // 关机渐灭后断电

// 电池
#define BATT_LOW_PCT      15
#define BATT_RECOVER_PCT  15
#define BATT_CHECK_MS     5000

// 手势幅度 (步数) / 速度 (每步延时 ms = MOTOR_STEP_DELAY_MS 倍数)
#define G_NOD_OPEN     140
#define G_NOD_HAPPY    90
#define G_WIG_HAPPY    70
#define G_TILT_COMFORT 60
#define G_TILT_SLEEP   45
#define G_TILT_LOWBAT  35
#define G_BREATH_AMP   70
#define G_DISTURB_AMP  55
#define SP_SLOW   (MOTOR_STEP_DELAY_MS * 5)
#define SP_NORMAL (MOTOR_STEP_DELAY_MS * 2)
#define SP_FAST   (MOTOR_STEP_DELAY_MS)

static const char* StateName(MoodState s) {
    switch (s) {
        case MOOD_OFF: return "OFF";
        case MOOD_POWER_ON: return "POWER_ON";
        case MOOD_CALM: return "CALM";
        case MOOD_HAPPY: return "HAPPY";
        case MOOD_COMFORT: return "COMFORT";
        case MOOD_DEEP_BREATH: return "DEEP_BREATH";
        case MOOD_SLEEPY: return "SLEEPY";
        case MOOD_DISTURBED: return "DISTURBED";
        case MOOD_LOW_BATTERY: return "LOW_BATTERY";
        case MOOD_NIGHT_LIGHT: return "NIGHT_LIGHT";
        default: return "?";
    }
}

MoodController::MoodController() {}
MoodController::~MoodController() {
    if (task_) { vTaskDelete(task_); task_ = nullptr; }
}

void MoodController::Initialize(TouchPad* touch, MotorControl* motor,
                                RgbLed* rgb, PowerControl* power) {
    touch_ = touch;
    motor_ = motor;
    rgb_ = rgb;
    power_ = power;
}

void MoodController::Start() {
    if (task_) return;
    tick_ms_ = 0;
    last_touch_ms_ = 0;
    rgb_->Off();
    HandleEvent(EVT_POWER_ON);   // 进入开机过渡
    xTaskCreate(TaskFunc, "mood", 3072, this, 5, &task_);
}

void MoodController::RequestPowerOff() {
    HandleEvent(EVT_POWER_OFF);
}

void MoodController::SetNightLight(bool on) {
    HandleEvent(on ? EVT_NIGHT_LIGHT_TOGGLE : EVT_NIGHT_LIGHT_TOGGLE);
    // 退出夜灯也用同一事件 (在 NIGHT_LIGHT 态下切换回 CALM)
}

void MoodController::TaskFunc(void* arg) {
    static_cast<MoodController*>(arg)->Loop();
}

void MoodController::Loop() {
    while (true) {
        tick_ms_ += 100;

        PollTouch(tick_ms_);
        PollBattery(tick_ms_);

        uint32_t in_state = tick_ms_ - state_enter_ms_;

        switch (state_) {
            case MOOD_POWER_ON:
                if (in_state >= T_POWER_ON) ChangeState(MOOD_CALM);
                break;
            case MOOD_CALM:
                if (tick_ms_ - last_touch_ms_ >= NO_TOUCH_SLEEP_MS) {
                    ChangeState(MOOD_SLEEPY);
                }
                break;
            case MOOD_HAPPY:
                if (in_state >= T_HAPPY) ChangeState(MOOD_CALM);
                break;
            case MOOD_COMFORT:
                if (comfort_release_ms_ && tick_ms_ - comfort_release_ms_ >= T_COMFORT_REL) {
                    ChangeState(MOOD_CALM);
                }
                break;
            case MOOD_DEEP_BREATH:
                if (in_state >= T_DEEP_BREATH) ChangeState(MOOD_CALM);
                break;
            case MOOD_DISTURBED:
                if (in_state >= T_DISTURBED) ChangeState(MOOD_CALM);
                break;
            case MOOD_OFF:
                if (!off_requested_ && in_state >= T_OFF_FADE) {
                    off_requested_ = true;
                    if (power_) power_->RequestShutdown();
                }
                break;
            default:
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ---- 触摸 ----
bool MoodController::LeftPressed() {
    return touch_ && (touch_->IsPressed(0) || touch_->IsPressed(1));
}
bool MoodController::RightPressed() {
    return touch_ && (touch_->IsPressed(2) || touch_->IsPressed(3));
}

void MoodController::OnShortTap(uint32_t now_ms) {
    // 5s 窗口内短按计数 -> 频繁触摸
    if (now_ms - short_window_ms_ > RAPID_WINDOW_MS) {
        short_count_ = 0;
        short_window_ms_ = now_ms;
    }
    short_count_++;
    if (short_count_ >= RAPID_COUNT) {
        short_count_ = 0;
        short_window_ms_ = now_ms;
        HandleEvent(EVT_RAPID_TOUCH);
    } else {
        HandleEvent(EVT_TOUCH_SHORT);
    }
}

void MoodController::PollTouch(uint32_t now_ms) {
    bool l = LeftPressed();
    bool r = RightPressed();
    bool both = l && r;
    bool any = l || r;

    // 按下边沿
    if (l && !prev_left_) { left_ms_ = now_ms; left_hold_fired_ = false; last_touch_ms_ = now_ms; }
    if (r && !prev_right_) { right_ms_ = now_ms; right_hold_fired_ = false; last_touch_ms_ = now_ms; }
    if (both && both_ms_ == 0) both_ms_ = now_ms;
    if (!both) { both_ms_ = 0; both_hold_fired_ = false; }

    // 单手长按 ≥3s (双手时不触发单手)
    if (state_ == MOOD_CALM || state_ == MOOD_SLEEPY) {
        if (l && !both && !left_hold_fired_ && now_ms - left_ms_ >= HOLD_ONE_MS) {
            left_hold_fired_ = true;
            HandleEvent(EVT_TOUCH_HOLD);
        }
        if (r && !both && !right_hold_fired_ && now_ms - right_ms_ >= HOLD_ONE_MS) {
            right_hold_fired_ = true;
            HandleEvent(EVT_TOUCH_HOLD);
        }
        // 双手 ≥5s
        if (both && !both_hold_fired_ && both_ms_ && now_ms - both_ms_ >= HOLD_BOTH_MS) {
            both_hold_fired_ = true;
            HandleEvent(EVT_TOUCH_BOTH_HOLD);
        }
    }

    // 松开边沿: 短按
    if (!l && prev_left_) {
        if (!left_hold_fired_ && now_ms - left_ms_ < SHORT_TAP_MAX_MS) OnShortTap(now_ms);
    }
    if (!r && prev_right_) {
        if (!right_hold_fired_ && now_ms - right_ms_ < SHORT_TAP_MAX_MS) OnShortTap(now_ms);
    }

    // 全部触摸结束
    if (!any && (prev_left_ || prev_right_)) {
        HandleEvent(EVT_TOUCH_END);
    }

    prev_left_ = l;
    prev_right_ = r;
}

void MoodController::PollBattery(uint32_t now_ms) {
    if (!power_ || now_ms - last_batt_ms_ < BATT_CHECK_MS) return;
    last_batt_ms_ = now_ms;
    int mv = power_->ReadBatteryMv();
    if (mv < 1000) return;   // ADC 未就绪/未校准, 跳过
    int pct = power_->ReadBatteryPercent();
    if (!low_batt_ && pct < BATT_LOW_PCT) {
        low_batt_ = true;
        HandleEvent(EVT_BATTERY_LOW);
    } else if (low_batt_ && pct >= BATT_RECOVER_PCT) {
        low_batt_ = false;
        HandleEvent(EVT_BATTERY_OK);
    }
}

// ---- 状态转换 (按 md 状态转换表) ----
void MoodController::HandleEvent(MoodEvent ev) {
    // 关机 / 低电抢占
    if (ev == EVT_POWER_OFF) { ChangeState(MOOD_OFF); return; }
    if (ev == EVT_BATTERY_LOW && state_ != MOOD_OFF && state_ != MOOD_LOW_BATTERY) {
        ChangeState(MOOD_LOW_BATTERY);
        return;
    }

    switch (state_) {
        case MOOD_OFF:
            if (ev == EVT_POWER_ON) ChangeState(MOOD_POWER_ON);
            break;
        case MOOD_POWER_ON:
            break;   // 自动转 CALM (超时)
        case MOOD_CALM:
            if (ev == EVT_TOUCH_SHORT) ChangeState(MOOD_HAPPY);
            else if (ev == EVT_TOUCH_HOLD) ChangeState(MOOD_COMFORT);
            else if (ev == EVT_TOUCH_BOTH_HOLD) ChangeState(MOOD_DEEP_BREATH);
            else if (ev == EVT_RAPID_TOUCH) ChangeState(MOOD_DISTURBED);
            else if (ev == EVT_NIGHT_LIGHT_TOGGLE) ChangeState(MOOD_NIGHT_LIGHT);
            else if (ev == EVT_BATTERY_OK) { /* no-op */ }
            break;
        case MOOD_HAPPY:
            break;   // 超时回 CALM
        case MOOD_COMFORT:
            if (ev == EVT_TOUCH_END) comfort_release_ms_ = tick_ms_;
            break;
        case MOOD_DEEP_BREATH:
            break;   // 超时回 CALM
        case MOOD_SLEEPY:
            if (ev == EVT_TOUCH_SHORT || ev == EVT_TOUCH_HOLD || ev == EVT_TOUCH_BOTH_HOLD) {
                ChangeState(MOOD_HAPPY);
            }
            break;
        case MOOD_DISTURBED:
            break;   // 冷却超时回 CALM
        case MOOD_LOW_BATTERY:
            if (ev == EVT_BATTERY_OK) ChangeState(MOOD_CALM);
            break;
        case MOOD_NIGHT_LIGHT:
            if (ev == EVT_NIGHT_LIGHT_TOGGLE) ChangeState(MOOD_CALM);
            break;
    }
}

// ---- 状态入场 (RGB + 电机手势) ----
void MoodController::ChangeState(MoodState s) {
    if (s == state_) return;
    state_ = s;
    state_enter_ms_ = tick_ms_;
    comfort_release_ms_ = 0;
    ESP_LOGI(TAG, "state -> %s", StateName(s));

    switch (s) {
        case MOOD_OFF:
            rgb_->FadeTo(WARM, 0, T_OFF_FADE);
            motor_->Home();
            off_requested_ = false;
            break;
        case MOOD_POWER_ON:
            rgb_->FadeTo(WARM, CALM_BRIGHT_PCT, T_POWER_ON);
            GesturePowerOn();
            break;
        case MOOD_CALM:
            rgb_->Breath(WARM, 3500, CALM_BRIGHT_PCT - 5, CALM_BRIGHT_PCT + 5);
            motor_->Home();
            break;
        case MOOD_HAPPY:
            rgb_->Solid(WARM, CALM_BRIGHT_PCT + 15);
            GestureHappy();
            break;
        case MOOD_COMFORT:
            rgb_->FadeTo(WARM, CALM_BRIGHT_PCT * 75 / 100, 2500);
            GestureComfort();
            break;
        case MOOD_DEEP_BREATH:
            rgb_->Breath(WARM, 7000, 30, 100);
            GestureDeepBreath();
            break;
        case MOOD_SLEEPY:
            rgb_->Breath(WARM, 6000, 20, 40);
            GestureSleepy();
            break;
        case MOOD_DISTURBED:
            rgb_->Breath(WARM, 400, 10, 90);
            GestureDisturbed();
            break;
        case MOOD_LOW_BATTERY:
            rgb_->Breath(WARM, 4500, 5, 25);
            GestureLowBattery();
            break;
        case MOOD_NIGHT_LIGHT:
            rgb_->Solid(WARM, 15);
            motor_->StopGesture();
            break;
    }
}

// ---- 手势脚本 ----
void MoodController::GesturePowerOn() {
    // 从低头位 -> 中立, 轻微仰头再回
    GestureStep g[] = {
        {MOTOR_NOD, G_NOD_OPEN, SP_SLOW},
        {MOTOR_NOD, -20, SP_SLOW},
    };
    motor_->PlayGesture(g, 2);
}

void MoodController::GestureHappy() {
    // 轻微仰头 + 左右摆 1~2 次
    GestureStep g[] = {
        {MOTOR_NOD, G_NOD_HAPPY, SP_NORMAL},
        {MOTOR_NOD, -G_NOD_HAPPY, SP_NORMAL},
        {MOTOR_SHAKE, G_WIG_HAPPY, SP_NORMAL},
        {MOTOR_SHAKE, -G_WIG_HAPPY, SP_NORMAL},
        {MOTOR_SHAKE, G_WIG_HAPPY, SP_NORMAL},
        {MOTOR_SHAKE, -G_WIG_HAPPY, SP_NORMAL},
    };
    motor_->PlayGesture(g, 6);
}

void MoodController::GestureComfort() {
    // 缓慢前倾 (倾听姿态), 保持
    GestureStep g[] = { {MOTOR_NOD, G_TILT_COMFORT, SP_SLOW} };
    motor_->PlayGesture(g, 1);
}

void MoodController::GestureDeepBreath() {
    // 极缓慢前倾 -> 回中, 2 次循环 (与呼吸灯近似同步)
    GestureStep g[] = {
        {MOTOR_NOD, G_BREATH_AMP, SP_SLOW},
        {MOTOR_NOD, -G_BREATH_AMP, SP_SLOW},
        {MOTOR_NOD, G_BREATH_AMP, SP_SLOW},
        {MOTOR_NOD, -G_BREATH_AMP, SP_SLOW},
    };
    motor_->PlayGesture(g, 4);
}

void MoodController::GestureSleepy() {
    GestureStep g[] = { {MOTOR_NOD, G_TILT_SLEEP, SP_SLOW} };
    motor_->PlayGesture(g, 1);
}

void MoodController::GestureDisturbed() {
    // 快速低头 + 歪头
    GestureStep g[] = {
        {MOTOR_NOD, -G_DISTURB_AMP, SP_FAST},
        {MOTOR_SHAKE, G_DISTURB_AMP, SP_FAST},
        {MOTOR_SHAKE, -G_DISTURB_AMP, SP_FAST},
    };
    motor_->PlayGesture(g, 3);
}

void MoodController::GestureLowBattery() {
    GestureStep g[] = { {MOTOR_NOD, G_TILT_LOWBAT, SP_SLOW} };
    motor_->PlayGesture(g, 1);
}
