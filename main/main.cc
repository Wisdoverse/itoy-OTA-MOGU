#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_ota_ops.h>
#include "sdkconfig.h"

#include "board.h"
#include "system_info.h"
#include "ota.h"

#define TAG "main"

/**
 * 检查新版本并拉取云端配置
 *
 * 流程：
 *   1. POST 设备信息到 OTA 服务器
 *   2. 服务器返回：固件版本/下载地址、MQTT/WebSocket 配置、server_time
 *   3. 有新版本 → 自动下载升级并重启
 *   4. 标记当前版本有效（防止回滚），流程结束
 */
#if !CONFIG_ITOY_ENABLE_DEBUG_MODE && !CONFIG_ITOY_ENABLE_MOTOR_SELFTEST
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

    ESP_LOGI(TAG, "========== 版本检查流程完成 ==========");
}
#endif  // !CONFIG_ITOY_ENABLE_DEBUG_MODE

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

    // OTA 检查版本 (固件 / MQTT / WebSocket 配置 / server_time)
    // 调试模式是 AP-only(无 STA), 跳过 OTA: 否则 OTA 里 GetBoardJson->GetRssi()
    // 会因无 STA 连接 ESP_ERROR_CHECK abort 导致重启循环。
#if !CONFIG_ITOY_ENABLE_DEBUG_MODE && !CONFIG_ITOY_ENABLE_MOTOR_SELFTEST
    Ota ota;
    CheckNewVersion(ota);
    // 启动后端实时通道 (MQTT/WebSocket + MCP), 仅正常模式; OTA 已写入连接配置到 NVS
    board.StartBackendService();
#endif

    ESP_LOGI(TAG, "========== itoy-OTA 启动完成 ==========");

    // 主循环 - 在此添加你的应用逻辑
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
