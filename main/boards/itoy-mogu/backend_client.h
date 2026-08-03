#ifndef BACKEND_CLIENT_H_
#define BACKEND_CLIENT_H_

#include <memory>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "power_control.h"
#include "touch_pad.h"
#include "motor_control.h"
#include "mood_controller.h"

class WebSocket;
class Mqtt;
struct cJSON;

// 精简版小智 (xiaozhi) 后端客户端 — 无语音:
//   OTA 拿到 websocket/mqtt 配置后连后端, 发 hello;
//   响应 MCP initialize / tools/list / tools/call(self.get_device_status);
//   处理下行 llm.emotion -> mood 手势.
// 线格式与 itoy-esp32 (xiaozhi) 逐字节一致, 后端无需改动即可读取.
class BackendClient {
public:
    BackendClient();
    ~BackendClient();

    // 注入子系统指针 (与 MoodController 同模式: 默认构造 + Initialize)
    void Initialize(PowerControl* power, TouchPad* touch,
                    MotorControl* motor, MoodController* mood);

    // 读 NVS 配置, 选 WS 或 MQTT, 起连接/重连任务
    void Start();

private:
    // 传输
    void ConnectLoop();
    bool StartWebSocket();
    bool StartMqtt();
    bool IsConnected() const;
    bool SendText(const std::string& text);

    // 报文
    std::string BuildHello(bool websocket) const;
    void OnText(const std::string& text);        // 统一解析+派发 (含 MQTT 自发自收过滤)
    void OnServerHello(const cJSON* root);

    // MCP
    void HandleMcpPayload(const cJSON* payload);
    void SendMcpFrame(int id, cJSON* result_or_error, bool is_error);
    cJSON* BuildToolsList() const;
    std::string BuildDeviceStatusJson() const;

    static MoodState EmotionToMood(const std::string& emotion);
    static void TaskFunc(void* arg);

    PowerControl* power_;
    TouchPad* touch_;
    MotorControl* motor_;
    MoodController* mood_;

    std::unique_ptr<WebSocket> ws_;
    std::unique_ptr<Mqtt> mqtt_;
    bool use_ws_ = true;
    int ws_version_ = 1;            // hello version (NVS websocket.version)
    std::string publish_topic_;     // MQTT 收发 topic
    std::string session_id_;        // 服务器 hello 下发
    bool hello_sent_ = false;
    TaskHandle_t task_ = nullptr;
};

#endif // BACKEND_CLIENT_H_
