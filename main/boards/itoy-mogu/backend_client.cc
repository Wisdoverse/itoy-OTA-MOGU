#include "backend_client.h"

#include <esp_log.h>
#include <esp_app_desc.h>
#include <esp_system.h>
#include <cJSON.h>

#include "board.h"
#include "settings.h"
#include "system_info.h"
#include "network_interface.h"
#include "web_socket.h"
#include "mqtt.h"
#include "wifi_station.h"

#define TAG "Backend"

// hello 里的音频参数 (与 itoy-esp32 一致; 蘑菇无音频但保留以免后端异常)
#define OPUS_FRAME_DURATION_MS 60

static const char* MoodName(MoodState s) {
    switch (s) {
        case MOOD_OFF: return "OFF";
        case MOOD_POWER_ON: return "POWER_ON";
        case MOOD_CALM: return "CALM";
        case MOOD_HAPPY: return "HAPPY";
        case MOOD_COMFORT: return "COMFORT";
        case MOOD_DEEP_BREATH: return "DEEP_BREATH";
        case MOOD_SLEEPY: return "SLEEPY";
        case MOOD_DISTURBED: return "DISTURBED";
        case MOOD_LOW_BATTERY: return "LOW_BATTERY";
        case MOOD_NIGHT_LIGHT: return "NIGHT_LIGHT";
        default: return "UNKNOWN";
    }
}

BackendClient::BackendClient() {}

void BackendClient::Initialize(PowerControl* power, TouchPad* touch,
                               MotorControl* motor, MoodController* mood) {
    power_ = power;
    touch_ = touch;
    motor_ = motor;
    mood_ = mood;
}

BackendClient::~BackendClient() {
    if (task_) { vTaskDelete(task_); task_ = nullptr; }
    if (ws_) ws_->Close();
}

// ------------------------------------------------------------------ 启动

void BackendClient::Start() {
    // 选传输: 优先 websocket, 否则 mqtt
    Settings ws_set("websocket", false);
    std::string ws_url = ws_set.GetString("url");
    if (!ws_url.empty()) {
        use_ws_ = true;
        ws_version_ = ws_set.GetInt("version", 1);
        if (ws_version_ == 0) ws_version_ = 1;
        ESP_LOGI(TAG, "使用 WebSocket: %s (version=%d)", ws_url.c_str(), ws_version_);
    } else {
        Settings mq_set("mqtt", false);
        std::string endpoint = mq_set.GetString("endpoint");
        publish_topic_ = mq_set.GetString("publish_topic");
        if (endpoint.empty() || publish_topic_.empty()) {
            ESP_LOGW(TAG, "OTA 未下发 websocket/mqtt 配置, 后端客户端不启动");
            return;
        }
        use_ws_ = false;
        ESP_LOGI(TAG, "使用 MQTT: %s topic=%s", endpoint.c_str(), publish_topic_.c_str());
    }

    xTaskCreate(TaskFunc, "backend", 8192, this, 5, &task_);
}

void BackendClient::TaskFunc(void* arg) {
    static_cast<BackendClient*>(arg)->ConnectLoop();
}

// 连接 + 重连循环
void BackendClient::ConnectLoop() {
    const int reconnect_delay_ms = 10000;
    while (true) {
        bool ok = IsConnected();
        if (!ok) {
            hello_sent_ = false;
            session_id_.clear();
            ok = use_ws_ ? StartWebSocket() : StartMqtt();
        }
        if (ok && !hello_sent_) {
            std::string hello = BuildHello(use_ws_);
            if (SendText(hello)) {
                hello_sent_ = true;
                ESP_LOGI(TAG, "-> 发送 hello: %s", hello.c_str());
            } else {
                ESP_LOGW(TAG, "hello 发送失败");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(ok ? 1000 : reconnect_delay_ms));
    }
}

bool BackendClient::IsConnected() const {
    if (use_ws_) return ws_ && ws_->IsConnected();
    return mqtt_ && mqtt_->IsConnected();
}

// ------------------------------------------------------------------ WebSocket

bool BackendClient::StartWebSocket() {
    Settings ws_set("websocket", false);
    std::string url = ws_set.GetString("url");
    std::string token = ws_set.GetString("token");
    if (url.empty()) return false;

    if (ws_) { ws_->Close(); ws_.reset(); }

    auto network = Board::GetInstance().GetNetwork();
    ws_ = network->CreateWebSocket(1);
    if (!ws_) { ESP_LOGE(TAG, "CreateWebSocket 失败"); return false; }

    if (!token.empty()) {
        std::string auth = (token.find(' ') == std::string::npos) ? ("Bearer " + token) : token;
        ws_->SetHeader("Authorization", auth.c_str());
    }
    ws_->SetHeader("Protocol-Version", std::to_string(ws_version_).c_str());
    ws_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    ws_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    ws_->OnDisconnected([this]() {
        ESP_LOGW(TAG, "WebSocket 断开, 将重连");
        hello_sent_ = false;
    });
    ws_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) return;  // 无音频, 忽略二进制帧
        OnText(std::string(data, len));
    });

    if (!ws_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "WebSocket 连接失败: %s", url.c_str());
        return false;
    }
    ESP_LOGI(TAG, "WebSocket 已连接");
    return true;
}

