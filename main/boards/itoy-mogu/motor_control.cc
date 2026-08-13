#include "motor_control.h"
#include "config.h"
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <cmath>
#include <cstdlib>

#define TAG "Motor"

// ============================================================
// StepperMotor
// ============================================================

StepperMotor::StepperMotor(gpio_num_t a, gpio_num_t b, gpio_num_t c, gpio_num_t d,
                           adc_oneshot_unit_handle_t adc_handle, adc_channel_t adc_channel,
                           uint32_t pot_min, uint32_t pot_max, bool pot_cw_inc)
    : adc_handle_(adc_handle), adc_channel_(adc_channel),
      pot_min_(pot_min), pot_max_(pot_max), pot_cw_inc_(pot_cw_inc),
      step_delay_ms_(MOTOR_STEP_DELAY_MS) {
    pins_[0] = a;
    pins_[1] = b;
    pins_[2] = c;
    pins_[3] = d;
}

StepperMotor::~StepperMotor() {
    Stop();
}

void StepperMotor::Initialize() {
    if (initialized_) return;

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pin_bit_mask = 0;
    for (int i = 0; i < 4; i++) {
        io_conf.pin_bit_mask |= (1ULL << pins_[i]);
    }
    gpio_config(&io_conf);

    Stop();
    initialized_ = true;
    ESP_LOGI(TAG, "Stepper motor init (pins %d,%d,%d,%d, ADC ch%d, pot[%lu,%lu])",
             pins_[0], pins_[1], pins_[2], pins_[3], (int)adc_channel_,
             (unsigned long)pot_min_, (unsigned long)pot_max_);
}

void StepperMotor::SetPhase(uint8_t phase) {
    for (int i = 0; i < 4; i++) {
        gpio_set_level(pins_[pin_order_[i]], kHalfStepSeq[phase][i]);
    }
}

void StepperMotor::SetPinOrder(uint8_t o0, uint8_t o1, uint8_t o2, uint8_t o3) {
    pin_order_[0] = o0;
    pin_order_[1] = o1;
    pin_order_[2] = o2;
    pin_order_[3] = o3;
}

void StepperMotor::AdvancePhase(bool clockwise) {
    if (clockwise) {
        current_phase_ = (current_phase_ + 1) & 7;
    } else {
        current_phase_ = (current_phase_ - 1) & 7;
    }
    SetPhase(current_phase_);
}

void StepperMotor::Stop() {
    for (int i = 0; i < 4; i++) {
        gpio_set_level(pins_[i], 0);
    }
}

uint32_t StepperMotor::ReadPotentiometer() const {
    if (!adc_handle_) return 0;
    int raw = 0;
    adc_oneshot_read(adc_handle_, adc_channel_, &raw);
    return (uint32_t)raw;
}

float StepperMotor::ReadPosition() const {
    return (float)ReadPotentiometer() / (float)POT_MAX_VALUE;
}

bool StepperMotor::CanStep(bool clockwise) const {
#if MOTOR_HAS_POT
    uint32_t pot = ReadPotentiometer();
    // 该方向是否趋向电位器上限
    bool toward_max = (clockwise == pot_cw_inc_);
    if (toward_max) {
        return pot < pot_max_;
    }
    return pot > pot_min_;
#else
    (void)clockwise;
    return true;   // 电位器未接入: 不做软限位, 靠步数累计 + 幅度参数约束行程
#endif
}

bool StepperMotor::StepOnceLimited(bool clockwise) {
    if (!CanStep(clockwise)) {
        Stop();   // 到限位, 断电防堵转发热
        return false;
    }
    AdvancePhase(clockwise);
    return true;
}

void StepperMotor::Step(int steps) {
    bool clockwise = steps >= 0;
    int abs_steps = std::abs(steps);
    for (int i = 0; i < abs_steps; i++) {
        if (!StepOnceLimited(clockwise)) break;   // 软限位保护
        vTaskDelay(pdMS_TO_TICKS(step_delay_ms_));
    }
}

