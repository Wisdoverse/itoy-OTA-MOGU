#ifndef POWER_CONTROL_H_
#define POWER_CONTROL_H_

#include <driver/gpio.h>
#include <driver/adc.h>
#include <esp_adc/adc_oneshot.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <functional>

// 电源控制:
// GPIO39 (POWER_ON): 高电平维持 SY8089 EN 供电锁存
// GPIO42 (POWER_LATCH): ON 信号, 拉低触发软关机
// 按键 (SW4) 经 Q3(N-MOS) 拉低 GPIO39 → 触发中断 → 软关机
//
// 开机流程:
//   SW4 按下 → Q3 导通 → GPIO39 被拉低 → ESP32 启动
//   ESP32 启动后 GPIO42 输出高 → D1 导通 → ON 信号维持 Q3 导通
//   同时 GPIO39 配置为高电平输出 → D1→ON→SY8089 EN 维持供电
//
// 关机流程:
//   方式1: 长按 SW4 → GPIO39 检测到低电平 → 软件关机
//   方式2: 软件调用 PowerOff() → GPIO42 拉低 → ON 信号消失 → SY8089 关闭

class PowerControl {
public:
    using ShutdownCallback = std::function<void()>;

    PowerControl();
    ~PowerControl();

    void Initialize();

    // 注入共享 ADC 句柄 (由 MotorControl 提供, 避免重复创建 ADC_UNIT_1)
    void SetBatteryAdc(adc_oneshot_unit_handle_t handle, adc_channel_t channel);

    // 软关机: 拉低 GPIO42, 系统将在几毫秒内断电
    // 可选: 执行 shutdown_cb 后再断电
    void RequestShutdown(ShutdownCallback pre_shutdown = nullptr);

    // 立即断电 (不执行回调)
    void ImmediatePowerOff();

    // 读取电池电压 (ADC), 返回 mV
    // 分压电阻: R7(上) / R6(下), 需根据实际硬件调整 BATT_DIVIDER_RATIO
    int ReadBatteryMv();

    // 获取电池百分比 (0~100)
    int ReadBatteryPercent();

    // 防止关机 (在关键操作期间调用)
    void PreventShutdown(bool prevent);

    bool IsShutdownPrevented() const { return shutdown_prevented_; }

private:
    static void PowerMonitorTask(void* arg);
    static void IRAM_ATTR PowerKeyIsrHandler(void* arg);

    volatile bool power_key_pressed_ = false;
    volatile uint32_t power_key_press_time_ = 0;
    bool shutdown_prevented_ = false;
    bool initialized_ = false;
    TaskHandle_t monitor_task_ = nullptr;
    SemaphoreHandle_t shutdown_sem_ = nullptr;
    ShutdownCallback pre_shutdown_cb_;

    adc_oneshot_unit_handle_t battery_adc_ = nullptr;
    adc_channel_t battery_chan_ = (adc_channel_t)0;
};

#endif // POWER_CONTROL_H_
