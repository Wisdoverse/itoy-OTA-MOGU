#include "wifi_board.h"
#include "config.h"
#include "motor_control.h"
#include "power_control.h"
#include "rgb_led.h"

#include <esp_log.h>

#if CONFIG_ITOY_ENABLE_DEBUG_MODE
// 调试模式: WiFi 热点 + 网页调试 (控电机/电位器/电池/日志), 跳过情绪与配网
#include "debug_web.h"
#else
// 正常模式: 情绪蘑菇 + 触摸/IMU/配网
#include "touch_pad.h"
#if CONFIG_ITOY_ENABLE_IMU
#include "imu_qmi8658a.h"
#endif
#include "mood_controller.h"
#endif

#define TAG "ItoyMogu"

class ItoyMogu : public WifiBoard {
private:
    MotorControl motor_;
    PowerControl power_;
    RgbLed rgb_;
#if CONFIG_ITOY_ENABLE_DEBUG_MODE
    DebugWeb debug_;
#else
    TouchPad touch_;
#if CONFIG_ITOY_ENABLE_IMU
    ImuQMI8658A imu_;
#endif
    MoodController mood_;
#endif

public:
    ItoyMogu() {
        ESP_LOGI(TAG, "初始化 itoy-mogu 开发板 (%s)",
#if CONFIG_ITOY_ENABLE_MOTOR_SELFTEST
                 "电机自检模式"
#elif CONFIG_ITOY_ENABLE_DEBUG_MODE
                 "调试模式"
#else
                 "情绪蘑菇"
#endif
        );

        // 电源控制 (必须最先: 锁存供电)
        power_.Initialize();

#if CONFIG_ITOY_ENABLE_MOTOR_SELFTEST
        // ---- 电机自检模式: 无网络/无调试, 仅正反转各 1 圈, 验证电机本身 ----
        // 不起 WiFi, 排除 WiFi brownout 干扰; 8192 步 = 输出轴 1 圈 (半步+1:64)
        motor_.Initialize();
        const int kRevSteps = MOTOR_SELFTEST_REV * MOTOR_STEPS_PER_REV;   // N 圈的步数
        ESP_LOGI(TAG, "=== 电机自检: 持续正反转 (无网络; %d 圈/段, %dms/步), 断电才停 ===",
                 MOTOR_SELFTEST_REV, MOTOR_SELFTEST_DELAY_MS);
        while (true) {
            ESP_LOGI(TAG, "[点头] 正 %d 圈", MOTOR_SELFTEST_REV);
            motor_.MoveSteps(MOTOR_NOD,    kRevSteps, MOTOR_SELFTEST_DELAY_MS);
            ESP_LOGI(TAG, "[点头] 反 %d 圈", MOTOR_SELFTEST_REV);
            motor_.MoveSteps(MOTOR_NOD,   -kRevSteps, MOTOR_SELFTEST_DELAY_MS);
            ESP_LOGI(TAG, "[摇头] 正 %d 圈", MOTOR_SELFTEST_REV);
            motor_.MoveSteps(MOTOR_SHAKE,  kRevSteps, MOTOR_SELFTEST_DELAY_MS);
            ESP_LOGI(TAG, "[摇头] 反 %d 圈", MOTOR_SELFTEST_REV);
            motor_.MoveSteps(MOTOR_SHAKE, -kRevSteps, MOTOR_SELFTEST_DELAY_MS);
        }
#elif CONFIG_ITOY_ENABLE_DEBUG_MODE
        // ---- 调试模式: 只初始化电机(含电池 ADC) + RGB; 跳过触摸/IMU/情绪 ----
        motor_.Initialize();
        power_.SetBatteryAdc(motor_.GetAdcHandle(), (adc_channel_t)BATTERY_ADC_CHAN);
        rgb_.Initialize();   // 默认熄灭
        ESP_LOGI(TAG, "调试模式: motor + adc + rgb 就绪 (网页由 StartNetwork 启动)");
#else
        // ---- 正常模式 ----
#if CONFIG_ITOY_ENABLE_MOTOR
        motor_.Initialize();
        power_.SetBatteryAdc(motor_.GetAdcHandle(), (adc_channel_t)BATTERY_ADC_CHAN);
#endif
        rgb_.Initialize();
        touch_.Initialize();
#if CONFIG_ITOY_ENABLE_MOTOR
        motor_.StartMotorTask();
#endif
        mood_.Initialize(&touch_, &motor_, &rgb_, &power_);
        mood_.Start();

#if CONFIG_ITOY_ENABLE_MOTOR
        ESP_LOGI(TAG, "Nod pot=%lu, Shake pot=%lu, Batt=%dmV, RGB=%dLEDs",
                 motor_.ReadNodPosition(), motor_.ReadShakePosition(),
                 power_.ReadBatteryMv(), rgb_.count());
#else
        ESP_LOGI(TAG, "RGB=%dLEDs (motor disabled, Batt 未采集)", rgb_.count());
#endif

#if CONFIG_ITOY_ENABLE_IMU
        esp_err_t imu_ret = imu_.Initialize();
        if (imu_ret == ESP_OK) {
            ImuData data;
            if (imu_.ReadData(data) == ESP_OK) {
                ESP_LOGI(TAG, "IMU: accel(%.2f,%.2f,%.2f)g gyro(%.1f,%.1f,%.1f)dps temp=%.1fC",
                         data.accel_x, data.accel_y, data.accel_z,
                         data.gyro_x, data.gyro_y, data.gyro_z, data.temp);
            }
        } else {
            ESP_LOGW(TAG, "IMU not available, continuing without it");
        }
#endif
        ESP_LOGI(TAG, "=== itoy-mogu 情绪蘑菇初始化完成 ===");
#endif
    }

    std::string GetBoardType() override {
        return "itoy-mogu";
    }

    // 调试模式: 起网页调试 AP (不调正常配网, 避免与调试 AP 冲突); 否则正常配网
    void StartNetwork() override {
#if CONFIG_ITOY_ENABLE_MOTOR_SELFTEST
        ESP_LOGI(TAG, "电机自检模式: 跳过网络");
#elif CONFIG_ITOY_ENABLE_DEBUG_MODE
        debug_.Start(&motor_, &power_);
#else
        WifiBoard::StartNetwork();
#endif
    }

    // 暴露子系统给应用层 (调试模式下仅 motor/power/rgb 可用)
    MotorControl& GetMotor() { return motor_; }
    PowerControl& GetPower() { return power_; }
    RgbLed& GetRgb() { return rgb_; }
#if !CONFIG_ITOY_ENABLE_DEBUG_MODE
    TouchPad& GetTouch() { return touch_; }
    MoodController& GetMood() { return mood_; }
#if CONFIG_ITOY_ENABLE_IMU
    ImuQMI8658A& GetImu() { return imu_; }
#endif
#endif
};

DECLARE_BOARD(ItoyMogu)