void StepperMotor::RotateDegrees(float degrees) {
    int steps = (int)roundf(degrees * MOTOR_STEPS_PER_REV / 360.0f);
    if (steps == 0) return;
    Step(steps);
}

void StepperMotor::RotateRevolutions(float revolutions) {
    RotateDegrees(revolutions * 360.0f);
}

void StepperMotor::StepRaw(int steps, int delay_ms) {
    // 与 728c777 的 Step() 完全一致: esp_rom_delay_us 微秒级精确延时
    // (不能用 vTaskDelay: FreeRTOS tick=10ms, pdMS_TO_TICKS(2~6)=0 -> 无延时 -> 电机狂抖)
    if (steps == 0) return;
    int dir = (steps > 0) ? 1 : -1;
    unsigned int total = (steps > 0) ? (unsigned int)steps : (unsigned int)(-steps);
    uint32_t us = (delay_ms > 0 ? (uint32_t)delay_ms : 2) * 1000;   // ms -> us

    for (unsigned int i = 0; i < total; ++i) {
        AdvancePhase(dir > 0);
        esp_rom_delay_us(us);
        // 每 64 步让出一次 CPU, 避免长时间忙等触发看门狗
        if ((i & 0x3F) == 0x3F) {
            vTaskDelay(1);
        }
    }
}

void StepperMotor::SetSpeed(int delay_ms) {
    if (delay_ms < 1) delay_ms = 1;
    step_delay_ms_ = delay_ms;
}

// ============================================================
// MotorControl
// ============================================================

MotorControl::MotorControl() {
    for (int i = 0; i < MOTOR_COUNT; i++) {
        motors_[i] = nullptr;
        active_dir_[i] = 0;
    }
}

MotorControl::~MotorControl() {
    if (motor_task_) {
        vTaskDelete(motor_task_);
        motor_task_ = nullptr;
    }
    if (step_mutex_) {
        vSemaphoreDelete(step_mutex_);
        step_mutex_ = nullptr;
    }
    for (int i = 0; i < MOTOR_COUNT; i++) {
        delete motors_[i];
        motors_[i] = nullptr;
    }
    if (adc_handle_) {
        adc_oneshot_del_unit(adc_handle_);
        adc_handle_ = nullptr;
    }
}

void MotorControl::Initialize() {
    if (initialized_) return;

    // 共享 ADC1 oneshot 单元 (电机电位器 + 电池电压共用, 避免重复创建)
    adc_oneshot_unit_init_cfg_t adc_init_cfg = {
        .unit_id = ADC_UNIT_1,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_init_cfg, &adc_handle_));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = POT_ADC_ATTEN,
        .bitwidth = POT_ADC_WIDTH,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, (adc_channel_t)POT_NOD_CHAN, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, (adc_channel_t)POT_SHAKE_CHAN, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, (adc_channel_t)BATTERY_ADC_CHAN, &chan_cfg));

    const uint32_t lo = (uint32_t)((uint32_t)POT_MAX_VALUE * POT_RANGE_MIN_PCT / 100);
    const uint32_t hi = (uint32_t)((uint32_t)POT_MAX_VALUE * POT_RANGE_MAX_PCT / 100);

    // 点头电机: U17 via U9 = GPIO41/40/48/47 + 电位器 U12 (ADC1_CH5)
    motors_[MOTOR_NOD] = new StepperMotor(
        MOTOR_NOD_A_GPIO, MOTOR_NOD_B_GPIO, MOTOR_NOD_C_GPIO, MOTOR_NOD_D_GPIO,
        adc_handle_, (adc_channel_t)POT_NOD_CHAN, lo, hi, MOTOR_NOD_POT_CW_INC);

    // 摇头电机: U16 via U8 = GPIO21/18/17/16 + 电位器 U5 (ADC1_CH4)
    motors_[MOTOR_SHAKE] = new StepperMotor(
        MOTOR_SHAKE_A_GPIO, MOTOR_SHAKE_B_GPIO, MOTOR_SHAKE_C_GPIO, MOTOR_SHAKE_D_GPIO,
        adc_handle_, (adc_channel_t)POT_SHAKE_CHAN, lo, hi, MOTOR_SHAKE_POT_CW_INC);

    motors_[MOTOR_NOD]->Initialize();
    motors_[MOTOR_SHAKE]->Initialize();
    motors_[MOTOR_NOD]->SetPinOrder(MOTOR_NOD_PIN_ORDER);
    motors_[MOTOR_SHAKE]->SetPinOrder(MOTOR_SHAKE_PIN_ORDER);

    step_mutex_ = xSemaphoreCreateMutex();

    initialized_ = true;
    ESP_LOGI(TAG, "Motor control init (nod=41/40/48/47+CH5, shake=21/18/17/16+CH4, has_pot=%d)",
             MOTOR_HAS_POT);
}