// ------------------------------------------------------------------ MQTT

bool BackendClient::StartMqtt() {
    Settings mq_set("mqtt", false);
    std::string endpoint = mq_set.GetString("endpoint");
    std::string client_id = mq_set.GetString("client_id");
    std::string username = mq_set.GetString("username");
    std::string password = mq_set.GetString("password");
    int keepalive = mq_set.GetInt("keepalive", 240);
    publish_topic_ = mq_set.GetString("publish_topic");
    if (endpoint.empty() || publish_topic_.empty()) return false;

    if (mqtt_) { mqtt_->Disconnect(); mqtt_.reset(); }

    auto network = Board::GetInstance().GetNetwork();
    mqtt_ = network->CreateMqtt(0);
    if (!mqtt_) { ESP_LOGE(TAG, "CreateMqtt 失败"); return false; }
    mqtt_->SetKeepAlive(keepalive);

    mqtt_->OnDisconnected([this]() {
        ESP_LOGW(TAG, "MQTT 断开, 将重连");
        hello_sent_ = false;
    });
    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        OnText(payload);  // OnText 内部过滤自发自收的回声
    });

    std::string broker = endpoint;
    int port = 8883;
    size_t pos = endpoint.find(':');
    if (pos != std::string::npos) {
        broker = endpoint.substr(0, pos);
        port = std::stoi(endpoint.substr(pos + 1));
    }

    if (!mqtt_->Connect(broker, port, client_id, username, password)) {
        ESP_LOGE(TAG, "MQTT 连接失败: %s:%d", broker.c_str(), port);
        return false;
    }
    mqtt_->Subscribe(publish_topic_);
    ESP_LOGI(TAG, "MQTT 已连接 %s:%d, topic=%s", broker.c_str(), port, publish_topic_.c_str());
    return true;
}

bool BackendClient::SendText(const std::string& text) {
    if (use_ws_) {
        return ws_ && ws_->Send(text);
    }
    return mqtt_ && !publish_topic_.empty() && mqtt_->Publish(publish_topic_, text);
}

// ------------------------------------------------------------------ 报文构建

std::string BackendClient::BuildHello(bool websocket) const {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", websocket ? ws_version_ : 3);
    cJSON_AddStringToObject(root, "transport", websocket ? "websocket" : "udp");

    cJSON* features = cJSON_CreateObject();
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);

    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);

    if (!websocket) {
        cJSON_AddItemToObject(root, "recordings", cJSON_CreateArray());  // 无 sdcard, 空
    }

    char* s = cJSON_PrintUnformatted(root);
    std::string msg(s);
    cJSON_free(s);
    cJSON_Delete(root);
    return msg;
}

// 解析并派发一条文本帧 (WS 文本 / MQTT payload).
// 内置过滤: MQTT 订阅同 topic 会收到自己发出的 mcp 响应/hello 回声, 按字段区分丢弃.
void BackendClient::OnText(const std::string& text) {
    cJSON* root = cJSON_Parse(text.c_str());
    if (!root) { ESP_LOGE(TAG, "JSON 解析失败: %.120s", text.c_str()); return; }

    cJSON* type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) { cJSON_Delete(root); return; }
    const char* t = type->valuestring;
    ESP_LOGI(TAG, "<- 收到消息 [%s]", t);

    if (strcmp(t, "hello") == 0) {
        // 自己发出的首条 hello 没有 session_id; 服务器 hello 才有
        if (cJSON_GetObjectItem(root, "session_id") != nullptr) OnServerHello(root);
    } else if (strcmp(t, "mcp") == 0) {
        cJSON* payload = cJSON_GetObjectItem(root, "payload");
        if (cJSON_IsObject(payload)) {
            // 带 method 的是后端请求; 只有 result/error 的是自己响应的回声, 丢弃
            if (cJSON_GetObjectItem(payload, "method") != nullptr) HandleMcpPayload(payload);
        }
    } else if (strcmp(t, "llm") == 0) {
        cJSON* emotion = cJSON_GetObjectItem(root, "emotion");
        if (cJSON_IsString(emotion) && mood_) {
            MoodState m = EmotionToMood(emotion->valuestring);
            ESP_LOGI(TAG, "情绪下发: %s -> %s", emotion->valuestring, MoodName(m));
            mood_->RequestExternalMood(m);
        }
    } else if (strcmp(t, "system") == 0) {
        cJSON* cmd = cJSON_GetObjectItem(root, "command");
        if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "reboot") == 0) {
            ESP_LOGI(TAG, "收到 reboot 指令, 1s 后重启");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
    }
    // tts / stt / command / alert / goodbye 等忽略 (无语音)

    cJSON_Delete(root);
}

