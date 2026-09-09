#include "mood_controller.h"
#include "behavior_engine.h"   // 开环模式下为空文件 (自带 POT 守卫)
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

// 安抚慢呼吸 (呼吸联动前提: 入场先 FadeTo 到下限, 完成后切 Breath — 见 Loop)
#define COMFORT_BREATH_PERIOD_MS  5000
#define COMFORT_BREATH_LO_PCT     25
#define COMFORT_BREATH_HI_PCT     45

// 触摸时长阈值 (ms)
#define SHORT_TAP_MAX_MS  1000    // <1s 视为短按
#define HOLD_ONE_MS       3000    // 单手 ≥3s
#define HOLD_BOTH_MS      5000    // 双手 ≥5s
#define NO_TOUCH_SLEEP_MS 600000  // 10min 无互动 -> 困倦
#define AUTO_OFF_MS       300000  // 困倦中再无互动 5min -> 自动关机 (距末次互动共 15min)
#define RAPID_WINDOW_MS   5000    // 频繁触摸窗口
#define RAPID_COUNT       3       // 窗口内短按达此次数 -> 受扰

// 状态超时 (ms)
#define T_POWER_ON     3000
#define T_HAPPY        4000
#define T_DEEP_BREATH  45000
#define T_DISTURBED    7000
#define T_COMFORT_REL  3000   // 安抚触摸结束后返回
#define T_OFF_FADE     3000   // 关机渐灭后断电

// 电池 (BATT_LOW_PCT 挪到 config.h, 与开机/关机提示灯共用)
#define BATT_RECOVER_PCT  15
#define BATT_CHECK_MS     5000

// 手势幅度 (度, 按 md 范围取值) — 运行时经 STEPS_FOR_DEG 换算为步数
#define G_DEG_NOD_OPEN     8       // 开机: 轻微仰头
#define G_DEG_NOD_HAPPY    5       // 开心: 轻微点头
#define G_DEG_WIG_HAPPY    12      // 开心: 左右摆 (md ±10~15°)
#define G_DEG_TILT_COMFORT 8       // 安抚: 前倾 (md 5~10°)
#define G_DEG_TILT_SLEEP   8       // 困倦: 低头 (md 5~10°)
#define G_DEG_TILT_LOWBAT  5       // 低电: 轻微低头
#define G_DEG_BREATH       6       // 深呼吸: 前倾幅度
#define G_DEG_DISTURB      9       // 受扰: (md 8~10°)
// 转头 (扭脖): 摇头为主、点头联动。半扫幅 = 中位->边的角度。
// 注意: 幅度受电位器软限位制约 (中位±约 8~12° 内安全)。过大 -> "超出物理行程" / 撞限位动作错乱。
#define G_DEG_TURN_SHAKE   8       // 转头: 左右半扫幅 (度) — 绝对位置 ±此值, 不要超过 ~12
#define G_DEG_TURN_NOD     4       // 转头: 点头联动幅度 (度)

// 速度 (每步延时 ms = MOTOR_STEP_DELAY_MS 倍数)
#define SP_SLOW   (MOTOR_STEP_DELAY_MS * 5)
#define SP_NORMAL (MOTOR_STEP_DELAY_MS * 2)
#define SP_FAST   (MOTOR_STEP_DELAY_MS)
#define SP_TURN   (MOTOR_STEP_DELAY_MS + 1)   // 转头: 3ms/步 (比 FAST 略慢, 平稳起转)

// 调试: 心跳打印间隔 (ms), 0 = 关闭
#define DBG_HEARTBEAT_MS   5000
// 调试网页模拟触摸时长 (须 < SHORT_TAP_MAX_MS: 松开按短按分类 -> HAPPY 链路;
// 1s~3s 之间是既非短按也非长按的死区, 别取)
#define DBG_SIM_TOUCH_MS   800

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
        case MOOD_HEAD_TURN: return "HEAD_TURN";
        default: return "?";
    }
}

