#ifndef WIFI_PROVISIONING_BLE_H_
#define WIFI_PROVISIONING_BLE_H_

#include "sdkconfig.h"   // CONFIG_ITOY_PROVISIONING_BLE

#include <stdint.h>
#include <esp_event.h>   // esp_event_base_t

// 蓝牙 BLE 配网, 仅在 menuconfig 选中 ITOY_PROVISIONING_BLE 时编译
#if CONFIG_ITOY_PROVISIONING_BLE

// BLE 配网 (ESP-IDF wifi_provisioning + NimBLE, 兼容 "ESP BLE Provisioning" App)
// 阻塞式: Start() 启动广播 Itoy-XXXX; 手机 App 下发 WiFi 凭据; 验证成功后
// 写入 SsidManager 并重启, 由正常流程 (WifiStation) 连接。
class WifiProvisioningBle {
public:
    void Start();   // 阻塞直到配网完成 (成功后内部 esp_restart)
private:
    static void ProvEventHandler(void* arg, esp_event_base_t base,
                                 int32_t id, void* data);
};

#endif  // CONFIG_ITOY_PROVISIONING_BLE

#endif  // WIFI_PROVISIONING_BLE_H_
