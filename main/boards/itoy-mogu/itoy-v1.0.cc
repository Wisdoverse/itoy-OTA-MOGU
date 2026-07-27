#include "wifi_board.h"
#include "config.h"
#include "touch_pad.h"
#include "motor_control.h"
#include "power_control.h"
#if CONFIG_ITOY_ENABLE_IMU
#include "imu_qmi8658a.h"
#endif
#include "rgb_led.h"
#include "mood_controller.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "ItoyMogu"

class ItoyMogu : public WifiBoard {
private:
    TouchPad touch_;
    MotorControl motor_;
    PowerControl power_;
#if CONFIG_ITOY_ENABLE_IMU
    ImuQMI8658A imu_;
#endif
    RgbLed rgb_;
    MoodController mood_;

public:
    ItoyMogu() {
        ESP_LOGI(TAG, "初始化 itoy-mogu 开发板 (情绪蘑菇)");

        // 1. 电源控制 (必须最先: 锁存供电)
        power_.Initialize();

        // 2. 电机控制 (创建共享 ADC + 电机, 含电池通道)
#if CONFIG_ITOY_ENABLE_MOTOR
        motor_.Initialize();
        // 3. 电池 ADC 共享句柄 (ADC 单元由电机模块创建, 关电机则无电池采集)
        power_.SetBatteryAdc(motor_.GetAdcHandle(), (adc_channel_t)BATTERY_ADC_CHAN);
#endif

        // 4. RGB 灯带 (WS2812B, GPIO38), 启动动画任务, 默认熄灭
        rgb_.Initialize();

        // 5. 触摸面板 (供 MoodController 轮询 IsPressed)
        touch_.Initialize();

#if CONFIG_ITOY_ENABLE_MOTOR
        // 6. 启动电机任务 (消费手势/驱动)
        motor_.StartMotorTask();

    #if CONFIG_ITOY_ENABLE_MOTOR_TEST
        // 电机自检: 两电机各正反转一小段, 打印电位器 (验证接线)
        ESP_LOGI(TAG, "=== 电机自检开始 ===");
        motor_.NodSteps(STEPS_FOR_DEG(10));    vTaskDelay(pdMS_TO_TICKS(300));
        motor_.NodSteps(-STEPS_FOR_DEG(10));   vTaskDelay(pdMS_TO_TICKS(300));
        motor_.ShakeSteps(STEPS_FOR_DEG(15));  vTaskDelay(pdMS_TO_TICKS(300));
        motor_.ShakeSteps(-STEPS_FOR_DEG(15));
        ESP_LOGI(TAG, "=== 电机自检完成: nod=%lu shake=%lu ===",
                 motor_.ReadNodPosition(), motor_.ReadShakePosition());
    #endif
#endif

        // 7. 情绪状态机 (消费触摸, 驱动 RGB + 电机手势), 自动进入 POWER_ON
        //    注: 关电机时 motor_ 未初始化, MoodController 的电机调用会安全空转
        mood_.Initialize(&touch_, &motor_, &rgb_, &power_);
        mood_.Start();

#if CONFIG_ITOY_ENABLE_MOTOR
        ESP_LOGI(TAG, "Nod pot=%lu, Shake pot=%lu, Batt=%dmV, RGB=%dLEDs",
                 motor_.ReadNodPosition(), motor_.ReadShakePosition(),
                 power_.ReadBatteryMv(), rgb_.count());
#else
        ESP_LOGI(TAG, "RGB=%dLEDs (motor disabled, Batt 未采集)", rgb_.count());
#endif

        // 8. IMU (QMI8658A) - 可选, 失败不影响启动
#if CONFIG_ITOY_ENABLE_IMU
        esp_err_t imu_ret = imu_.Initialize();
        if (imu_ret == ESP_OK) {
            ImuData data;
            if (imu_.ReadData(data) == ESP_OK) {
                ESP_LOGI(TAG, "IMU: accel(%.2f,%.2f,%.2f)g gyro(%.1f,%.1f,%.1f)dps temp=%.1fC",
                         data.accel_x, data.accel_y, data.accel_z,
                         data.gyro_x, data.gyro_y, data.gyro_z,
                         data.temp);
            }
        } else {
            ESP_LOGW(TAG, "IMU not available, continuing without it");
        }
#endif

        ESP_LOGI(TAG, "=== itoy-mogu 情绪蘑菇初始化完成 ===");
    }

    std::string GetBoardType() override {
        return "itoy-mogu";
    }

    // 暴露子系统给应用层
    TouchPad& GetTouch() { return touch_; }
    MotorControl& GetMotor() { return motor_; }
    PowerControl& GetPower() { return power_; }
#if CONFIG_ITOY_ENABLE_IMU
    ImuQMI8658A& GetImu() { return imu_; }
#endif
    RgbLed& GetRgb() { return rgb_; }
    MoodController& GetMood() { return mood_; }
};

DECLARE_BOARD(ItoyMogu)
