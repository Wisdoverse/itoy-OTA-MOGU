#include "power_control.h"
#include "config.h"

#include <esp_log.h>
#include <esp_err.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "power"

// ---------------------------------------------------------------------------
// 可调参数
// ---------------------------------------------------------------------------
// 按键有效电平：0 = 高电平有效（网表 R15 下拉，按下为高）；1 = 低电平有效
#define POWER_BUTTON_ACTIVE_LOW     0

#define POWER_BUTTON_HOLD_MS        2000   // 长按关机阈值（持续按下达到此时长即关机）
#define POWER_BUTTON_POLL_MS        20     // 轮询周期
#define POWER_STARTUP_IGNORE_MS     500    // 开机后忽略按键的时长，避免上电长按误触发关机
#define POWER_TASK_STACK            2048

// ---------------------------------------------------------------------------
// 基本操作
// ---------------------------------------------------------------------------

static inline void SetPowerLatch(int on) {
    gpio_set_level(POWER_ON_GPIO, on ? 1 : 0);
}

static inline bool IsButtonPressed(void) {
    int lvl = gpio_get_level(POWER_JUDGE_GPIO);
    return POWER_BUTTON_ACTIVE_LOW ? (lvl == 0) : (lvl == 1);
}

static void PowerOff(void) {
    ESP_LOGW(TAG, "长按 %dms，开始关机：两脚全部拉低", POWER_BUTTON_HOLD_MS);

    // POWER_ON = 0：释放供电锁定
    SetPowerLatch(0);

    // POWER_JUDGE 切为输出低：主动把 ON 网络拉低，协助 PMIC 彻底断电
    gpio_set_direction(POWER_JUDGE_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_JUDGE_GPIO, 0);

    // 等待电源切断（若硬件未断，进程仍在，循环里会继续维持低电平）
    vTaskDelay(pdMS_TO_TICKS(2000));
}

// ---------------------------------------------------------------------------
// 按键监测任务
// ---------------------------------------------------------------------------
static void PowerMonitorTask(void* arg) {
    (void)arg;

    // 开机忽略窗口：上电瞬间用户可能仍按着开机键
    int ignore_ms = POWER_STARTUP_IGNORE_MS;
    int held_ms = 0;

    while (true) {
        if (ignore_ms > 0) {
            ignore_ms -= POWER_BUTTON_POLL_MS;
            held_ms = 0;
        } else if (IsButtonPressed()) {
            held_ms += POWER_BUTTON_POLL_MS;
            if (held_ms >= POWER_BUTTON_HOLD_MS) {
                PowerOff();
                held_ms = 0;
            }
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(POWER_BUTTON_POLL_MS));
    }
}

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------
void PowerControlInit(void) {
    // 1) POWER_ON(GPIO39)：输出，立即拉高锁定供电（必须赶在用户松开开机键前完成）
    gpio_config_t out_cfg = {};
    out_cfg.pin_bit_mask = (1ULL << POWER_ON_GPIO);
    out_cfg.mode = GPIO_MODE_OUTPUT;
    out_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    out_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    out_cfg.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&out_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "POWER_ON 配置失败: %s", esp_err_to_name(err));
    }
    gpio_set_level(POWER_ON_GPIO, 1);
    ESP_LOGI(TAG, "POWER_ON(GPIO%d) = HIGH，供电已锁定", POWER_ON_GPIO);

    // 2) POWER_JUDGE(GPIO42)：输入，检测按键（按钮按下为高，启用内部下拉匹配 R15）
    gpio_config_t in_cfg = {};
    in_cfg.pin_bit_mask = (1ULL << POWER_JUDGE_GPIO);
    in_cfg.mode = GPIO_MODE_INPUT;
    in_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    in_cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    in_cfg.intr_type = GPIO_INTR_DISABLE;
    err = gpio_config(&in_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "POWER_JUDGE 配置失败: %s", esp_err_to_name(err));
    }

    // 3) 启动按键监测任务
    xTaskCreate(PowerMonitorTask, "power_mon", POWER_TASK_STACK, NULL, 4, NULL);
    ESP_LOGI(TAG, "长按 POWER_JUDGE(GPIO%d) %dms 即可关机",
             POWER_JUDGE_GPIO, POWER_BUTTON_HOLD_MS);
}