static const char* EventName(MoodEvent e) {
    switch (e) {
        case EVT_NONE: return "NONE";
        case EVT_POWER_ON: return "POWER_ON";
        case EVT_POWER_OFF: return "POWER_OFF";
        case EVT_TOUCH_SHORT: return "TOUCH_SHORT";
        case EVT_TOUCH_HOLD: return "TOUCH_HOLD";
        case EVT_TOUCH_BOTH_HOLD: return "TOUCH_BOTH_HOLD";
        case EVT_TOUCH_END: return "TOUCH_END";
        case EVT_NO_TOUCH_TIMEOUT: return "NO_TOUCH_TIMEOUT";
        case EVT_RAPID_TOUCH: return "RAPID_TOUCH";
        case EVT_BATTERY_LOW: return "BATT_LOW";
        case EVT_BATTERY_OK: return "BATT_OK";
        case EVT_NIGHT_LIGHT_TOGGLE: return "NIGHT_LIGHT";
        default: return "?";
    }
}

MoodController::MoodController() {}
MoodController::~MoodController() {
    if (task_) { vTaskDelete(task_); task_ = nullptr; }
}

void MoodController::Initialize(TouchPad* touch, MotorControl* motor,
                                RgbLed* rgb, PowerControl* power,
                                BehaviorEngine* engine) {
    touch_ = touch;
    motor_ = motor;
    rgb_ = rgb;
    power_ = power;
    engine_ = engine;
}

void MoodController::Start() {
    if (task_) return;
    tick_ms_ = 0;
    last_touch_ms_ = 0;
    rgb_->Off();
    HandleEvent(EVT_POWER_ON);   // 进入开机过渡
    xTaskCreate(TaskFunc, "mood", 4096, this, 5, &task_);
}

void MoodController::RequestPowerOff() {
    HandleEvent(EVT_POWER_OFF);
}

void MoodController::SetNightLight(bool on) {
    HandleEvent(on ? EVT_NIGHT_LIGHT_TOGGLE : EVT_NIGHT_LIGHT_TOGGLE);
    // 退出夜灯也用同一事件 (在 NIGHT_LIGHT 态下切换回 CALM)
}

void MoodController::RequestExternalMood(MoodState s) {
    // 先写值再置标志, mood 任务下一 tick (≤100ms) 读到后应用. 关机态不打断.
    last_touch_ms_ = tick_ms_;   // 远程互动也算"在用", 不触发无互动自动关机
    external_mood_ = s;
    pending_external_mood_ = true;
    ESP_LOGI(TAG, "external mood requested: %s", StateName(s));
}

void MoodController::DemoState(MoodState s) {
    // 调试演示: 不依赖状态机任务, 直接重放某状态的入场效果 (RGB + 电机手势)。
    // 先把 state_ 置成一个非法值, 使 ChangeState 的 `if (s==state_) return` 不生效,
    // 这样连续点同一个情绪也能每次重放。
    ESP_LOGI(TAG, "demo -> %s", StateName(s));
    state_ = static_cast<MoodState>(0xFF);
    ChangeState(s);
}

void MoodController::PostDebugDemo(MoodState s) {
    // 先写值再置标志 (同 RequestExternalMood); 网页互动也算"在用",
    // 不触发无互动自动关机
    last_touch_ms_ = tick_ms_;
    dbg_demo_state_ = s;
    pending_dbg_demo_ = true;
}

void MoodController::PostDebugTouch(int8_t side) {
    last_touch_ms_ = tick_ms_;
    dbg_touch_side_ = side;
    pending_dbg_touch_ = true;
}

void MoodController::PostDebugIdle() {
    last_touch_ms_ = tick_ms_;
    pending_dbg_idle_ = true;
}

const char* MoodController::state_name() const {
    return StateName(state_);
}

void MoodController::TaskFunc(void* arg) {
    static_cast<MoodController*>(arg)->Loop();
}

