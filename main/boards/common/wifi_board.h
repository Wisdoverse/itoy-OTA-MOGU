#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"

class WifiBoard : public Board {
protected:
    bool wifi_config_mode_ = false;
    void EnterConfigMode();              // 按 Kconfig 选 SoftAP 或 BLE
    void EnterWifiConfigMode();          // SoftAP + 网页配网
    void EnterBleProvisioningMode();     // 蓝牙 BLE 配网 (CONFIG_ITOY_PROVISIONING_BLE)
    virtual std::string GetBoardJson() override;

public:
    WifiBoard();
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual NetworkInterface* GetNetwork() override;
    virtual void SetPowerSaveMode(bool enabled) override;
    virtual void ResetWifiConfiguration();
};

#endif // WIFI_BOARD_H
