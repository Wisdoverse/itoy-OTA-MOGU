#ifndef MOTOR_CONTROL_H_
#define MOTOR_CONTROL_H_

#include <driver/gpio.h>
#include <driver/adc.h>
#include <esp_adc/adc_oneshot.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// 电机 ID
enum MotorId : int {
    MOTOR_NOD   = 0,   // 点头电机 (前后) U17
    MOTOR_SHAKE = 1,   // 摇头电机 (左右) U16
    MOTOR_COUNT = 2,
};

// 单路 ULN2003 步进电机 + 电位器位置反馈
class StepperMotor {
public:
    StepperMotor(gpio_num_t a, gpio_num_t b, gpio_num_t c, gpio_num_t d,
                 adc_oneshot_unit_handle_t adc_handle, adc_channel_t adc_channel,
                 uint32_t pot_min, uint32_t pot_max, bool pot_cw_inc);
    ~StepperMotor();

    void Initialize();

    // 固定步数动作 (受软限位保护, 到限位即停)
    void Step(int steps);                       // steps>0 正转, <0 反转
    void RotateDegrees(float degrees);
    void RotateRevolutions(float revolutions);

    // 带软限位的单步 (供驱动任务/手势调用), 返回是否真的走了
    bool StepOnceLimited(bool clockwise);

    // 当前方向是否允许继续步进 (软限位)
    bool CanStep(bool clockwise) const;

    // 停止 (所有相断电)
    void Stop();

    // 读取电位器位置 (0~POT_MAX_VALUE)
    uint32_t ReadPotentiometer() const;
    float ReadPosition() const;

    void SetSpeed(int delay_ms);
    int GetSpeed() const { return step_delay_ms_; }

    uint32_t pot_min() const { return pot_min_; }
    uint32_t pot_max() const { return pot_max_; }
    bool pot_cw_inc() const { return pot_cw_inc_; }

private:
    void AdvancePhase(bool clockwise);   // 仅推进相序 + 输出, 不延时
    void SetPhase(uint8_t phase);

    gpio_num_t pins_[4];
    adc_oneshot_unit_handle_t adc_handle_;
    adc_channel_t adc_channel_;
    uint32_t pot_min_;
    uint32_t pot_max_;
    bool pot_cw_inc_;          // 正转(cw)是否使电位器读数增大
    int step_delay_ms_;
    uint8_t current_phase_ = 0;
    bool initialized_ = false;

    // 8 拍步进序列 (半步模式, 更平滑)
    static constexpr uint8_t kHalfStepSeq[8][4] = {
        {1, 0, 0, 0}, {1, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 1, 0},
        {0, 0, 1, 0}, {0, 0, 1, 1}, {0, 0, 0, 1}, {1, 0, 0, 1},
    };
};

// 手势脚本中的一步: 指定电机走 N 步 (正=cw/负=ccw), 每步延时 delay_ms (即速度)
struct GestureStep {
    uint8_t motor;     // MotorId
    int16_t steps;     // 有符号步数
    uint16_t delay_ms; // 单步延时 (速度)
};

#define MAX_GESTURE_STEPS 16

class MotorControl {
public:
    MotorControl();
    ~MotorControl();

    void Initialize();

    StepperMotor& GetMotor(MotorId id);

    // 共享 ADC1 oneshot 句柄 (供电源模块读电池, 避免重复创建 ADC 单元)
    adc_oneshot_unit_handle_t GetAdcHandle() const { return adc_handle_; }

    // ---- 触摸驱动 (按住即动) ----
    // dir: +1 正转, -1 反转, 0 停止该电机
    void Drive(MotorId id, int dir);
    void StopAll();
    void StartMotorTask();     // 启动步进消费任务 (~MOTOR_STEP_DELAY_MS tick)

    // ---- 手势 API (阻塞, 供应用层 / AI 后续调用) ----
    void NodSteps(int steps);
    void ShakeSteps(int steps);
    // 步进到目标电位器百分比 (0~100), 带最大步数保护
    void MoveToPercent(MotorId id, int percent);

    // ---- 非阻塞手势 (情绪状态用, 由电机任务播放) ----
    // 播放一段手势脚本 (拷贝到内部缓冲, 立即返回)
    void PlayGesture(const GestureStep* steps, int n);
    bool IsGestureDone() const { return gesture_done_; }
    void StopGesture();          // 立即停止手势/驱动并断电
    void Home();                 // 两电机向中立位(电位器 50%)回中, 步数有上限

    // 位置读取
    uint32_t ReadNodPosition();
    uint32_t ReadShakePosition();
    float ReadNodPositionNorm();
    float ReadShakePositionNorm();

private:
    static void MotorTaskFunc(void* arg);
    void MotorLoop();
    int GestureTick();   // 推进手势一微步, 返回本步延时 ms

    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    StepperMotor* motors_[MOTOR_COUNT]{};
    bool initialized_ = false;

    SemaphoreHandle_t step_mutex_ = nullptr;        // 串行化步进 (驱动任务 vs 手势)
    TaskHandle_t motor_task_ = nullptr;
    volatile int active_dir_[MOTOR_COUNT]{};        // 每电机当前驱动方向 +1/-1/0

    // 手势播放器
    GestureStep gesture_[MAX_GESTURE_STEPS]{};
    int gesture_len_ = 0;
    int gesture_idx_ = 0;
    int gesture_remaining_ = 0;
    volatile bool gesture_active_ = false;
    volatile bool gesture_done_ = true;
};

#endif // MOTOR_CONTROL_H_