StepperMotor& MotorControl::GetMotor(MotorId id) {
    return *motors_[id];
}

void MotorControl::Drive(MotorId id, int dir) {
    if (id < 0 || id >= MOTOR_COUNT) return;
    bool invert = (id == MOTOR_NOD) ? MOTOR_NOD_INVERT : MOTOR_SHAKE_INVERT;
    int eff = invert ? -dir : dir;
    active_dir_[id] = (eff > 0) ? 1 : ((eff < 0) ? -1 : 0);
    if (active_dir_[id] == 0) {
        motors_[id]->Stop();
    }
}

void MotorControl::StopAll() {
    for (int i = 0; i < MOTOR_COUNT; i++) {
        active_dir_[i] = 0;
        if (motors_[i]) motors_[i]->Stop();
    }
}

void MotorControl::StartMotorTask() {
    if (motor_task_) return;
    xTaskCreate(MotorTaskFunc, "motor", 3072, this, 5, &motor_task_);
}

void MotorControl::MotorTaskFunc(void* arg) {
    auto* self = static_cast<MotorControl*>(arg);
    self->MotorLoop();
}

void MotorControl::MotorLoop() {
    int yield_cnt = 0;
    while (true) {
        int delay_ms = 50;
        xSemaphoreTake(step_mutex_, portMAX_DELAY);
        if (dual_active_) {
            delay_ms = DualGestureTick();             // 双轴手势优先 (转头等联动)
        } else if (gesture_active_) {
            delay_ms = GestureTick();                 // 单轴手势
        } else if (homing_) {
            delay_ms = HomeTick();                    // 回中 (按 pos_ 反向步进至 0)
        } else {
            bool any = false;
            for (int i = 0; i < MOTOR_COUNT; i++) {
                int d = active_dir_[i];
                if (d != 0) {
                    motors_[i]->StepOnceLimited(d > 0);
                    pos_[i] += (d > 0) ? 1 : -1;      // 累计相对零位的偏移
                    any = true;
                }
            }
            if (any) delay_ms = MOTOR_STEP_DELAY_MS;
        }
        xSemaphoreGive(step_mutex_);

        // 步进延时: <10ms 时 FreeRTOS tick(10ms) 粒度不够, pdMS_TO_TICKS 算出 0
        // -> 电机狂抖不转只发烫。改用 esp_rom_delay_us 微秒级精确延时。
        if (delay_ms > 0 && delay_ms < 10) {
            esp_rom_delay_us((uint32_t)delay_ms * 1000);
            if (++yield_cnt >= 64) { vTaskDelay(1); yield_cnt = 0; }
        } else {
            vTaskDelay(pdMS_TO_TICKS(delay_ms > 0 ? delay_ms : 50));
            yield_cnt = 0;
        }
    }
}

// 推进手势一微步, 返回本步延时 (速度)
int MotorControl::GestureTick() {
    if (gesture_idx_ >= gesture_len_) {
        gesture_active_ = false;
        gesture_done_ = true;
        return 50;
    }
    if (gesture_remaining_ <= 0) {
        // 当前步走完, 进入下一步
        gesture_idx_++;
        if (gesture_idx_ >= gesture_len_) {
            gesture_active_ = false;
            gesture_done_ = true;
            return 50;
        }
        gesture_remaining_ = std::abs((int)gesture_[gesture_idx_].steps);
        return gesture_[gesture_idx_].delay_ms ? gesture_[gesture_idx_].delay_ms
                                               : MOTOR_STEP_DELAY_MS;
    }
    const GestureStep& gs = gesture_[gesture_idx_];
    if (gs.motor < MOTOR_COUNT) {
        motors_[gs.motor]->StepOnceLimited(gs.steps >= 0);
        pos_[gs.motor] += (gs.steps >= 0) ? 1 : -1;   // 累计相对零位的偏移
    }
    gesture_remaining_--;
    return gs.delay_ms ? gs.delay_ms : MOTOR_STEP_DELAY_MS;
}

