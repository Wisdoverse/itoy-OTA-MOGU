#include "stepper_motor.h"
#include "config.h"

#include <esp_log.h>
#include <esp_err.h>
#include <esp_rom_sys.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define TAG "stepper"

// 半步 8 拍励磁序列，每行为一个相位 [IN1, IN2, IN3, IN4]
static const uint8_t kHalfStepPatterns[8][4] = {
    {1, 0, 0, 0},
    {1, 1, 0, 0},
    {0, 1, 0, 0},
    {0, 1, 1, 0},
    {0, 0, 1, 0},
    {0, 0, 1, 1},
    {0, 0, 0, 1},
    {1, 0, 0, 1},
};

// =====================================================================
// StepperMotor
// =====================================================================

StepperMotor::StepperMotor(gpio_num_t in1, gpio_num_t in2, gpio_num_t in3, gpio_num_t in4)
    : phase_(0), step_delay_us_(MOTOR_STEP_DELAY_US) {
    pins_[0] = in1;
    pins_[1] = in2;
    pins_[2] = in3;
    pins_[3] = in4;
}

StepperMotor::~StepperMotor() {
    PowerOff();
}

void StepperMotor::Init() {
    uint64_t mask = 0;
    for (int i = 0; i < 4; ++i) {
        mask |= (1ULL << pins_[i]);
    }
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = mask;
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO 配置失败: %s", esp_err_to_name(err));
    }

    phase_ = 0;
    PowerOff();
}

void StepperMotor::WritePhase(uint8_t phase) {
    const uint8_t* p = kHalfStepPatterns[phase & 0x07];
    for (int i = 0; i < 4; ++i) {
        gpio_set_level(pins_[i], p[i]);
    }
}

void StepperMotor::Step(int steps) {
    if (steps == 0) {
        return;
    }
    int dir = (steps > 0) ? 1 : -1;
    unsigned int total = (steps > 0) ? (unsigned int)steps : (unsigned int)(-steps);

    for (unsigned int i = 0; i < total; ++i) {
        phase_ = (phase_ + dir + 8) % 8;
        WritePhase((uint8_t)phase_);
        esp_rom_delay_us(step_delay_us_);
        // 每 64 步让出一次 CPU，避免长时间占用触发看门狗
        if ((i & 0x3F) == 0x3F) {
            vTaskDelay(1);
        }
    }
}

void StepperMotor::PowerOff() {
    for (int i = 0; i < 4; ++i) {
        gpio_set_level(pins_[i], 0);
    }
}

// =====================================================================
// 串口命令驱动（测试入口）
// =====================================================================

static StepperMotor g_motor_a(MOTOR_A_IN1_GPIO, MOTOR_A_IN2_GPIO,
                              MOTOR_A_IN3_GPIO, MOTOR_A_IN4_GPIO);
static StepperMotor g_motor_b(MOTOR_B_IN1_GPIO, MOTOR_B_IN2_GPIO,
                              MOTOR_B_IN3_GPIO, MOTOR_B_IN4_GPIO);

// 互斥锁：避免命令任务与自测任务同时驱动同一个电机
static SemaphoreHandle_t g_motor_mutex = NULL;

// 28BYJ-48 半步模式下约 4096 步/圈，仅用于打印参考值
#define STEPS_PER_REV 4096

// 向命令串口格式化输出（类似 printf）
static void UartPrintf(const char* fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        uart_write_bytes(MOTOR_CMD_UART_NUM, buf, (size_t)n);
    }
}

static void PrintHelp() {
    UartPrintf("\r\n");
    UartPrintf("========================================\r\n");
    UartPrintf("     步进电机测试模块（串口命令驱动）\r\n");
    UartPrintf("========================================\r\n");
    UartPrintf(" 电机A: IN1=%d IN2=%d IN3=%d IN4=%d\r\n",
               MOTOR_A_IN1_GPIO, MOTOR_A_IN2_GPIO, MOTOR_A_IN3_GPIO, MOTOR_A_IN4_GPIO);
    UartPrintf(" 电机B: IN1=%d IN2=%d IN3=%d IN4=%d\r\n",
               MOTOR_B_IN1_GPIO, MOTOR_B_IN2_GPIO, MOTOR_B_IN3_GPIO, MOTOR_B_IN4_GPIO);
    UartPrintf("----------------------------------------\r\n");
    UartPrintf(" 命令格式:\r\n");
    UartPrintf("   A <步数> [延时ms]   电机A 正(+)反(-)\r\n");
    UartPrintf("   B <步数> [延时ms]   电机B 正(+)反(-)\r\n");
    UartPrintf("   O                   两电机断电\r\n");
    UartPrintf("   H                   显示帮助\r\n");
    UartPrintf(" 示例: A 2048   /   B -2048 3\r\n");
    UartPrintf(" (参考: %d 步/圈)\r\n", STEPS_PER_REV);
    UartPrintf("========================================\r\n");
}

