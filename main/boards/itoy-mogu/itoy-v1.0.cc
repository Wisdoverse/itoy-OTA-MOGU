#include "wifi_board.h"
#include "config.h"
#include "touch_pad.h"
#include "motor_control.h"
#include "power_control.h"
#include "imu_qmi8658a.h"
#include "rgb_led.h"

#include <esp_log.h>

#define TAG "ItoyMogu"

class ItoyMogu : public WifiBoard {
private:
    TouchPad touch_;
    MotorControl motor_;
    PowerControl power_;
    ImuQMI8658A imu_;
    RgbLed rgb_;

    // 触摸事件 -> 电机驱动 (任意 pad 状态变化时重算两个电机方向)
    void OnTouchEvent(int channel, bool pressed) {
        ESP_LOGI(TAG, "Touch ch%d (GPIO%d) %s",
                 channel, channel + 1, pressed ? "PRESSED" : "RELEASED");

        // 根据当前 4 路触摸实时状态决定每个电机方向 (支持多指/释放正确)
        // ch0=点头前, ch1=点头后, ch2=摇头左, ch3=摇头右
        int nod_dir = 0, shake_dir = 0;
        if (touch_.IsPressed(0))      nod_dir = +1;
        else if (touch_.IsPressed(1)) nod_dir = -1;
        if (touch_.IsPressed(2))      shake_dir = +1;
        else if (touch_.IsPressed(3)) shake_dir = -1;

        motor_.Drive(MOTOR_NOD, nod_dir);
        motor_.Drive(MOTOR_SHAKE, shake_dir);
    }

public:
    ItoyMogu() {
        ESP_LOGI(TAG, "初始化 itoy-mogu 开发板 (网表重构版)");

        // 1. 电源控制 (必须最先: 锁存供电)
        power_.Initialize();

        // 2. 电机控制 (创建共享 ADC + 电机, 含电池通道)
        motor_.Initialize();

        // 3. 电池 ADC 共享句柄 (电源读电池用)
        power_.SetBatteryAdc(motor_.GetAdcHandle(), (adc_channel_t)BATTERY_ADC_CHAN);

        // 4. RGB 灯带 (WS2812B, GPIO38 经 U13 连接器), 上电默认熄灭
        rgb_.Initialize();

        // 5. 触摸面板 + 绑定控制逻辑
        touch_.Initialize();
        touch_.SetCallback([this](int channel, bool pressed) {
            this->OnTouchEvent(channel, pressed);
        });
        touch_.StartScanTask(30);   // 30ms 轮询

        // 6. 启动电机步进消费任务 (按住即动 + 软限位)
        motor_.StartMotorTask();

        ESP_LOGI(TAG, "Nod pot=%lu, Shake pot=%lu, Batt=%dmV, RGB=%dLEDs",
                 motor_.ReadNodPosition(), motor_.ReadShakePosition(),
                 power_.ReadBatteryMv(), rgb_.count());

        // 7. IMU (QMI8658A) - 可选, 失败不影响启动
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

        ESP_LOGI(TAG, "=== itoy-mogu 全部外设初始化完成 ===");
    }

    std::string GetBoardType() override {
        return "itoy-mogu";
    }

    // 暴露子系统给应用层
    TouchPad& GetTouch() { return touch_; }
    MotorControl& GetMotor() { return motor_; }
    PowerControl& GetPower() { return power_; }
    ImuQMI8658A& GetImu() { return imu_; }
    RgbLed& GetRgb() { return rgb_; }
};

DECLARE_BOARD(ItoyMogu)
