#ifndef MDNS_MANAGER_H
#define MDNS_MANAGER_H

#include <esp_err.h>
#include <esp_event.h>

// 管理 mDNS 广播: STA 上线后广播 _itoy._tcp 服务, 供 App 局域网发现,
// 凭 TXT 里的 Device-Id (= SystemInfo::GetMacAddress(), 与 OTA 的 Device-Id
// 头一致) 向云端绑定. 设备本身不开本地服务, port 80 仅占位.
//
// 契约详见 docs/superpowers/specs/2026-08-13-mdns-discovery-replace-activation-design.md
class MdnsManager {
public:
    static MdnsManager& GetInstance() {
        static MdnsManager instance;
        return instance;
    }

    // 注册 IP_EVENT_STA_GOT_IP 回调并立即广播一次(调用点已在 STA 拿到 IP 之后).
    // 幂等; 任何 mdns 失败只告警, 不阻塞启动.
    void Start();

private:
    MdnsManager() = default;
    ~MdnsManager() = default;
    MdnsManager(const MdnsManager&) = delete;
    MdnsManager& operator=(const MdnsManager&) = delete;

    // mdns_init + hostname + service_add, 幂等(initialized_ 守卫).
    void Announce();

    static void IpEventHandler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data);

    bool started_ = false;
    bool initialized_ = false;
};

#endif  // MDNS_MANAGER_H