static void ProcessLine(char* line) {
    char motor = 0;
    long steps = 0;
    long delay_ms = 0;
    int matched = sscanf(line, " %c %ld %ld", &motor, &steps, &delay_ms);
    motor = (char)toupper((unsigned char)motor);

    if (matched < 1 || motor == 'H' || motor == '?') {
        PrintHelp();
        return;
    }
    if (motor == 'O') {
        xSemaphoreTake(g_motor_mutex, portMAX_DELAY);
        g_motor_a.PowerOff();
        g_motor_b.PowerOff();
        xSemaphoreGive(g_motor_mutex);
        UartPrintf("[OK] 两电机已断电\r\n");
        return;
    }
    if (matched < 2) {
        UartPrintf("[错误] 缺少步数，格式: A <步数>\r\n");
        return;
    }
    if (motor != 'A' && motor != 'B') {
        UartPrintf("[错误] 未知电机 '%c'，仅支持 A / B\r\n", motor);
        return;
    }

    // 可选第三参数为每步延时(ms)，未提供则使用默认值
    uint32_t us = (matched >= 3 && delay_ms > 0) ? (uint32_t)delay_ms * 1000
                                                 : MOTOR_STEP_DELAY_US;

    UartPrintf("[运行] 电机%c 转动 %ld 步 (约 %ld 圈)...\r\n",
               motor, steps, steps / STEPS_PER_REV);
    xSemaphoreTake(g_motor_mutex, portMAX_DELAY);
    if (motor == 'A') {
        g_motor_a.SetStepDelay(us);
        g_motor_a.Step((int)steps);
    } else {
        g_motor_b.SetStepDelay(us);
        g_motor_b.Step((int)steps);
    }
    xSemaphoreGive(g_motor_mutex);
    UartPrintf("[完成] 电机%c 完成\r\n", motor);
    ESP_LOGI(TAG, "电机%c 转动 %ld 步", motor, steps);
}

// 命令接收任务：逐字节读取一行并解析执行
static void CommandTask(void* arg) {
    (void)arg;
    PrintHelp();

    char line[64];
    int idx = 0;
    uint8_t byte;

    while (true) {
        int len = uart_read_bytes(MOTOR_CMD_UART_NUM, &byte, 1, portMAX_DELAY);
        if (len <= 0) {
            continue;
        }
        // 回显，方便在串口终端查看输入
        uart_write_bytes(MOTOR_CMD_UART_NUM, (const char*)&byte, 1);

        // 回车 / 换行都作为一行结束（兼容 \r、\n、\r\n）
        if (byte == '\n' || byte == '\r') {
            line[idx] = '\0';
            idx = 0;
            if (line[0] != '\0') {
                ProcessLine(line);
            }
            continue;
        }
        if (idx < (int)sizeof(line) - 1) {
            line[idx++] = (char)byte;
        }
    }
}

#if MOTOR_SELFTEST_ENABLE
// 开机自测任务：A/B 电机小幅左右摆动一段时间，用于验证接线与方向
static void SelfTestTask(void* arg) {
    (void)arg;
    const int steps = MOTOR_SELFTEST_STEPS;

    UartPrintf("\r\n[自测] 开始：A/B 电机同时摆动 %d 步/次，持续约 %d 秒\r\n",
               steps, MOTOR_SELFTEST_DURATION_MS / 1000);
    ESP_LOGI(TAG, "开机自测：摆动 %d 步/次，持续 %d 秒", steps, MOTOR_SELFTEST_DURATION_MS / 1000);

    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(MOTOR_SELFTEST_DURATION_MS);
    int dir = 1;
    while (xTaskGetTickCount() < end) {
        int s = steps * dir;
        xSemaphoreTake(g_motor_mutex, portMAX_DELAY);
        g_motor_a.SetStepDelay(MOTOR_STEP_DELAY_US);
        g_motor_b.SetStepDelay(MOTOR_STEP_DELAY_US);
        g_motor_a.Step(s);
        g_motor_b.Step(s);
        xSemaphoreGive(g_motor_mutex);

        UartPrintf("[自测] %s %d 步\r\n", dir > 0 ? "正转" : "反转", steps);
        dir = -dir;
        vTaskDelay(pdMS_TO_TICKS(MOTOR_SELFTEST_PAUSE_MS));
    }

    xSemaphoreTake(g_motor_mutex, portMAX_DELAY);
    g_motor_a.PowerOff();
    g_motor_b.PowerOff();
    xSemaphoreGive(g_motor_mutex);

    UartPrintf("[自测] 完成，电机已断电（可继续用串口命令控制）\r\n");
    ESP_LOGI(TAG, "开机自测完成");
    vTaskDelete(NULL);
}
#endif

void StepperTestStart() {
    ESP_LOGI(TAG, "初始化步进电机测试模块");

    // 1. 初始化电机
    g_motor_a.Init();
    g_motor_b.Init();

    // 2. 创建电机互斥锁（命令任务与自测任务共享，避免同时驱动同一电机）
    g_motor_mutex = xSemaphoreCreateMutex();

    // 3. 复用 UART0（烧录/日志口 GPIO43/44）接收命令：
    //    同一个串口终端里看日志 + 发送电机命令，无需额外串口或引脚。
    //    UART0 已由控制台初始化（115200），这里只挂接 RX 驱动读字节。
    esp_err_t err = uart_driver_install(MOTOR_CMD_UART_NUM, MOTOR_CMD_UART_RX_BUF,
                                        0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "UART0 驱动安装失败: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "电机命令: 在 UART0 串口终端发送 (例如 A 2048 / B -2048 / H)");

    // 4. 创建任务：自测任务（小幅摆动 1 分钟）+ 命令接收任务，二者并行
#if MOTOR_SELFTEST_ENABLE
    xTaskCreate(SelfTestTask, "stepper_test", MOTOR_TASK_STACK_SIZE, NULL, 4, NULL);
#endif
    xTaskCreate(CommandTask, "stepper_cmd", MOTOR_TASK_STACK_SIZE, NULL, 5, NULL);
}
