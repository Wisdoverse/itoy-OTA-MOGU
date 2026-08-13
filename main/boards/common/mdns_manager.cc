#include "mdns_manager.h"

#include <cctype>
#include <string>

#include <esp_log.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <mdns.h>
#include <esp_app_desc.h>

#include "board.h"
#include "system_info.h"

#define TAG "MdnsManager"

// ---- 契约常量 ----
static const char* MDNS_SERVICE    = "_itoy";
static const char* MDNS_PROTO      = "_tcp";
static const uint16_t MDNS_PORT    = 80;   // 占位, 当前无监听; App 不得直连
static const char* MDNS_PROTO_NAME = "xiaozhi";
static const char* MDNS_CONTRACT_V = "1";

// "aa:bb:cc:dd:ee:ff" -> "aabbccddeeff"
static std::string StripColons(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c != ':') out.push_back(c);
    }
    return out;
}

void MdnsManager::Start() {
    if (started_) return;
    started_ = true;

    esp_err_t err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &MdnsManager::IpEventHandler, this);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "注册 IP_EVENT_STA_GOT_IP 失败: %s", esp_err_to_name(err));
    }

    // 调用点(WaitForConnected 成功后)此时 IP 通常已就绪, 首个 got-ip 事件可能已错过,
    // 立即广播一次; 后续断线重连由事件回调兜底.
    Announce();
}

void MdnsManager::IpEventHandler(void* arg, esp_event_base_t /*base*/,
                                 int32_t /*id*/, void* /*data*/) {
    static_cast<MdnsManager*>(arg)->Announce();
}

void MdnsManager::Announce() {
    if (initialized_) return;  // 幂等; mdns 内部已自行处理后续 IP/网卡变更

    auto& board = Board::GetInstance();
    std::string mac = SystemInfo::GetMacAddress();           // "aa:bb:cc:dd:ee:ff" = Device-Id
    std::string hostname = "itoy-" + StripColons(mac);       // itoy-aabbccddeeff

    // 实例名 Itoy-XXXX: MAC 末 2 字节(冒号串末 5 字符)去冒号转大写
    std::string tail = mac.substr(mac.size() - 5);           // "ee:ff"
    std::string tail_compact;
    for (char c : tail) {
        if (c != ':') tail_compact.push_back((char)toupper((unsigned char)c));
    }
    std::string instance_name = "Itoy-" + tail_compact;      // Itoy-EEFF

    std::string uuid  = board.GetUuid();
    std::string fw    = esp_app_get_description()->version;

    // mdns_service_add 会复制 instance/service/proto/txt 字符串, 局部 std::string 安全.
    mdns_txt_item_t txt[] = {
        {"id",    mac.c_str()},        // 与 OTA Device-Id 头一致, 云端据此绑定
        {"uuid",  uuid.c_str()},       // Client-Id
        {"board", BOARD_TYPE},         // itoy-mogu
        {"name",  BOARD_NAME},
        {"fw",    fw.c_str()},
        {"proto", MDNS_PROTO_NAME},    // xiaozhi
        {"v",     MDNS_CONTRACT_V},    // 契约版本
    };

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init 失败: %s", esp_err_to_name(err));
        return;
    }
    initialized_ = true;

    mdns_hostname_set(hostname.c_str());
    mdns_service_add(instance_name.c_str(),
                     MDNS_SERVICE, MDNS_PROTO, MDNS_PORT,
                     txt, sizeof(txt) / sizeof(txt[0]));

    ESP_LOGI(TAG, "mDNS 广播: %s.local  %s.%s  实例=%s  id=%s  fw=%s",
             hostname.c_str(), MDNS_SERVICE, MDNS_PROTO,
             instance_name.c_str(), mac.c_str(), fw.c_str());
}
