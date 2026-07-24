#include "motor_control.h"
#include "config.h"
#include <esp_log.h>
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
        gpio_set_level(pins_[i], kHalfStepSeq[phase][i]);
    }
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
    uint32_t pot = ReadPotentiometer();
    // 该方向是否趋向电位器上限
    bool toward_max = (clockwise == pot_cw_inc_);
    if (toward_max) {
        return pot < pot_max_;
    }
    return pot > pot_min_;
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

    step_mutex_ = xSemaphoreCreateMutex();

    initialized_ = true;
    ESP_LOGI(TAG, "Motor control init (nod=41/40/48/47+CH5, shake=21/18/17/16+CH4)");
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
    while (true) {
        int delay_ms = 50;
        xSemaphoreTake(step_mutex_, portMAX_DELAY);
        if (gesture_active_) {
            delay_ms = GestureTick();                 // 手势优先
        } else {
            bool any = false;
            for (int i = 0; i < MOTOR_COUNT; i++) {
                int d = active_dir_[i];
                if (d != 0) {
                    motors_[i]->StepOnceLimited(d > 0);
                    any = true;
                }
            }
            if (any) delay_ms = MOTOR_STEP_DELAY_MS;
        }
        xSemaphoreGive(step_mutex_);

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
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
    }
    gesture_remaining_--;
    return gs.delay_ms ? gs.delay_ms : MOTOR_STEP_DELAY_MS;
}

void MotorControl::PlayGesture(const GestureStep* steps, int n) {
    if (!initialized_ || !steps || n <= 0) return;
    xSemaphoreTake(step_mutex_, portMAX_DELAY);
    for (int i = 0; i < MOTOR_COUNT; i++) active_dir_[i] = 0;   // 取消手动驱动
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

void MotorControl::StopGesture() {
    ESP_LOGI(TAG, "gesture stop");
    xSemaphoreTake(step_mutex_, portMAX_DELAY);
    gesture_active_ = false;
    gesture_done_ = true;
    for (int i = 0; i < MOTOR_COUNT; i++) {
        active_dir_[i] = 0;
        if (motors_[i]) motors_[i]->Stop();
    }
    xSemaphoreGive(step_mutex_);
}

void MotorControl::Home() {
    if (!initialized_) return;
    GestureStep script[MOTOR_COUNT];
    int n = 0;
    for (int i = 0; i < MOTOR_COUNT; i++) {
        uint32_t pot = motors_[i]->ReadPotentiometer();
        uint32_t mid = (motors_[i]->pot_min() + motors_[i]->pot_max()) / 2;
        int delta = (int)mid - (int)pot;
        // 电位器增量 -> 步数 (启发式缩放, 需实测校准)
        int steps = delta / 4;
        if (motors_[i]->pot_cw_inc() == 0) steps = -steps;
        if (steps > 300) steps = 300;
        if (steps < -300) steps = -300;
        if (steps != 0) {
            script[n].motor = (uint8_t)i;
            script[n].steps = (int16_t)steps;
            script[n].delay_ms = (uint16_t)(MOTOR_STEP_DELAY_MS * 3);   // 回中慢速
            n++;
        }
    }
    ESP_LOGI(TAG, "home: motor0 %d steps, motor1 % d steps",
             (int)script[0].steps, n > 1 ? (int)script[1].steps : 0);
    PlayGesture(script, n);
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
    return motors_[MOTOR_NOD]->ReadPotentiometer();
}

uint32_t MotorControl::ReadShakePosition() {
    return motors_[MOTOR_SHAKE]->ReadPotentiometer();
}

float MotorControl::ReadNodPositionNorm() {
    return motors_[MOTOR_NOD]->ReadPosition();
}

float MotorControl::ReadShakePositionNorm() {
    return motors_[MOTOR_SHAKE]->ReadPosition();
}