void MotorControl::PlayGesture(const GestureStep* steps, int n) {
    if (!initialized_ || !steps || n <= 0) return;
    xSemaphoreTake(step_mutex_, portMAX_DELAY);
    for (int i = 0; i < MOTOR_COUNT; i++) active_dir_[i] = 0;   // 取消手动驱动
    dual_active_ = false;   // 取消双轴手势, 单轴优先
    int len = n > MAX_GESTURE_STEPS ? MAX_GESTURE_STEPS : n;
    for (int i = 0; i < len; i++) gesture_[i] = steps[i];
    gesture_len_ = len;
    gesture_idx_ = 0;
    gesture_remaining_ = std::abs((int)gesture_[0].steps);
    gesture_active_ = true;
    gesture_done_ = false;
    ESP_LOGI(TAG, "gesture play: %d segs (seg0: motor%d steps%d %dms)",
             len, (int)gesture_[0].motor, (int)gesture_[0].steps, (int)gesture_[0].delay_ms);
    xSemaphoreGive(step_mutex_);
}

void MotorControl::PlayDualGesture(const DualGestureStep* steps, int n, bool auto_home) {
    if (!initialized_ || !steps || n <= 0) return;
    xSemaphoreTake(step_mutex_, portMAX_DELAY);
    for (int i = 0; i < MOTOR_COUNT; i++) active_dir_[i] = 0;   // 取消手动驱动
    gesture_active_ = false;                                    // 双轴优先, 取消单轴手势
    homing_ = false;                                            // 取消回中
    dual_auto_home_ = auto_home;
    int len = n > MAX_DUAL_GESTURE_STEPS ? MAX_DUAL_GESTURE_STEPS : n;
    for (int i = 0; i < len; i++) dual_[i] = steps[i];
    dual_len_ = len;
    dual_idx_ = 0;
    dual_active_ = true;
    gesture_done_ = false;
    DualSetupSegment();
    ESP_LOGI(TAG, "dual gesture play: %d segs auto_home=%d (seg0: nod%d shake%d %dms)",
             len, auto_home ? 1 : 0,
             (int)dual_[0].nod_steps, (int)dual_[0].shake_steps, (int)dual_[0].delay_ms);
    xSemaphoreGive(step_mutex_);
}

// 按 dual_[dual_idx_] 计算本段主/副轴参数。主轴 = 本段步数多的电机 (每 tick 走一步),
// 副轴用 Bresenham 把它的步数均摊到主轴步数上。
void MotorControl::DualSetupSegment() {
    if (dual_idx_ >= dual_len_) { dual_active_ = false; gesture_done_ = true; return; }
    const DualGestureStep& s = dual_[dual_idx_];
    int na = std::abs((int)s.nod_steps);
    int sa = std::abs((int)s.shake_steps);
    dual_delay_ms_ = s.delay_ms ? s.delay_ms : MOTOR_STEP_DELAY_MS;
    if (na >= sa) {
        dual_major_motor_ = MOTOR_NOD;
        dual_major_dir_   = (s.nod_steps >= 0) ? 1 : -1;
        dual_minor_motor_ = MOTOR_SHAKE;
        dual_minor_dir_   = (s.shake_steps >= 0) ? 1 : -1;
        dual_major_total_ = na; dual_major_left_ = na; dual_minor_total_ = sa;
    } else {
        dual_major_motor_ = MOTOR_SHAKE;
        dual_major_dir_   = (s.shake_steps >= 0) ? 1 : -1;
        dual_minor_motor_ = MOTOR_NOD;
        dual_minor_dir_   = (s.nod_steps >= 0) ? 1 : -1;
        dual_major_total_ = sa; dual_major_left_ = sa; dual_minor_total_ = na;
    }
    dual_err_ = 0;
}

