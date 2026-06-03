#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_ota_ops.h>

#include "board.h"
#include "system_info.h"
#include "ota.h"

#define TAG "main"

/**
 * 检查新版本并处理设备激活流程
 *
 * 流程：
 *   1. POST 设备信息到 OTA 服务器
 *   2. 服务器返回：固件版本/下载地址、MQTT/WebSocket 配置、激活码
 *   3. 有新版本 → 自动下载升级并重启
 *   4. 有激活码 → 通过串口输出激活码，等待用户在小程序/网页绑定设备
 *   5. 激活成功 → 标记当前版本有效，流程结束
 */
static void CheckNewVersion(Ota& ota) {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10;

    while (true) {
        ESP_LOGI(TAG, "========== 正在检查服务器版本 ==========");
        ESP_LOGI(TAG, "OTA 地址: %s", ota.GetCheckVersionUrl().c_str());

        if (!ota.CheckVersion()) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "检查版本失败次数过多 (%d)，退出", MAX_RETRY);
                return;
            }
            ESP_LOGW(TAG, "检查版本失败，%d 秒后重试 (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            retry_delay *= 2;  // 每次重试延迟翻倍: 10s → 20s → 40s...
            continue;
        }
        retry_count = 0;
        retry_delay = 10;

        ESP_LOGI(TAG, "当前版本: %s", ota.GetCurrentVersion().c_str());

        // 有新版本 → 自动升级
        if (ota.HasNewVersion()) {
            ESP_LOGI(TAG, "========== 发现新版本: %s ==========", ota.GetFirmwareVersion().c_str());
            ESP_LOGI(TAG, "下载地址: %s", ota.GetFirmwareUrl().c_str());

            bool success = ota.StartUpgrade([](int progress, size_t speed) {
                ESP_LOGI(TAG, "OTA 升级进度: %d%%, 速度: %u B/s", progress, speed);
            });

            if (success) {
                ESP_LOGI(TAG, "升级成功，正在重启...");
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
                return;  // 不会到达这里
            }
            ESP_LOGE(TAG, "升级失败，继续正常运行");
        } else {
            ESP_LOGI(TAG, "已是最新版本");
        }

        // 标记当前版本有效（防止 OTA 回滚）
        ota.MarkCurrentVersionValid();

        // 无需激活 → 完成
        if (!ota.HasActivationCode() && !ota.HasActivationChallenge()) {
            ESP_LOGI(TAG, "========== 设备已激活，无需绑定 ==========");
            break;
        }

        // 有激活码 → 显示给用户
        if (ota.HasActivationCode()) {
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "╔══════════════════════════════════════╗");
            ESP_LOGI(TAG, "║        设备需要绑定激活              ║");
            ESP_LOGI(TAG, "╠══════════════════════════════════════╣");
            ESP_LOGI(TAG, "║  激活码: %-27s ║", ota.GetActivationCode().c_str());
            if (!ota.GetActivationMessage().empty()) {
                ESP_LOGI(TAG, "║  提示: %-29s ║", ota.GetActivationMessage().c_str());
            }
            ESP_LOGI(TAG, "║  请在控制台/小程序中输入激活码绑定   ║");
            ESP_LOGI(TAG, "╚══════════════════════════════════════╝");
            ESP_LOGI(TAG, "");
        }

        // 尝试激活（等待用户在服务器端完成绑定）
        ESP_LOGI(TAG, "正在等待设备激活...");
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "激活尝试 %d/%d", i + 1, 10);
            esp_err_t err = ota.Activate();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "========== 设备激活成功！ ==========");
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                // 服务器要求继续等待
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                ESP_LOGW(TAG, "激活失败，10秒后重试");
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
        }
        break;
    }

    // 输出服务器下发的配置信息
    if (ota.HasMqttConfig()) {
        ESP_LOGI(TAG, "已获取 MQTT 服务器配置");
    } else if (ota.HasWebsocketConfig()) {
        ESP_LOGI(TAG, "已获取 WebSocket 服务器配置");
    }

    if (ota.HasServerTime()) {
        ESP_LOGI(TAG, "已同步服务器时间");
    }

    ESP_LOGI(TAG, "========== 版本检查与激活流程完成 ==========");
}

extern "C" void app_main(void)
{
    // 初始化默认事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 初始化 NVS（WiFi 配置和 OTA 设置存储在这里）
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 数据损坏，正在擦除");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化开发板并启动网络（WiFi 配网或连接已知网络）
    auto& board = Board::GetInstance();
    ESP_LOGI(TAG, "开发板: %s, UUID: %s", board.GetBoardType().c_str(), board.GetUuid().c_str());

    ESP_LOGI(TAG, "========== 正在启动网络 ==========");
    board.StartNetwork();
    ESP_LOGI(TAG, "========== 网络连接完成 ==========");

    // OTA 检查版本 + 设备激活绑定
    Ota ota;
    CheckNewVersion(ota);

    ESP_LOGI(TAG, "========== itoy-OTA 启动完成 ==========");

    // 主循环 - 在此添加你的应用逻辑
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
