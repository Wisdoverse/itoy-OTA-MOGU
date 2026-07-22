#include "power_control.h"
#include "config.h"
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_sleep.h>
#include <driver/adc.h>
#include <esp_adc/adc_oneshot.h>

#define TAG "PowerCtrl"

// 电池分压参数: 根据实际硬件调整
// 假设分压电阻 Rtop=100K, Rbottom=100K (2:1)
// ADC 满量程 3.3V → 实际输入 6.6V
// 修改这些值以匹配你的硬件
#define BATT_DIVIDER_RATIO   2.0f
#define BATT_VOLTAGE_MIN_MV  3000   // 空电 3.0V
#define BATT_VOLTAGE_MAX_MV  4200   // 满电 4.2V

// 长按关机时间
#define LONG_PRESS_MS        3000

PowerControl::PowerControl() {
    shutdown_sem_ = xSemaphoreCreateBinary();
}

PowerControl::~PowerControl() {
    if (monitor_task_) {
        vTaskDelete(monitor_task_);
    }
    if (shutdown_sem_) {
        vSemaphoreDelete(shutdown_sem_);
    }
}

void PowerControl::Initialize() {
    if (initialized_) return;

    // GPIO39 (POWER_ON): 先配置为输出高, 维持供电
    // 注: GPIO39 是 RTC GPIO, 只能输入。这里用 POWER_LATCH (GPIO42) 维持。
    // 实际上 GPIO39 在开机时由 SW4→Q3 拉低然后释放。
    // 我们需要在启动后立刻拉高 GPIO42 锁存供电。
    gpio_config_t latch_conf = {};
    latch_conf.intr_type = GPIO_INTR_DISABLE;
    latch_conf.mode = GPIO_MODE_OUTPUT;
    latch_conf.pin_bit_mask = (1ULL << POWER_LATCH_GPIO);
    latch_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    latch_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&latch_conf);
    gpio_set_level(POWER_LATCH_GPIO, 1);  // 锁存供电

    // GPIO39 配置为输入 + 上拉, 检测 SW4 按键
    gpio_config_t key_conf = {};
    key_conf.intr_type = GPIO_INTR_NEGEDGE;  // 下降沿中断
    key_conf.mode = GPIO_MODE_INPUT;
    key_conf.pin_bit_mask = (1ULL << POWER_ON_GPIO);
    key_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    key_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&key_conf);

    // 安装 GPIO ISR
    gpio_install_isr_service(0);
    gpio_isr_handler_add(POWER_ON_GPIO, PowerKeyIsrHandler, this);

    // 电池 ADC 不在此创建, 由 MotorControl 共享句柄 (见 SetBatteryAdc)
    // 启动电源监控任务
    xTaskCreate(PowerMonitorTask, "pwr_mon", 2048, this, 5, &monitor_task_);

    initialized_ = true;
    ESP_LOGI(TAG, "Power control initialized (ON=GPIO%d, KEY=GPIO%d)",
             POWER_LATCH_GPIO, POWER_ON_GPIO);
}

void IRAM_ATTR PowerControl::PowerKeyIsrHandler(void* arg) {
    auto* self = static_cast<PowerControl*>(arg);
    self->power_key_pressed_ = true;
    self->power_key_press_time_ = xTaskGetTickCount() * portTICK_PERIOD_MS;
}

void PowerControl::PowerMonitorTask(void* arg) {
    auto* self = static_cast<PowerControl*>(arg);

    while (true) {
        if (self->power_key_pressed_) {
            // 等待按键释放
            while (gpio_get_level(POWER_ON_GPIO) == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            uint32_t press_duration = xTaskGetTickCount() * portTICK_PERIOD_MS
                                       - self->power_key_press_time_;
            self->power_key_pressed_ = false;

            ESP_LOGI(TAG, "Power key pressed for %lu ms", press_duration);

            if (press_duration >= LONG_PRESS_MS && !self->shutdown_prevented_) {
                ESP_LOGW(TAG, "Long press detected, shutting down...");
                self->RequestShutdown();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void PowerControl::RequestShutdown(ShutdownCallback pre_shutdown) {
    ESP_LOGI(TAG, "Shutdown requested");

    if (pre_shutdown) {
        pre_shutdown();
    }

    // 执行关机
    ImmediatePowerOff();
}

void PowerControl::ImmediatePowerOff() {
    ESP_LOGW(TAG, "Powering off NOW");

    // 拉低 GPIO42 → ON 信号消失 → SY8089 EN 拉低 → 系统断电
    gpio_set_level(POWER_LATCH_GPIO, 0);

    // 等待断电 (如果不断电, 可能硬件有问题)
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGE(TAG, "Power did not turn off! Entering deep sleep as fallback");
    esp_deep_sleep_start();
}

void PowerControl::SetBatteryAdc(adc_oneshot_unit_handle_t handle, adc_channel_t channel) {
    battery_adc_ = handle;
    battery_chan_ = channel;
    ESP_LOGI(TAG, "Battery ADC bound (shared handle)");
}

int PowerControl::ReadBatteryMv() {
    if (!battery_adc_) return 0;
    int raw = 0;
    if (adc_oneshot_read(battery_adc_, battery_chan_, &raw) != ESP_OK) return 0;
    // raw 0..4095 -> ADC 端电压 (DB_12 衰减, 按 3.3V 满量程近似)
    // 再乘分压比还原电池电压。R7/R6 实测后校准 BATT_DIVIDER_RATIO。
    float adc_mv = (float)raw * 3300.0f / (float)POT_MAX_VALUE;
    return (int)(adc_mv * BATT_DIVIDER_RATIO);
}

int PowerControl::ReadBatteryPercent() {
    int mv = ReadBatteryMv();
    if (mv <= BATT_VOLTAGE_MIN_MV) return 0;
    if (mv >= BATT_VOLTAGE_MAX_MV) return 100;
    return (int)((mv - BATT_VOLTAGE_MIN_MV) * 100.0f
                 / (BATT_VOLTAGE_MAX_MV - BATT_VOLTAGE_MIN_MV));
}

void PowerControl::PreventShutdown(bool prevent) {
    shutdown_prevented_ = prevent;
    ESP_LOGI(TAG, "Shutdown %s", prevent ? "prevented" : "allowed");
}