// 推进双轴手势一微步: 主轴走一步, 副轴按 Bresenham 误差决定是否走一步。返回本步延时。
int MotorControl::DualGestureTick() {
    // 当前段走完 -> 推进到下一段 (跳过空段)
    while (dual_idx_ < dual_len_ && dual_major_left_ <= 0) {
        dual_idx_++;
        if (dual_idx_ >= dual_len_) {
            DualGestureDone();
            return 50;
        }
        DualSetupSegment();
    }
    if (dual_major_left_ <= 0) {     // 空播放 / 全部走完
        DualGestureDone();
        return 50;
    }
    // 主轴走一步
    motors_[dual_major_motor_]->StepOnceLimited(dual_major_dir_ > 0);
    pos_[dual_major_motor_] += dual_major_dir_;          // 累计相对零位的偏移
    dual_major_left_--;
    // 副轴: Bresenham 整数 DDA, 把 minor_total 步均摊到 major_total 步上
    dual_err_ += dual_minor_total_;
    if (dual_major_total_ > 0 && dual_err_ >= dual_major_total_) {
        dual_err_ -= dual_major_total_;
        motors_[dual_minor_motor_]->StepOnceLimited(dual_minor_dir_ > 0);
        pos_[dual_minor_motor_] += dual_minor_dir_;
    }
    return dual_delay_ms_;
}

// 双轴手势结束: auto_home 则接着回中 (到位后断电), 否则立即断电防发热
void MotorControl::DualGestureDone() {
    dual_active_ = false;
    gesture_done_ = true;
    if (dual_auto_home_) {
        home_steps_ = 0;
        homing_ = true;        // 下一 tick 由 HomeTick 接管, 到位断电
    } else {
        StopAll();             // 立即断电
    }
}

void MotorControl::StopGesture() {
    if (!initialized_) return;   // 未初始化 (电机被 menuconfig 关闭) 时安全跳过
    ESP_LOGI(TAG, "gesture stop");
    xSemaphoreTake(step_mutex_, portMAX_DELAY);
    gesture_active_ = false;
    dual_active_ = false;
    homing_ = false;
    gesture_done_ = true;
    for (int i = 0; i < MOTOR_COUNT; i++) {
        active_dir_[i] = 0;
        if (motors_[i]) motors_[i]->Stop();
    }
    xSemaphoreGive(step_mutex_);
}

void MotorControl::Home() {
    // 回中: 非阻塞。置 homing_, 电机任务用 HomeTick 按 pos_ 反向步进回零位, 到位断电。
    if (!initialized_) return;
    xSemaphoreTake(step_mutex_, portMAX_DELAY);
    gesture_active_ = false;
    dual_active_ = false;
    for (int i = 0; i < MOTOR_COUNT; i++) active_dir_[i] = 0;
    home_steps_ = 0;
    homing_ = true;
    xSemaphoreGive(step_mutex_);
    ESP_LOGI(TAG, "home -> zero (pos nod=%ld shake=%ld)", (long)pos_[MOTOR_NOD], (long)pos_[MOTOR_SHAKE]);
}

void MotorControl::RecordZero() {
    // 记录"当前位置 = 零位": 清零步数累计 pos_。断电。不依赖电位器。
    if (!initialized_) return;
    xSemaphoreTake(step_mutex_, portMAX_DELAY);
    pos_[MOTOR_NOD] = 0;
    pos_[MOTOR_SHAKE] = 0;
    gesture_active_ = false;
    dual_active_ = false;
    homing_ = false;
    for (int i = 0; i < MOTOR_COUNT; i++) { active_dir_[i] = 0; if (motors_[i]) motors_[i]->Stop(); }   // 断电
    xSemaphoreGive(step_mutex_);
    ESP_LOGI(TAG, "zero recorded (pos cleared)");
}