void BackendClient::OnServerHello(const cJSON* root) {
    cJSON* sid = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(sid)) {
        session_id_ = sid->valuestring;
        ESP_LOGI(TAG, "===== 后端通道就绪 (session_id=%s) =====", session_id_.c_str());
    }
}

// ------------------------------------------------------------------ MCP

void BackendClient::HandleMcpPayload(const cJSON* payload) {
    cJSON* id_item = cJSON_GetObjectItem(payload, "id");
    if (!cJSON_IsNumber(id_item)) return;
    int id = id_item->valueint;

    cJSON* method = cJSON_GetObjectItem(payload, "method");
    if (!cJSON_IsString(method)) {
        cJSON* e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "message", "Missing method");
        SendMcpFrame(id, e, true);
        return;
    }
    std::string m = method->valuestring;
    ESP_LOGI(TAG, "<- MCP 请求: %s (id=%d)", m.c_str(), id);

    if (m.rfind("notifications", 0) == 0) return;  // 通知无需回复

    if (m == "initialize") {
        cJSON* result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
        cJSON* caps = cJSON_CreateObject();
        cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());
        cJSON_AddItemToObject(result, "capabilities", caps);
        cJSON* info = cJSON_CreateObject();
        cJSON_AddStringToObject(info, "name", BOARD_NAME);
        cJSON_AddStringToObject(info, "version", esp_app_get_description()->version);
        cJSON_AddItemToObject(result, "serverInfo", info);
        SendMcpFrame(id, result, false);
    } else if (m == "tools/list") {
        SendMcpFrame(id, BuildToolsList(), false);
    } else if (m == "tools/call") {
        cJSON* params = cJSON_GetObjectItem(payload, "params");
        cJSON* name = params ? cJSON_GetObjectItem(params, "name") : nullptr;
        if (cJSON_IsString(name) && strcmp(name->valuestring, "self.get_device_status") == 0) {
            std::string status = BuildDeviceStatusJson();
            ESP_LOGI(TAG, "上报设备状态: %s", status.c_str());
            cJSON* result = cJSON_CreateObject();
            cJSON* content = cJSON_CreateArray();
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "type", "text");
            cJSON_AddStringToObject(item, "text", status.c_str());  // cJSON 自动转义
            cJSON_AddItemToArray(content, item);
            cJSON_AddItemToObject(result, "content", content);
            cJSON_AddBoolToObject(result, "isError", false);
            SendMcpFrame(id, result, false);
        } else {
            cJSON* e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "message", ("Unknown tool: " + std::string(name->valuestring)).c_str());
            SendMcpFrame(id, e, true);
        }
    } else {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "message", ("Method not implemented: " + m).c_str());
        SendMcpFrame(id, err, true);
    }
}