void MoodController::Loop() {
    while (true) {
        tick_ms_ += 100;

        // 调试网页命令 (生产调试模式: httpd 线程投递, 统一在 mood 任务执行 —
        // 避免跨任务直碰状态机/engine; 功能验证模式不 Start 本任务, 标志恒空)
        if (pending_dbg_demo_) {
            pending_dbg_demo_ = false;
            DemoState(dbg_demo_state_);
        }
        if (pending_dbg_touch_) {
            pending_dbg_touch_ = false;
            synth_side_ = dbg_touch_side_;
            synth_until_ms_ = tick_ms_ + DBG_SIM_TOUCH_MS;
            ESP_LOGI(TAG, "sim touch %s for %dms", synth_side_ < 0 ? "LEFT" : "RIGHT",
                     DBG_SIM_TOUCH_MS);
        }
        if (pending_dbg_idle_) {
            pending_dbg_idle_ = false;
#if CONFIG_ITOY_HOME_POT
            if (engine_) engine_->ForceIdle();
#else
            ESP_LOGI(TAG, "dbg idle: 需电位器闭环模式");
#endif
        }

        PollTouch(tick_ms_);
        PollBattery(tick_ms_);

#if CONFIG_ITOY_HOME_POT
        if (engine_) engine_->Tick(tick_ms_);   // 行为层心跳 (优先级回落/idle/呼吸联动)
#endif

        // COMFORT: 入场渐变完成后切慢呼吸 (呼吸联动前提; 时刻在 ChangeState 设定)
        if (comfort_breath_pending_ms_ && tick_ms_ >= comfort_breath_pending_ms_) {
            comfort_breath_pending_ms_ = 0;
            rgb_->Breath(WARM, COMFORT_BREATH_PERIOD_MS, COMFORT_BREATH_LO_PCT,
                         COMFORT_BREATH_HI_PCT);
        }

        // 后端下发情绪: 应用待处理请求 (关机态不打断)
        if (pending_external_mood_) {
            MoodState req = external_mood_;
            pending_external_mood_ = false;
            if (state_ != MOOD_OFF && req != state_) {
                ChangeState(req);
            }
        }

        uint32_t in_state = tick_ms_ - state_enter_ms_;

        switch (state_) {
            case MOOD_POWER_ON:
                if (in_state >= T_POWER_ON) { ESP_LOGI(TAG, "timeout POWER_ON done"); ChangeState(MOOD_CALM); }
                break;
            case MOOD_CALM:
                if (tick_ms_ - last_touch_ms_ >= NO_TOUCH_SLEEP_MS) {
                    ESP_LOGI(TAG, "timeout no-touch 10min -> SLEEPY");
                    ChangeState(MOOD_SLEEPY);
                }
                break;
            case MOOD_SLEEPY:
                // 困倦中再无互动 -> 自动关机 (走 MOOD_OFF: 渐灭+回中+断电)
                if (tick_ms_ - last_touch_ms_ >= AUTO_OFF_MS) {
                    ESP_LOGI(TAG, "timeout sleepy no-touch 5min -> AUTO OFF");
                    ChangeState(MOOD_OFF);
                }
                break;
            case MOOD_HAPPY:
                if (in_state >= T_HAPPY) { ESP_LOGI(TAG, "timeout HAPPY"); ChangeState(MOOD_CALM); }
                break;
            case MOOD_COMFORT:
                if (comfort_release_ms_ && tick_ms_ - comfort_release_ms_ >= T_COMFORT_REL) {
                    ESP_LOGI(TAG, "timeout COMFORT release"); ChangeState(MOOD_CALM);
                }
                break;
            case MOOD_DEEP_BREATH:
                if (in_state >= T_DEEP_BREATH) { ESP_LOGI(TAG, "timeout DEEP_BREATH"); ChangeState(MOOD_CALM); }
                break;
            case MOOD_DISTURBED:
                if (in_state >= T_DISTURBED) { ESP_LOGI(TAG, "timeout DISTURBED cooldown"); ChangeState(MOOD_CALM); }
                break;
            case MOOD_OFF:
                if (!off_requested_ && in_state >= T_OFF_FADE) {
                    off_requested_ = true;
                    ESP_LOGI(TAG, "OFF fade done -> power off");
                    if (power_) power_->RequestShutdown();
                }
                break;
            default:
                break;
        }

        // 调试心跳: 周期打印状态/触摸/电位器, 便于串口观察
#if DBG_HEARTBEAT_MS > 0
        if (tick_ms_ - last_hb_ms_ >= DBG_HEARTBEAT_MS) {
            last_hb_ms_ = tick_ms_;
            ESP_LOGI(TAG, "hb state=%s touch[L=%d R=%d] pot[nod=%lu shake=%lu]",
                     StateName(state_), LeftPressed(), RightPressed(),
                     motor_ ? motor_->ReadNodPosition() : 0,
                     motor_ ? motor_->ReadShakePosition() : 0);
        }
#endif

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
        ESP_LOGI(TAG, "short tap x%lu -> RAPID", (unsigned long)short_count_);
        short_count_ = 0;
        short_window_ms_ = now_ms;
        HandleEvent(EVT_RAPID_TOUCH);
    } else {
        ESP_LOGI(TAG, "short tap (5s count=%lu)", (unsigned long)short_count_);
        HandleEvent(EVT_TOUCH_SHORT);
    }
}