// 推进回中一微步: 每轴按 pos_ 反向走一步 (把累计偏移归零), 两轴 pos_ 都到 0 则断电结束。
// 开环步数定位, 不读电位器。
int MotorControl::HomeTick() {
    bool done = true;
    for (int i = 0; i < MOTOR_COUNT; i++) {
        if (pos_[i] == 0) continue;                 // 该轴已在零位
        done = false;
        bool cw = pos_[i] < 0;                       // pos>0 (走过 cw) -> 反向 ccw; pos<0 -> cw
        motors_[i]->StepOnceLimited(cw);
        pos_[i] += cw ? 1 : -1;                      // 朝 0 收敛
    }
    if (++home_steps_ > 4096) done = true;           // 超限保护 (防卡死无限转)
    if (done) {
        homing_ = false;
        StopAll();      // 到位断电, 防发热
        gesture_done_ = true;
        pos_[MOTOR_NOD] = 0;
        pos_[MOTOR_SHAKE] = 0;
        ESP_LOGI(TAG, "home done (steps=%d)", home_steps_);
        return 50;
    }
    return MOTOR_STEP_DELAY_MS * 2;   // 回中略慢, 稳定起转
}

void MotorControl::NodSteps(int steps) {
    if (!initialized_) return;
    xSemaphoreTake(step_mutex_, portMAX_DELAY);
    motors_[MOTOR_NOD]->Step(steps);
    xSemaphoreGive(step_mutex_);
}

void MotorControl::ShakeSteps(int steps) {
    if (!initialized_) return;
    xSemaphoreTake(step_mutex_, portMAX_DELAY);
    motors_[MOTOR_SHAKE]->Step(steps);
    xSemaphoreGive(step_mutex_);
}

void MotorControl::MoveSteps(MotorId id, int steps, int delay_ms) {
    if (!initialized_ || id < 0 || id >= MOTOR_COUNT) return;
    if (delay_ms < 1) delay_ms = 3;
    xSemaphoreTake(step_mutex_, portMAX_DELAY);
    motors_[id]->StepRaw(steps, delay_ms);
    pos_[id] += steps;        // 累计相对零位的偏移 (开环)
    motors_[id]->Stop();   // 走完立刻断电, 防止线圈持续通电发热
    xSemaphoreGive(step_mutex_);
}

void MotorControl::MoveToPercent(MotorId id, int percent) {
    if (!initialized_ || id < 0 || id >= MOTOR_COUNT) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    StepperMotor* m = motors_[id];
    uint32_t target = m->pot_min() +
                      (uint32_t)((m->pot_max() - m->pot_min()) * percent / 100);
    const uint32_t tolerance = (m->pot_max() - m->pot_min()) / 25 + 1;   // ~4% 死区
    const int max_steps = MOTOR_STEPS_PER_REV;                            // 保护
    const bool inc = m->pot_cw_inc();

    xSemaphoreTake(step_mutex_, portMAX_DELAY);
    for (int i = 0; i < max_steps; i++) {
        uint32_t pot = m->ReadPotentiometer();
        int diff = (int)target - (int)pot;
        if (std::abs(diff) <= (int)tolerance) break;
        bool cw = (diff > 0) == inc;   // 需要增大读数则按 cw_inc 方向
        if (!m->StepOnceLimited(cw)) break;
        vTaskDelay(pdMS_TO_TICKS(MOTOR_STEP_DELAY_MS));
    }
    xSemaphoreGive(step_mutex_);
}

uint32_t MotorControl::ReadNodPosition() {
    if (!initialized_) return 0;
    return motors_[MOTOR_NOD]->ReadPotentiometer();
}

uint32_t MotorControl::ReadShakePosition() {
    if (!initialized_) return 0;
    return motors_[MOTOR_SHAKE]->ReadPotentiometer();
}

float MotorControl::ReadNodPositionNorm() {
    if (!initialized_) return 0.0f;
    return motors_[MOTOR_NOD]->ReadPosition();
}

float MotorControl::ReadShakePositionNorm() {
    if (!initialized_) return 0.0f;
    return motors_[MOTOR_SHAKE]->ReadPosition();
}
