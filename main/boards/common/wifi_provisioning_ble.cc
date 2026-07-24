#include "sdkconfig.h"
#include "wifi_provisioning_ble.h"

#if CONFIG_ITOY_PROVISIONING_BLE

#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

#include "ssid_manager.h"

#define TAG "ProvBLE"

// 安全等级 + PoP (Proof of Possession)。手机 App 连接时需输入此 PoP。
#define PROV_SECURITY_TYPE  WIFI_PROV_SECURITY_1
#define PROV_POP            "itoy123456"

static char s_ssid[33] = {0};
static char s_password[65] = {0};
static bool s_got_creds = false;

void WifiProvisioningBle::ProvEventHandler(void* /*arg*/, esp_event_base_t base,
                                           int32_t id, void* data) {
    if (base != WIFI_PROV_EVENT) return;
    switch (id) {
        case WIFI_PROV_CRED_RECV: {
            wifi_sta_config_t* cfg = (wifi_sta_config_t*)data;
            snprintf(s_ssid, sizeof(s_ssid), "%s", (const char*)cfg->ssid);
            snprintf(s_password, sizeof(s_password), "%s", (const char*)cfg->password);
            s_got_creds = true;
            ESP_LOGI(TAG, "recv SSID='%s'", s_ssid);
            break;
        }
        case WIFI_PROV_CRED_FAIL: {
            int reason = data ? *(int*)data : -1;
            ESP_LOGE(TAG, "cred fail reason=%d (凭据错误或连不上, App 会重试)", reason);
            s_got_creds = false;
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "cred success -> 写入 SsidManager");
            if (s_got_creds) {
                SsidManager::GetInstance().Clear();
                SsidManager::GetInstance().AddSsid(s_ssid, s_password);
            }
            break;
        case WIFI_PROV_END:
            ESP_LOGI(TAG, "provisioning end -> reboot 进入正常连接流程");
            wifi_prov_mgr_deinit();
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
            break;
        default:
            break;
    }
}

void WifiProvisioningBle::Start() {
    // 注册配网事件
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                               ProvEventHandler, NULL));

    // BLE scheme: 配网结束后释放 BT 资源 (本设备仅配网时用 BT, 省内存)
    wifi_prov_mgr_config_t cfg = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    ESP_ERROR_CHECK(wifi_prov_mgr_init(cfg));

    // service name: Itoy-XXXX (MAC 末 2 字节)
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    char service_name[16];
    snprintf(service_name, sizeof(service_name), "Itoy-%02X%02X", mac[4], mac[5]);

    ESP_LOGI(TAG, "================ BLE 配网 ================");
    ESP_LOGI(TAG, "service='%s'  security=1  PoP='%s'", service_name, PROV_POP);
    ESP_LOGI(TAG, "用 'ESP BLE Provisioning' App 扫描, 选中 %s,", service_name);
    ESP_LOGI(TAG, "输入 PoP=%s, 选 WiFi 并输入密码。", PROV_POP);
    ESP_LOGI(TAG, "==========================================");

    ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(PROV_SECURITY_TYPE, PROV_POP,
                                                     service_name, NULL));

    // 阻塞; 成功后事件处理器会 esp_restart
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

#endif  // CONFIG_ITOY_PROVISIONING_BLE