void MoodController::PollTouch(uint32_t now_ms) {
    bool l = LeftPressed();
    bool r = RightPressed();
    // 生产调试: 网页模拟触摸 — 有效期内并入对应侧读数, 从按下边沿到松开
    // 走完整事件链 (即时反应/长按分类/短按计数), 与真手指无差别
    if (synth_until_ms_ && now_ms < synth_until_ms_) {
        if (synth_side_ < 0) l = true;
        else                 r = true;
    }
    bool both = l && r;
    bool any = l || r;

#if CONFIG_ITOY_HOME_POT
    // 静止态不喂按下边沿: 夜灯"静止是特性", OFF(3s 后断电)/开机入场被摸会
    // 突发转头。松手边沿照常转发 — 它只会结束动作、不发起动作, touching_
    // 也不会粘滞到下一个状态
    bool react_ok = engine_ && state_ != MOOD_OFF && state_ != MOOD_POWER_ON &&
                    state_ != MOOD_NIGHT_LIGHT;
#endif
    // 按下边沿
    if (l && !prev_left_) {
        left_ms_ = now_ms; left_hold_fired_ = false; last_touch_ms_ = now_ms;
        ESP_LOGI(TAG, "touch LEFT down");
        last_side_ = -1;
#if CONFIG_ITOY_HOME_POT
        if (react_ok) engine_->OnTouchDown(SIDE_LEFT);   // 即时反应: 转向左侧保持
#endif
    }
    if (r && !prev_right_) {
        right_ms_ = now_ms; right_hold_fired_ = false; last_touch_ms_ = now_ms;
        ESP_LOGI(TAG, "touch RIGHT down");
        last_side_ = 1;
#if CONFIG_ITOY_HOME_POT
        if (react_ok) engine_->OnTouchDown(SIDE_RIGHT);   // 即时反应: 转向右侧保持
#endif
    }
    if (both && both_ms_ == 0) both_ms_ = now_ms;
    if (!both) { both_ms_ = 0; both_hold_fired_ = false; }

    // 单手长按 ≥3s (双手时不触发单手)
    if (state_ == MOOD_CALM || state_ == MOOD_SLEEPY) {
        if (l && !both && !left_hold_fired_ && now_ms - left_ms_ >= HOLD_ONE_MS) {
            left_hold_fired_ = true;
            ESP_LOGI(TAG, "touch LEFT hold >=3s");
            HandleEvent(EVT_TOUCH_HOLD);
        }
        if (r && !both && !right_hold_fired_ && now_ms - right_ms_ >= HOLD_ONE_MS) {
            right_hold_fired_ = true;
            ESP_LOGI(TAG, "touch RIGHT hold >=3s");
            HandleEvent(EVT_TOUCH_HOLD);
        }
        // 双手 ≥5s
        if (both && !both_hold_fired_ && both_ms_ && now_ms - both_ms_ >= HOLD_BOTH_MS) {
            both_hold_fired_ = true;
            ESP_LOGI(TAG, "touch BOTH hold >=5s");
            HandleEvent(EVT_TOUCH_BOTH_HOLD);
        }
    }

    // 松开边沿: 短按
    if (!l && prev_left_) {
        uint32_t dur = now_ms - left_ms_;
        ESP_LOGI(TAG, "touch LEFT up (dur=%lums)", (unsigned long)dur);
        if (!left_hold_fired_ && dur < SHORT_TAP_MAX_MS) OnShortTap(now_ms);
    }
    if (!r && prev_right_) {
        uint32_t dur = now_ms - right_ms_;
        ESP_LOGI(TAG, "touch RIGHT up (dur=%lums)", (unsigned long)dur);
        if (!right_hold_fired_ && dur < SHORT_TAP_MAX_MS) OnShortTap(now_ms);
    }

    // 全部触摸结束
    if (!any && (prev_left_ || prev_right_)) {
        HandleEvent(EVT_TOUCH_END);
#if CONFIG_ITOY_HOME_POT
        if (engine_) engine_->OnAllTouchUp();   // 无情绪接管 -> 缓动回中
#endif
    }

    prev_left_ = l;
    prev_right_ = r;
}

