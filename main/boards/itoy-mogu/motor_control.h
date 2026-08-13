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

    // 原始步进: 不走电位器软限位、不自定义速度, 指定每步延时 (调试/裸板用)
    // 适合没接电位器、或想验证电机本身是否转的场景
    void StepRaw(int steps, int delay_ms);

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

    // 设置相位顺序: 逻辑线圈 0,1,2,3 -> 物理引脚序号 (A=0,B=1,C=2,D=3)
    // 用于修正"震动不转"(线圈接线顺序不对)。默认 0,1,2,3
    void SetPinOrder(uint8_t o0, uint8_t o1, uint8_t o2, uint8_t o3);

    uint32_t pot_min() const { return pot_min_; }
    uint32_t pot_max() const { return pot_max_; }
    bool pot_cw_inc() const { return pot_cw_inc_; }

private:
    void AdvancePhase(bool clockwise);   // 仅推进相序 + 输出, 不延时
    void SetPhase(uint8_t phase);

    gpio_num_t pins_[4];
    uint8_t pin_order_[4] = {0, 1, 2, 3};   // 逻辑线圈 -> 物理引脚序号
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

// 双轴手势步: 两电机同步运动一段 (用于"转头"等需要点头+摇头联动的动作)。
// nod_steps / shake_steps 为本段增量步数 (有符号), 由电机任务用 Bresenham 整数
// 插值把副轴步数均摊到主轴步数上, 实现平滑的斜向/弧线联动。delay_ms = 主轴每步延时。
struct DualGestureStep {
    int16_t nod_steps;     // 本段点头电机增量 (+=抬头/仰, -=低头)
    int16_t shake_steps;   // 本段摇头电机增量 (+=右, -=左)
    uint16_t delay_ms;     // 单步延时 (速度) = 主轴每步间隔
};

#define MAX_DUAL_GESTURE_STEPS 16

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
    // 调试用原始步进: 不走电位器软限位, 带加速斜坡; delay_ms=每步巡航延时
    void MoveSteps(MotorId id, int steps, int delay_ms = 3);
    // 步进到目标电位器百分比 (0~100), 带最大步数保护
    void MoveToPercent(MotorId id, int percent);

    // ---- 非阻塞手势 (情绪状态用, 由电机任务播放) ----
    // 播放一段手势脚本 (拷贝到内部缓冲, 立即返回)
    void PlayGesture(const GestureStep* steps, int n);
    // 播放一段双轴联动手势 (点头+摇头同步, Bresenham 插值; 用于"转头"等)。
    // auto_home=true 时手势结束后自动回中并断电 (转头这类"动作"用; 保持姿态的情绪别开)
    void PlayDualGesture(const DualGestureStep* steps, int n, bool auto_home = false);
    bool IsGestureDone() const { return gesture_done_; }
    void StopGesture();          // 立即停止手势/驱动并断电
    void Home();                 // 回中: 按 pos_ 反向步进回零位, 到位断电 (步数定位, 不依赖电位器)
    void RecordZero();           // 记录当前位置为零位 (清零 pos_), 断电

    // 位置读取
    uint32_t ReadNodPosition();
    uint32_t ReadShakePosition();
    float ReadNodPositionNorm();
    float ReadShakePositionNorm();

private:
    static void MotorTaskFunc(void* arg);
    void MotorLoop();
    int GestureTick();          // 推进单轴手势一微步, 返回本步延时 ms
    int DualGestureTick();      // 推进双轴手势一微步, 返回本步延时 ms
    void DualSetupSegment();    // 按 dual_[idx] 计算本段主/副轴参数 (Bresenham)
    void DualGestureDone();     // 双轴手势收尾: auto_home 则转回中, 否则断电
    int HomeTick();             // 推进回中一微步 (按 pos_ 反向步进至 0), 返回本步延时

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

    // 双轴手势播放器 (转头等联动动作)
    DualGestureStep dual_[MAX_DUAL_GESTURE_STEPS]{};
    int dual_len_ = 0;
    int dual_idx_ = 0;
    int dual_major_motor_ = 0;   // 本段主轴 (步数多者) MotorId
    int dual_major_dir_ = 0;     // 主轴方向 ±1
    int dual_major_left_ = 0;    // 本段主轴剩余步数 (= 本段 tick 数)
    int dual_major_total_ = 0;   // 本段主轴总步数 (Bresenham 分母)
    int dual_minor_motor_ = 0;   // 本段副轴 MotorId
    int dual_minor_dir_ = 0;     // 副轴方向 ±1
    int dual_minor_total_ = 0;   // 本段副轴总步数
    int dual_err_ = 0;           // Bresenham 累计误差
    uint16_t dual_delay_ms_ = 0;
    volatile bool dual_active_ = false;

    // 相对定位 (电位器未接入时): pos_ = 自"记录零位"以来的累计步数 (有符号, +=cw)。
    // 回中即把 pos_ 反向步进归零。电位器接好后软限位由 CanStep 提供, 定位仍用步数。
    int32_t pos_[MOTOR_COUNT] = {0, 0};
    volatile bool homing_ = false;   // 回中模式 (电机任务按 pos_ 反向步进至 0, 到位断电)
    int home_steps_ = 0;             // 回中步数累计 (超限保护)
    bool dual_auto_home_ = false;    // 双轴手势结束后自动回中
};

#endif // MOTOR_CONTROL_H_
