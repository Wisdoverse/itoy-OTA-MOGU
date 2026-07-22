#ifndef STEPPER_MOTOR_H
#define STEPPER_MOTOR_H

#include <stdint.h>
#include <driver/gpio.h>

/**
 * 步进电机测试模块
 *
 * 提供两个 4 相单极性步进电机（ULN2003 + 28BYJ-48 类）的半步驱动，
 * 并通过专用 UART 接收 ASCII 命令控制电机转动。
 *
 * 引脚与串口配置见 boards/<board>/config.h 中的 MOTOR_* 宏。
 *
 * 命令格式（串口发送，回车换行结束）：
 *   A <步数> [延时ms]   电机A 正转(+) / 反转(-)
 *   B <步数> [延时ms]   电机B 正转(+) / 反转(-)
 *   O                   两电机断电
 *   H                   显示帮助
 * 示例：A 2048      （电机A 正转 2048 步）
 *       B -2048 3   （电机B 反转 2048 步，每步 3ms）
 */

// 单个 4 相步进电机驱动（半步 8 拍）
class StepperMotor {
public:
    StepperMotor(gpio_num_t in1, gpio_num_t in2, gpio_num_t in3, gpio_num_t in4);
    ~StepperMotor();

    // 配置 4 个控制脚为输出并断电（在调度器启动后调用）
    void Init();

    // 转动 steps 步：>0 正转，<0 反转，0 不动
    void Step(int steps);

    // 设置每半步延时（微秒），越小转得越快
    void SetStepDelay(uint32_t us) { step_delay_us_ = us; }

    // 断电（所有线圈置低，省电 / 降温）
    void PowerOff();

private:
    void WritePhase(uint8_t phase);

    gpio_num_t pins_[4];
    int phase_;            // 当前相位 0..7
    uint32_t step_delay_us_;
};

// 启动步进电机测试模块：初始化电机A/B，配置命令串口，创建命令接收任务
void StepperTestStart();

#endif  // STEPPER_MOTOR_H
