#ifndef STEPPER_MOTOR_H_
#define STEPPER_MOTOR_H_

#include "sdkconfig.h"

// 步进电机测试模块 (串口命令驱动), 仅 CONFIG_ITOY_ENABLE_MOTOR_TEST 时编译。
// 注意: 纯 GPIO 半步驱动, **不使用电位器 / ADC / 软限位** —— 无需接电位器即可
// 验证电机接线; 也不读位置、不防堵转, 注意控制步数避免机械顶死。
#if CONFIG_ITOY_ENABLE_MOTOR_TEST

#include <stdint.h>
#include <driver/gpio.h>

/**
 * 单个 4 相单极性步进电机驱动 (ULN2003 + 28BYJ-48 类, 半步 8 拍)。
 * 引脚见 config.h 的 MOTOR_A/B_IN*_GPIO。
 *
 * 串口命令 (UART0 / USB, 115200, 回车结束):
 *   A <步数> [延时ms]   电机A(点头) 正(+)反(-)
 *   B <步数> [延时ms]   电机B(摇头) 正(+)反(-)
 *   O                   两电机断电
 *   H                   帮助
 * 示例: A 2048       (电机A 正转 2048 步)
 *       B -2048 3    (电机B 反转 2048 步, 每步 3ms)
 */
class TestStepper {
public:
    TestStepper(gpio_num_t in1, gpio_num_t in2, gpio_num_t in3, gpio_num_t in4);
    ~TestStepper();

    // 配置 4 个控制脚为输出并断电 (调度器启动后调用)
    void Init();

    // 转动 steps 步: >0 正转, <0 反转, 0 不动
    void Step(int steps);

    // 设置每半步延时 (微秒), 越小转得越快
    void SetStepDelay(uint32_t us) { step_delay_us_ = us; }

    // 断电 (所有线圈置低, 省电/降温)
    void PowerOff();

private:
    void WritePhase(uint8_t phase);

    gpio_num_t pins_[4];
    int phase_;            // 当前相位 0..7
    uint32_t step_delay_us_;
};

// 启动步进电机测试模块: 初始化电机A/B, 挂接 UART0 命令接收任务
void StepperTestStart();

#endif  // CONFIG_ITOY_ENABLE_MOTOR_TEST

#endif  // STEPPER_MOTOR_H_
