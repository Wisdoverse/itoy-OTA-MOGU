#include "imu_qmi8658a.h"
#include "config.h"
#include <esp_log.h>
#include <cstring>

#define TAG "QMI8658A"

ImuQMI8658A::ImuQMI8658A() {
    mutex_ = xSemaphoreCreateMutex();
}

ImuQMI8658A::~ImuQMI8658A() {
    if (i2c_dev_) {
        i2c_master_bus_rm_device(i2c_dev_);
    }
    if (i2c_bus_) {
        i2c_del_master_bus(i2c_bus_);
    }
    if (mutex_) {
        vSemaphoreDelete(mutex_);
    }
}

esp_err_t ImuQMI8658A::WriteReg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(i2c_dev_, buf, 2, 100);
}

esp_err_t ImuQMI8658A::ReadReg(uint8_t reg, uint8_t* val) {
    return i2c_master_transmit_receive(i2c_dev_, &reg, 1, val, 1, 100);
}

esp_err_t ImuQMI8658A::ReadRegs(uint8_t reg, uint8_t* buf, size_t len) {
    return i2c_master_transmit_receive(i2c_dev_, &reg, 1, buf, len, 100);
}

esp_err_t ImuQMI8658A::Initialize() {
    if (connected_) return ESP_OK;

    // 初始化 I2C 总线
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = (i2c_port_t)IMU_I2C_PORT,
        .sda_io_num = IMU_I2C_SDA_GPIO,
        .scl_io_num = IMU_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_new_master_bus(&i2c_cfg, &i2c_bus_));
    if (!i2c_bus_) {
        ESP_LOGE(TAG, "Failed to create I2C bus");
        return ESP_FAIL;
    }

    // 创建 I2C 设备
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMI8658A_ADDR,
        .scl_speed_hz = IMU_I2C_FREQ_HZ,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        },
    };
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &i2c_dev_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device");
        return ret;
    }

    // 检查 WHO_AM_I
    uint8_t who = 0;
    ret = ReadReg(qmi8658a::REG_WHO_AM_I, &who);
    if (ret != ESP_OK || who != qmi8658a::WHO_AM_I_VAL) {
        ESP_LOGE(TAG, "QMI8658A not found (WHO_AM_I=0x%02X, expect 0x%02X)",
                 who, qmi8658a::WHO_AM_I_VAL);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "QMI8658A found at 0x%02X", QMI8658A_ADDR);

    xSemaphoreTake(mutex_, portMAX_DELAY);

    // 复位
    WriteReg(0x02, 0x00);  // CTRL1 = 0 (standby)
    WriteReg(0x03, 0x00);  // CTRL2 = 0
    WriteReg(0x04, 0x00);  // CTRL3 = 0
    vTaskDelay(pdMS_TO_TICKS(10));

    // CTRL1: 加速度计使能, ±4g, 500Hz
    // bit7:6=range, bit3:0=ODR, bit0=enable
    WriteReg(qmi8658a::REG_CTRL1,
             (uint8_t)qmi8658a::ACCEL_4G | (uint8_t)qmi8658a::ODR_500Hz | 0x01);

    // CTRL2: 陀螺仪使能, ±512dps, 500Hz
    WriteReg(qmi8658a::REG_CTRL2,
             (uint8_t)qmi8658a::GYRO_512DPS | (uint8_t)qmi8658a::ODR_500Hz | 0x01);

    // CTRL3: 正常模式
    WriteReg(qmi8658a::REG_CTRL3, 0x01);

    vTaskDelay(pdMS_TO_TICKS(50));

    xSemaphoreGive(mutex_);

    connected_ = true;
    ESP_LOGI(TAG, "QMI8658A initialized (accel ±4g, gyro ±512dps, 500Hz)");
    return ESP_OK;
}

