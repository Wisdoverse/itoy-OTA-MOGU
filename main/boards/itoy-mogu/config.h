#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/adc.h>

// ============================================================
// itoy-mogu PCB GPIO 引脚定义 (按 PADS 网表核对)
// MCU: ESP32-S3-WROOM-1U (N8R8)
// 音频: INMP441 (I2S 麦克风) + MAX98357 (I2S 功放)
// IMU:  QMI8658A (I2C)
// 电机: 2 路 ULN2003 步进 (点头 + 摇头)
// ============================================================

// --- Touch (4 路触摸铜片) ---
// ESP32-S3: Touch1=GPIO1, Touch2=GPIO2, Touch3=GPIO3, Touch4=GPIO4
// GPIO0 为 BOOT 键, 不是触摸通道
#define TOUCH_PAD_1_GPIO    GPIO_NUM_1
#define TOUCH_PAD_2_GPIO    GPIO_NUM_2
#define TOUCH_PAD_3_GPIO    GPIO_NUM_3
#define TOUCH_PAD_4_GPIO    GPIO_NUM_4
#define TOUCH_PAD_COUNT     4

// --- ADC (电位器位置反馈 + 电池电压) ---
// ESP32-S3 ADC1: GPIO5=CH4, GPIO6=CH5, GPIO7=CH6
#define POT_NOD_GPIO        GPIO_NUM_6    // 点头电位器 U12 (前后)
#define POT_SHAKE_GPIO      GPIO_NUM_5    // 摇头电位器 U5  (左右)
#define POT_NOD_CHAN        ADC1_CHANNEL_5   // GPIO6
#define POT_SHAKE_CHAN      ADC1_CHANNEL_4   // GPIO5
#define BATTERY_GPIO        GPIO_NUM_7    // 电池电压检测 (BAT)
#define BATTERY_ADC_CHAN    ADC1_CHANNEL_6   // GPIO7
#define POT_ADC_ATTEN       ADC_ATTEN_DB_12
#define POT_ADC_WIDTH       ADC_BITWIDTH_12
#define POT_MAX_VALUE       4095

// 电位器软限位 (量程百分比, 防止机械顶死), 可按实物调
#define POT_RANGE_MIN_PCT   10
#define POT_RANGE_MAX_PCT   90

// --- I2S 麦克风 (INMP441) ---
// INMP441: WS, SD(OUT), SCK(BCLK) — 标准 I2S 从设备
#define MIC_I2S_WS_GPIO     GPIO_NUM_8
#define MIC_I2S_SD_GPIO     GPIO_NUM_9
#define MIC_I2S_SCK_GPIO    GPIO_NUM_11

// --- I2S 喇叭功放 (MAX98357) ---
// MAX98357: DIN, BCLK, LRCLK — I2S 接收端
#define SPK_I2S_DIN_GPIO    GPIO_NUM_10
#define SPK_I2S_BCLK_GPIO   GPIO_NUM_12
#define SPK_I2S_LRCLK_GPIO  GPIO_NUM_13

// --- I2C (QMI8658A IMU) ---
#define IMU_I2C_SDA_GPIO    GPIO_NUM_14
#define IMU_I2C_SCL_GPIO    GPIO_NUM_15
#define IMU_I2C_PORT        I2C_NUM_0
#define IMU_I2C_FREQ_HZ     400000
#define QMI8658A_ADDR       0x6B

// --- USB ---
// GPIO19/20 为 USB D+/D-, 烧录口, 不可他用

// --- 电源控制 ---
#define POWER_ON_GPIO       GPIO_NUM_39   // 按键检测 (SW4 经 Q3 拉低)
#define POWER_LATCH_GPIO    GPIO_NUM_42   // ON 锁存信号, 拉低触发软关机

// --- 电机1: 点头 U17 (前后) via U9 ---
#define MOTOR_NOD_A_GPIO    GPIO_NUM_41
#define MOTOR_NOD_B_GPIO    GPIO_NUM_40
#define MOTOR_NOD_C_GPIO    GPIO_NUM_48
#define MOTOR_NOD_D_GPIO    GPIO_NUM_47
#define MOTOR_NOD_INVERT    0   // 1 = 翻转正/负方向 (装机方向不对时改这里)

// --- 电机2: 摇头 U16 (左右) via U8 ---
#define MOTOR_SHAKE_A_GPIO  GPIO_NUM_21
#define MOTOR_SHAKE_B_GPIO  GPIO_NUM_18
#define MOTOR_SHAKE_C_GPIO  GPIO_NUM_17
#define MOTOR_SHAKE_D_GPIO  GPIO_NUM_16
#define MOTOR_SHAKE_INVERT  0   // 1 = 翻转正/负方向

// --- 步进电机参数 ---
#define MOTOR_STEP_DELAY_MS 2       // 步进间隔 (ms), 越小越快
// 28BYJ-48 + ULN2003, 8 拍半步模式: 约 4096 步/圈 (1:64 减速)
#define MOTOR_STEPS_PER_REV 4096

// 电位器方向标定: 正转(cw)是否使该轴电位器读数增大
// 1 = 增大, 0 = 减小。实测若软限位在刚起步就立即触发, 把对应项取反
#define MOTOR_NOD_POT_CW_INC   1
#define MOTOR_SHAKE_POT_CW_INC 1

#endif // _BOARD_CONFIG_H_