void MoodController::PollBattery(uint32_t now_ms) {
    if (!power_ || now_ms - last_batt_ms_ < BATT_CHECK_MS) return;
    last_batt_ms_ = now_ms;
    int mv = power_->ReadBatteryMv();
    if (mv < 1000) { ESP_LOGW(TAG, "batt ADC not ready (mv=%d), skip", mv); return; }
    int pct = power_->ReadBatteryPercent();
    ESP_LOGI(TAG, "batt: %dmV = %d%%", mv, pct);
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
    ESP_LOGI(TAG, "evt %s @ %s", EventName(ev), StateName(state_));

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
        case MOOD_HEAD_TURN:
            break;   // 调试演示态: 不响应触摸事件, 仅由网页调试切换
    }
}

// ---- 状态入场 (RGB + 手势/行为) ----
void MoodController::ChangeState(MoodState s) {
    if (s == state_) return;
    MoodState prev = state_;
    state_ = s;
    state_enter_ms_ = tick_ms_;
    comfort_release_ms_ = 0;
    comfort_breath_pending_ms_ = 0;
    ESP_LOGI(TAG, "state: %s -> %s", StateName(prev), StateName(s));

#if CONFIG_ITOY_HOME_POT
    if (prev == MOOD_CALM && engine_) engine_->LeaveCalm();   // 离开 CALM 关 idle
#endif

    switch (s) {
        case MOOD_OFF:
            rgb_->FadeTo(WARM, 0, T_OFF_FADE);
#if CONFIG_ITOY_HOME_POT
            if (engine_) engine_->PlayEmotion(EMO_OFF);       // 停手势 + 回中
#else
            motor_->Home();
#endif
            off_requested_ = false;
            break;
        case MOOD_POWER_ON:
            rgb_->FadeTo(WARM, CALM_BRIGHT_PCT, T_POWER_ON);
            PlayMoodGesture();
            break;
        case MOOD_CALM:
            rgb_->Breath(WARM, 3500, CALM_BRIGHT_PCT - 5, CALM_BRIGHT_PCT + 5);
            motor_->Home();
#if CONFIG_ITOY_HOME_POT
            if (engine_) engine_->EnterCalm(tick_ms_);        // 开 idle (先等半程间隔)
#endif
            break;
        case MOOD_HAPPY:
            rgb_->Solid(WARM, CALM_BRIGHT_PCT + 15);
            PlayMoodGesture();
            break;
        case MOOD_COMFORT:
#if CONFIG_ITOY_HOME_POT
            // 慢呼吸 (呼吸联动前提): 先渐变到下限, 完成后切 Breath (见 Loop)
            rgb_->FadeTo(WARM, COMFORT_BREATH_LO_PCT, 800);
            comfort_breath_pending_ms_ = tick_ms_ + 800;
#else
            rgb_->FadeTo(WARM, CALM_BRIGHT_PCT * 75 / 100, 2500);
#endif
            PlayMoodGesture();
            break;
        case MOOD_DEEP_BREATH:
            rgb_->Breath(WARM, 7000, 30, 100);
            PlayMoodGesture();
            break;
        case MOOD_SLEEPY:
            rgb_->Breath(WARM, 6000, 20, 40);
            PlayMoodGesture();   // 保持态: engine 自动接呼吸联动 = 打盹微动
            break;
        case MOOD_DISTURBED:
            rgb_->Breath(WARM, 400, 10, 90);
            PlayMoodGesture();
            break;
        case MOOD_LOW_BATTERY:
            rgb_->Breath(WARM, 4500, 5, 25);
            PlayMoodGesture();   // 保持态: engine 自动接呼吸联动
            break;
        case MOOD_NIGHT_LIGHT:
            rgb_->Solid(WARM, 15);
#if CONFIG_ITOY_HOME_POT
            if (engine_) engine_->PlayEmotion(EMO_NIGHT_LIGHT);   // 停手势, 静止
#else
            motor_->StopGesture();
#endif
            break;
        case MOOD_HEAD_TURN:
            rgb_->Solid(WARM, CALM_BRIGHT_PCT);
            PlayMoodGesture();
            break;
    }
}