esp_err_t ImuQMI8658A::SetAccelRange(qmi8658a::AccelRange range) {
    if (!connected_) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mutex_, portMAX_DELAY);

    uint8_t ctrl1;
    ReadReg(qmi8658a::REG_CTRL1, &ctrl1);
    ctrl1 = (ctrl1 & 0x0F) | (uint8_t)range;
    WriteReg(qmi8658a::REG_CTRL1, ctrl1);

    switch (range) {
        case qmi8658a::ACCEL_2G:  accel_scale_ = 2.0f / 32768.0f; break;
        case qmi8658a::ACCEL_4G:  accel_scale_ = 4.0f / 32768.0f; break;
        case qmi8658a::ACCEL_8G:  accel_scale_ = 8.0f / 32768.0f; break;
        case qmi8658a::ACCEL_16G: accel_scale_ = 16.0f / 32768.0f; break;
    }

    xSemaphoreGive(mutex_);
    return ESP_OK;
}

esp_err_t ImuQMI8658A::SetGyroRange(qmi8658a::GyroRange range) {
    if (!connected_) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mutex_, portMAX_DELAY);

    uint8_t ctrl2;
    ReadReg(qmi8658a::REG_CTRL2, &ctrl2);
    ctrl2 = (ctrl2 & 0x0F) | (uint8_t)range;
    WriteReg(qmi8658a::REG_CTRL2, ctrl2);

    switch (range) {
        case qmi8658a::GYRO_256DPS:  gyro_scale_ = 256.0f / 32768.0f; break;
        case qmi8658a::GYRO_512DPS:  gyro_scale_ = 512.0f / 32768.0f; break;
        case qmi8658a::GYRO_1024DPS: gyro_scale_ = 1024.0f / 32768.0f; break;
        case qmi8658a::GYRO_2048DPS: gyro_scale_ = 2048.0f / 32768.0f; break;
    }

    xSemaphoreGive(mutex_);
    return ESP_OK;
}

static int16_t ToInt16(uint8_t hi, uint8_t lo) {
    return (int16_t)((hi << 8) | lo);
}

esp_err_t ImuQMI8658A::ReadData(ImuData& data) {
    if (!connected_) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(mutex_, portMAX_DELAY);

    uint8_t buf[14];
    esp_err_t ret = ReadRegs(qmi8658a::REG_AX_L, buf, 14);
    if (ret != ESP_OK) {
        xSemaphoreGive(mutex_);
        return ret;
    }

    // QMI8658A 输出顺序: X_L, X_H, Y_L, Y_H, Z_L, Z_H
    data.accel_x = ToInt16(buf[1], buf[0]) * accel_scale_;
    data.accel_y = ToInt16(buf[3], buf[2]) * accel_scale_;
    data.accel_z = ToInt16(buf[5], buf[4]) * accel_scale_;

    data.gyro_x = ToInt16(buf[7], buf[6]) * gyro_scale_;
    data.gyro_y = ToInt16(buf[9], buf[8]) * gyro_scale_;
    data.gyro_z = ToInt16(buf[11], buf[10]) * gyro_scale_;

    // 温度: 12-bit signed, 1 LSB = 0.0625°C, offset 25°C
    int16_t temp_raw = ToInt16(buf[13], buf[12]) >> 4;
    data.temp = temp_raw * 0.0625f + 25.0f;

    data.timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    xSemaphoreGive(mutex_);
    return ESP_OK;
}

esp_err_t ImuQMI8658A::ReadAccel(float& x, float& y, float& z) {
    ImuData data;
    esp_err_t ret = ReadData(data);
    x = data.accel_x;
    y = data.accel_y;
    z = data.accel_z;
    return ret;
}

esp_err_t ImuQMI8658A::ReadGyro(float& x, float& y, float& z) {
    ImuData data;
    esp_err_t ret = ReadData(data);
    x = data.gyro_x;
    y = data.gyro_y;
    z = data.gyro_z;
    return ret;
}

esp_err_t ImuQMI8658A::ReadTemperature(float& temp) {
    if (!connected_) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(mutex_, portMAX_DELAY);
    uint8_t buf[2];
    esp_err_t ret = ReadRegs(qmi8658a::REG_TEMP_L, buf, 2);
    if (ret == ESP_OK) {
        int16_t temp_raw = ToInt16(buf[1], buf[0]) >> 4;
        temp = temp_raw * 0.0625f + 25.0f;
    }
    xSemaphoreGive(mutex_);
    return ret;
}
