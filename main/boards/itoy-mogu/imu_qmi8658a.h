#ifndef IMU_QMI8658A_H_
#define IMU_QMI8658A_H_

#include <driver/i2c_master.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <array>

struct ImuData {
    float accel_x;  // g
    float accel_y;  // g
    float accel_z;  // g
    float gyro_x;   // deg/s
    float gyro_y;   // deg/s
    float gyro_z;   // deg/s
    float temp;     // °C
    uint32_t timestamp_ms;
};

// QMI8658A 寄存器地址
namespace qmi8658a {
    constexpr uint8_t ADDR = 0x6B;

    // WHO_AM_I
    constexpr uint8_t REG_WHO_AM_I = 0x00;
    constexpr uint8_t WHO_AM_I_VAL = 0x05;

    // 控制寄存器
    constexpr uint8_t REG_CTRL1 = 0x02;  // 加速度计控制
    constexpr uint8_t REG_CTRL2 = 0x03;  // 陀螺仪控制
    constexpr uint8_t REG_CTRL3 = 0x04;  // 模式控制

    // 数据寄存器
    constexpr uint8_t REG_AX_L = 0x35;
    constexpr uint8_t REG_GX_L = 0x3B;
    constexpr uint8_t REG_TEMP_L = 0x41;

    // 加速度计量程
    enum AccelRange : uint8_t {
        ACCEL_2G  = 0b00 << 4,
        ACCEL_4G  = 0b01 << 4,
        ACCEL_8G  = 0b10 << 4,
        ACCEL_16G = 0b11 << 4,
    };

    // 陀螺仪量程
    enum GyroRange : uint8_t {
        GYRO_256DPS  = 0b00 << 4,
        GYRO_512DPS  = 0b01 << 4,
        GYRO_1024DPS = 0b10 << 4,
        GYRO_2048DPS = 0b11 << 4,
    };

    // 输出数据率
    enum Odr : uint8_t {
        ODR_1000Hz = 0b0101 << 0,
        ODR_500Hz  = 0b0110 << 0,
        ODR_250Hz  = 0b0111 << 0,
        ODR_125Hz  = 0b1000 << 0,
        ODR_62Hz   = 0b1001 << 0,
        ODR_31Hz   = 0b1010 << 0,
    };
}

class ImuQMI8658A {
public:
    ImuQMI8658A();
    ~ImuQMI8658A();

    // 初始化 I2C + IMU
    esp_err_t Initialize();

    // 检查设备是否在线
    bool IsConnected() const { return connected_; }

    // 读取全部数据
    esp_err_t ReadData(ImuData& data);

    // 读取加速度
    esp_err_t ReadAccel(float& x, float& y, float& z);

    // 读取陀螺仪
    esp_err_t ReadGyro(float& x, float& y, float& z);

    // 读取温度
    esp_err_t ReadTemperature(float& temp);

    // 设置量程
    esp_err_t SetAccelRange(qmi8658a::AccelRange range);
    esp_err_t SetGyroRange(qmi8658a::GyroRange range);

private:
 esp_err_t WriteReg(uint8_t reg, uint8_t val);
    esp_err_t ReadReg(uint8_t reg, uint8_t* val);
    esp_err_t ReadRegs(uint8_t reg, uint8_t* buf, size_t len);

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    i2c_master_dev_handle_t i2c_dev_ = nullptr;
    bool connected_ = false;
    SemaphoreHandle_t mutex_ = nullptr;

    float accel_scale_ = 4.0f / 32768.0f;  // 默认 ±4g
    float gyro_scale_ = 512.0f / 32768.0f; // 默认 ±512dps
};

#endif // IMU_QMI8658A_H_