// ---- 手势/行为调度 ----
// 电位器闭环 (ITOY_HOME_POT): 入场动作全部经 BehaviorEngine — 情绪变体随机抽选、
//   S 缓动、摆动偏向触摸侧、保持态 (安抚/困倦/低电) 自动接呼吸联动。
//   脚本本体与幅度参数都在 behavior_engine.cc。
// 开环 (默认): 原步数脚本 (GestureXxx, 幅度=度经 STEPS_FOR_DEG 换算), 行为不变。
void MoodController::PlayMoodGesture() {
#if CONFIG_ITOY_HOME_POT
    if (!engine_) return;
    TouchSide side = last_side_ < 0 ? SIDE_LEFT : (last_side_ > 0 ? SIDE_RIGHT : SIDE_NONE);
    EmoKind k = EMO_NONE;
    switch (state_) {
        case MOOD_POWER_ON:    k = EMO_POWER_ON; break;
        case MOOD_HAPPY:       k = EMO_HAPPY; break;
        case MOOD_COMFORT:     k = EMO_COMFORT; break;
        case MOOD_DEEP_BREATH: k = EMO_DEEP_BREATH; break;
        case MOOD_SLEEPY:      k = EMO_SLEEPY; break;
        case MOOD_DISTURBED:   k = EMO_DISTURBED; break;
        case MOOD_LOW_BATTERY: k = EMO_LOW_BATTERY; break;
        case MOOD_HEAD_TURN:   k = EMO_HEAD_TURN; break;
        default: return;
    }
    engine_->PlayEmotion(k, side);
#else
    switch (state_) {
        case MOOD_POWER_ON:    GesturePowerOn(); break;
        case MOOD_HAPPY:       GestureHappy(); break;
        case MOOD_COMFORT:     GestureComfort(); break;
        case MOOD_DEEP_BREATH: GestureDeepBreath(); break;
        case MOOD_SLEEPY:      GestureSleepy(); break;
        case MOOD_DISTURBED:   GestureDisturbed(); break;
        case MOOD_LOW_BATTERY: GestureLowBattery(); break;
        case MOOD_HEAD_TURN:   GestureHeadTurn(); break;
        default: break;
    }
#endif
}

// ---- 开环手势实现 (步数脚本; 电位器闭环模式编译但不会被调用) ----

void MoodController::GesturePowerOn() {
    // 从低头位 -> 中立, 轻微仰头再回
    ESP_LOGI(TAG, "gesture POWER_ON: nod +%d/-2 deg", G_DEG_NOD_OPEN);
    int16_t up = (int16_t)STEPS_FOR_DEG(G_DEG_NOD_OPEN);
    int16_t dn = (int16_t)STEPS_FOR_DEG(2);
    GestureStep g[] = {
        {MOTOR_NOD, up, SP_SLOW},
        {MOTOR_NOD, (int16_t)(-dn), SP_SLOW},
    };
    motor_->PlayGesture(g, 2);
}

void MoodController::GestureHappy() {
    // 轻微仰头 + 左右摆 1 次 (±12°)
    ESP_LOGI(TAG, "gesture HAPPY: nod ±%d deg, wiggle ±%d deg", G_DEG_NOD_HAPPY, G_DEG_WIG_HAPPY);
    int16_t nod = (int16_t)STEPS_FOR_DEG(G_DEG_NOD_HAPPY);
    int16_t wig = (int16_t)STEPS_FOR_DEG(G_DEG_WIG_HAPPY);
    GestureStep g[] = {
        {MOTOR_NOD, nod, SP_NORMAL},
        {MOTOR_NOD, (int16_t)(-nod), SP_NORMAL},
        {MOTOR_SHAKE, wig, SP_NORMAL},
        {MOTOR_SHAKE, (int16_t)(-wig), SP_NORMAL},
    };
    motor_->PlayGesture(g, 4);
}

