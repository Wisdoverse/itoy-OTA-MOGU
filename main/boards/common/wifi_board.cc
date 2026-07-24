#include "wifi_board.h"
#include "system_info.h"
#include "settings.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_network.h>
#include <esp_log.h>

#include <wifi_station.h>
#include <wifi_configuration_ap.h>
#include <ssid_manager.h>

#if CONFIG_ITOY_PROVISIONING_BLE
#include "wifi_provisioning_ble.h"
#endif

static const char *TAG = "WifiBoard";

// 按 Kconfig 配网方式分发
void WifiBoard::EnterConfigMode() {
#if CONFIG_ITOY_PROVISIONING_BLE
    EnterBleProvisioningMode();
#else
    EnterWifiConfigMode();
#endif
}

void WifiBoard::EnterBleProvisioningMode() {
#if CONFIG_ITOY_PROVISIONING_BLE
    ESP_LOGI(TAG, "Entering BLE provisioning mode");
    WifiProvisioningBle prov;
    prov.Start();   // 阻塞; 成功后内部 esp_restart
#else
    EnterWifiConfigMode();   // 未启用 BLE, 回退 SoftAP
#endif
}

WifiBoard::WifiBoard() {
    Settings settings("wifi", true);
    wifi_config_mode_ = settings.GetInt("force_ap") == 1;
    if (wifi_config_mode_) {
        ESP_LOGI(TAG, "force_ap is set to 1, reset to 0");
        settings.SetInt("force_ap", 0);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::EnterWifiConfigMode() {
    auto& wifi_ap = WifiConfigurationAp::GetInstance();
    wifi_ap.SetSsidPrefix("Itoy");
    wifi_ap.Start();

    ESP_LOGI(TAG, "WiFi config mode started. SSID: %s, URL: %s",
             wifi_ap.GetSsid().c_str(), wifi_ap.GetWebServerUrl().c_str());

    // Wait forever until reset after configuration
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void WifiBoard::StartNetwork() {
    if (wifi_config_mode_) {
        EnterConfigMode();
        return;
    }
    ESP_LOGI(TAG, "Starting WiFi in station mode...");

    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    if (ssid_list.empty()) {
        wifi_config_mode_ = true;
        EnterConfigMode();
        return;
    }

    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.Start();

    if (!wifi_station.WaitForConnected(60 * 1000)) {
        wifi_station.Stop();
        wifi_config_mode_ = true;
        ESP_LOGI(TAG, "Failed to connect to WiFi, entering config mode");
        EnterConfigMode();
        return;
    }

    ESP_LOGI(TAG, "WiFi connected. SSID: %s, IP: %s",
             wifi_station.GetSsid().c_str(), wifi_station.GetIpAddress().c_str());
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

std::string WifiBoard::GetBoardJson() {
    auto& wifi_station = WifiStation::GetInstance();
    std::string board_json = R"({)";
    board_json += R"("type":")" + std::string(BOARD_TYPE) + R"(",)";
    board_json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";
    if (!wifi_config_mode_) {
        board_json += R"("ssid":")" + wifi_station.GetSsid() + R"(",)";
        board_json += R"("rssi":)" + std::to_string(wifi_station.GetRssi()) + R"(,)";
        board_json += R"("channel":)" + std::to_string(wifi_station.GetChannel()) + R"(,)";
        board_json += R"("ip":")" + wifi_station.GetIpAddress() + R"(",)";
    }
    board_json += R"("mac":")" + SystemInfo::GetMacAddress() + R"(")";
    board_json += R"(})";
    return board_json;
}

void WifiBoard::SetPowerSaveMode(bool enabled) {
    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.SetPowerSaveMode(enabled);
}

void WifiBoard::ResetWifiConfiguration() {
    {
        Settings settings("wifi", true);
        settings.SetInt("force_ap", 1);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}
