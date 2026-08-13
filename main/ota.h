#ifndef _OTA_H
#define _OTA_H

#include <functional>
#include <string>

#include <esp_err.h>
#include "board.h"

class Ota {
public:
    Ota();
    ~Ota();

    bool CheckVersion();
    bool HasNewVersion() { return has_new_version_; }
    bool HasMqttConfig() { return has_mqtt_config_; }
    bool HasWebsocketConfig() { return has_websocket_config_; }
    bool HasServerTime() { return has_server_time_; }
    bool StartUpgrade(std::function<void(int progress, size_t speed)> callback);
    bool StartUpgradeFromUrl(const std::string& url, std::function<void(int progress, size_t speed)> callback);
    void MarkCurrentVersionValid();

    const std::string& GetFirmwareVersion() const { return firmware_version_; }
    const std::string& GetCurrentVersion() const { return current_version_; }
    const std::string& GetFirmwareUrl() const { return firmware_url_; }
    std::string GetCheckVersionUrl();

private:
    bool has_new_version_ = false;
    bool has_mqtt_config_ = false;
    bool has_websocket_config_ = false;
    bool has_server_time_ = false;
    std::string current_version_;
    std::string firmware_version_;
    std::string firmware_url_;

    bool Upgrade(const std::string& firmware_url);
    std::function<void(int progress, size_t speed)> upgrade_callback_;
    std::vector<int> ParseVersion(const std::string& version);
    bool IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion);
    std::unique_ptr<Http> SetupHttp();
};

#endif // _OTA_H