void MoodController::GestureComfort() {
    // 缓慢前倾 (倾听姿态), 保持
    ESP_LOGI(TAG, "gesture COMFORT: tilt +%d deg", G_DEG_TILT_COMFORT);
    GestureStep g[] = { {MOTOR_NOD, (int16_t)STEPS_FOR_DEG(G_DEG_TILT_COMFORT), SP_SLOW} };
    motor_->PlayGesture(g, 1);
}

void MoodController::GestureDeepBreath() {
    // 极缓慢前倾 -> 回中, 2 次循环 (与呼吸灯近似同步)
    ESP_LOGI(TAG, "gesture DEEP_BREATH: ±%d deg x2", G_DEG_BREATH);
    int16_t amp = (int16_t)STEPS_FOR_DEG(G_DEG_BREATH);
    GestureStep g[] = {
        {MOTOR_NOD, amp, SP_SLOW},
        {MOTOR_NOD, (int16_t)(-amp), SP_SLOW},
        {MOTOR_NOD, amp, SP_SLOW},
        {MOTOR_NOD, (int16_t)(-amp), SP_SLOW},
    };
    motor_->PlayGesture(g, 4);
}

void MoodController::GestureSleepy() {
    ESP_LOGI(TAG, "gesture SLEEPY: tilt +%d deg", G_DEG_TILT_SLEEP);
    GestureStep g[] = { {MOTOR_NOD, (int16_t)STEPS_FOR_DEG(G_DEG_TILT_SLEEP), SP_SLOW} };
    motor_->PlayGesture(g, 1);
}

void MoodController::GestureDisturbed() {
    // 快速低头 + 歪头
    ESP_LOGI(TAG, "gesture DISTURBED: nod -%d, wiggle ±%d deg", G_DEG_DISTURB, G_DEG_DISTURB);
    int16_t amp = (int16_t)STEPS_FOR_DEG(G_DEG_DISTURB);
    GestureStep g[] = {
        {MOTOR_NOD, (int16_t)(-amp), SP_FAST},
        {MOTOR_SHAKE, amp, SP_FAST},
        {MOTOR_SHAKE, (int16_t)(-amp), SP_FAST},
    };
    motor_->PlayGesture(g, 3);
}

void MoodController::GestureLowBattery() {
    ESP_LOGI(TAG, "gesture LOW_BATT: tilt +%d deg", G_DEG_TILT_LOWBAT);
    GestureStep g[] = { {MOTOR_NOD, (int16_t)STEPS_FOR_DEG(G_DEG_TILT_LOWBAT), SP_SLOW} };
    motor_->PlayGesture(g, 1);
}

void MoodController::GestureHeadTurn() {
    // 扭脖子: 点头+摇头联动, 看起来像人扭脖。
    // 约定 (与其它手势一致): nod + = 抬头/仰, - = 低头; shake + = 右, - = 左。
    // 轨迹 (nod, shake) 对角往返, 起止于中位:
    //   蓄势 -> (低头,左); 左->右 先低头后抬头; 右->左 先仰头后低头; 收势 -> 回中。
    // 各段增量和为 0, 动作结束后头回到起始 (中) 位。
    ESP_LOGI(TAG, "gesture HEAD_TURN: shake ±%d deg, nod ±%d deg", G_DEG_TURN_SHAKE, G_DEG_TURN_NOD);
    int16_t A = (int16_t)STEPS_FOR_DEG(G_DEG_TURN_SHAKE);   // 半扫幅 (中->边)
    int16_t N = (int16_t)STEPS_FOR_DEG(G_DEG_TURN_NOD);     // 点头幅
    DualGestureStep g[] = {
        { (int16_t)(-N),      (int16_t)(-A),      SP_TURN },  // 蓄势 -> 左 + 低头
        { (int16_t)(2 * N),   (int16_t)(2 * A),   SP_TURN },  // 左->右: 下->上 (先低后抬)
        { (int16_t)(-2 * N),  (int16_t)(-2 * A),  SP_TURN },  // 右->左: 上->下 (先仰后低)
        { (int16_t)(N),       (int16_t)(A),       SP_TURN },  // 收势 -> 回中
    };
    motor_->PlayDualGesture(g, 4, /*auto_home=*/true);   // 结束后自动回零位并断电
}