// 组 JSON-RPC 帧 -> 再套外层 {session_id, type:mcp, payload} -> 发送
void BackendClient::SendMcpFrame(int id, cJSON* result_or_error, bool is_error) {
    cJSON* rpc = cJSON_CreateObject();
    cJSON_AddStringToObject(rpc, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(rpc, "id", id);
    if (is_error) {
        if (result_or_error) cJSON_AddItemToObject(rpc, "error", result_or_error);
    } else {
        if (result_or_error) cJSON_AddItemToObject(rpc, "result", result_or_error);
    }

    cJSON* wrap = cJSON_CreateObject();
    cJSON_AddStringToObject(wrap, "session_id", session_id_.c_str());
    cJSON_AddStringToObject(wrap, "type", "mcp");
    cJSON_AddItemToObject(wrap, "payload", rpc);

    char* s = cJSON_PrintUnformatted(wrap);
    std::string msg(s);
    cJSON_free(s);
    cJSON_Delete(wrap);
    SendText(msg);
    ESP_LOGI(TAG, "-> mcp reply id=%d", id);
}

cJSON* BackendClient::BuildToolsList() const {
    cJSON* result = cJSON_CreateObject();
    cJSON* tools = cJSON_CreateArray();

    cJSON* tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", "self.get_device_status");
    cJSON_AddStringToObject(tool, "description",
        "Provides the real-time status of the device: battery, wifi signal, touch, motor positions, mood.");
    cJSON* schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    cJSON_AddItemToObject(schema, "properties", cJSON_CreateObject());
    cJSON_AddItemToObject(tool, "inputSchema", schema);
    cJSON_AddItemToArray(tools, tool);

    cJSON_AddItemToObject(result, "tools", tools);
    return result;
}

std::string BackendClient::BuildDeviceStatusJson() const {
    cJSON* root = cJSON_CreateObject();

    // itoy-esp32 固定字段 (蘑菇无音频/屏, 留空对象避免后端缺键)
    cJSON_AddItemToObject(root, "audio_speaker", cJSON_CreateObject());
    cJSON_AddItemToObject(root, "screen", cJSON_CreateObject());

    // battery
    cJSON* battery = cJSON_CreateObject();
    cJSON_AddNumberToObject(battery, "level", power_ ? power_->ReadBatteryPercent() : 0);
    cJSON_AddBoolToObject(battery, "charging", false);  // 蘑菇无充电检测脚
    cJSON_AddItemToObject(root, "battery", battery);

    // network (阈值与 itoy-esp32 一致)
    cJSON* network = cJSON_CreateObject();
    auto& wifi = WifiStation::GetInstance();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi.GetSsid().c_str());
    int rssi = wifi.GetRssi();
    const char* sig = (rssi >= -60) ? "strong" : (rssi >= -70) ? "medium" : "weak";
    cJSON_AddStringToObject(network, "signal", sig);
    cJSON_AddItemToObject(root, "network", network);

    // touch (扩展)
    cJSON* touch = cJSON_CreateObject();
    cJSON_AddNumberToObject(touch, "count", TOUCH_PAD_COUNT);
    cJSON* pressed = cJSON_CreateArray();
    cJSON* raw = cJSON_CreateArray();
    for (int i = 0; i < TOUCH_PAD_COUNT; i++) {
        cJSON_AddItemToArray(pressed, cJSON_CreateBool(touch_ ? touch_->IsPressed(i) : false));
        cJSON_AddItemToArray(raw, cJSON_CreateNumber((double)(touch_ ? touch_->GetRawValue(i) : 0)));
    }
    cJSON_AddItemToObject(touch, "pressed", pressed);
    cJSON_AddItemToObject(touch, "raw", raw);
    cJSON_AddItemToObject(root, "touch", touch);

    // motors (扩展)
    cJSON* motors = cJSON_CreateObject();
    if (motor_) {
        cJSON_AddNumberToObject(motors, "nod", (double)motor_->ReadNodPosition());
        cJSON_AddNumberToObject(motors, "shake", (double)motor_->ReadShakePosition());
        cJSON_AddNumberToObject(motors, "nod_percent", (int)(motor_->ReadNodPositionNorm() * 100));
        cJSON_AddNumberToObject(motors, "shake_percent", (int)(motor_->ReadShakePositionNorm() * 100));
    }
    cJSON_AddItemToObject(root, "motors", motors);

    // mood (扩展)
    cJSON_AddStringToObject(root, "mood", MoodName(mood_ ? mood_->state() : MOOD_OFF));

    char* s = cJSON_PrintUnformatted(root);
    std::string json(s);
    cJSON_free(s);
    cJSON_Delete(root);
    return json;
}

// ------------------------------------------------------------------ 情绪映射

MoodState BackendClient::EmotionToMood(const std::string& emotion) {
    auto eq = [&](const char* s) { return emotion == s; };
    if (eq("happy") || eq("laughing") || eq("funny") || eq("loving") || eq("confident") ||
        eq("cool") || eq("delicious") || eq("silly") || eq("winking") || eq("kissy") ||
        eq("surprised") || eq("shocked"))
        return MOOD_HAPPY;
    if (eq("sad") || eq("crying"))
        return MOOD_COMFORT;
    if (eq("anger") || eq("angry") || eq("scare"))
        return MOOD_DISTURBED;
    if (eq("sleepy") || eq("yawning"))
        return MOOD_SLEEPY;
    // neutral / relaxed / idle / staticstate / thinking / confused / embarrassed / buxue / 其他
    return MOOD_CALM;
}
