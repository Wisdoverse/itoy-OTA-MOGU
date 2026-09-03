#include "backend_client.h"

#include "rgb_led.h"

#include <esp_log.h>
#include <esp_app_desc.h>
#include <esp_system.h>
#include <esp_timer.h>
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

// ---- 云端电机指令的后台执行 (MoveSteps 阻塞, 不能占网络回调任务) ----
namespace {
struct CloudMotorCmd {
    MotorControl* motor;
    MotorId id;
    int steps;
    int delay_ms;
};
volatile bool s_cloud_motor_busy = false;   // 同时只执行一条, 忙则拒绝

void CloudMotorTask(void* arg) {
    auto* c = static_cast<CloudMotorCmd*>(arg);
    c->motor->MoveSteps(c->id, c->steps, c->delay_ms);
    s_cloud_motor_busy = false;
    delete c;
    vTaskDelete(nullptr);
}
}  // namespace

void BackendClient::Initialize(PowerControl* power, TouchPad* touch,
                               MotorControl* motor, MoodController* mood, RgbLed* rgb) {
    power_ = power;
    touch_ = touch;
    motor_ = motor;
    mood_ = mood;
    rgb_ = rgb;
}

BackendClient::~BackendClient() {
    if (task_) { vTaskDelete(task_); task_ = nullptr; }
    if (ws_) ws_->Close();
}

// ------------------------------------------------------------------ 启动

void BackendClient::Start() {
    // 与 itoy-esp32 一致: 优先 MQTT, 其次 WebSocket
    Settings mq_set("mqtt", false);
    std::string endpoint = mq_set.GetString("endpoint");
    publish_topic_ = mq_set.GetString("publish_topic");
    if (!endpoint.empty() && !publish_topic_.empty()) {
        use_ws_ = false;
        ESP_LOGI(TAG, "使用 MQTT: %s topic=%s", endpoint.c_str(), publish_topic_.c_str());
    } else {
        Settings ws_set("websocket", false);
        std::string ws_url = ws_set.GetString("url");
        if (ws_url.empty()) {
            ESP_LOGW(TAG, "OTA 未下发 mqtt/websocket 配置, 后端客户端不启动");
            return;
        }
        use_ws_ = true;
        ws_version_ = ws_set.GetInt("version", 1);
        if (ws_version_ == 0) ws_version_ = 1;
        ESP_LOGI(TAG, "使用 WebSocket: %s (version=%d)", ws_url.c_str(), ws_version_);
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

    ESP_LOGI(TAG, "连接 %s (token=%d字节, version=%d, free_heap=%lu)",
             url.c_str(), (int)token.size(), ws_version_, (unsigned long)esp_get_free_heap_size());

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

    ESP_LOGI(TAG, "连接 MQTT %s:%d (topic=%s, keepalive=%d, free_heap=%lu)",
             broker.c_str(), port, publish_topic_.c_str(), keepalive, (unsigned long)esp_get_free_heap_size());

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
        // 情绪下发 (主要来源)
        cJSON* emotion = cJSON_GetObjectItem(root, "emotion");
        if (cJSON_IsString(emotion)) {
            MoodState m = EmotionToMood(emotion->valuestring);
            ESP_LOGI(TAG, ">>> 收到情绪下发: \"%s\" -> 触发 %s", emotion->valuestring, MoodName(m));
            if (mood_) mood_->RequestExternalMood(m);
        } else {
            ESP_LOGW(TAG, "llm 消息缺少 emotion 字段: %.120s", text.c_str());
        }
    } else if (strcmp(t, "alert") == 0) {
        // 告警也带 emotion (status / message / emotion)
        cJSON* status = cJSON_GetObjectItem(root, "status");
        cJSON* message = cJSON_GetObjectItem(root, "message");
        cJSON* emotion = cJSON_GetObjectItem(root, "emotion");
        ESP_LOGI(TAG, ">>> 收到告警: status=%s emotion=%s msg=%s",
                 cJSON_IsString(status) ? status->valuestring : "(无)",
                 cJSON_IsString(emotion) ? emotion->valuestring : "(无)",
                 cJSON_IsString(message) ? message->valuestring : "(无)");
        if (cJSON_IsString(emotion) && mood_) {
            MoodState m = EmotionToMood(emotion->valuestring);
            ESP_LOGI(TAG, "    告警情绪 -> 触发 %s", MoodName(m));
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
    // tts / stt / command / goodbye 等忽略 (无语音)

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
        cJSON* args = params ? cJSON_GetObjectItem(params, "arguments") : nullptr;
        if (cJSON_IsString(name) && strcmp(name->valuestring, "self.get_device_status") == 0) {
            std::string status = BuildDeviceStatusJson();
            // 串口同步打印: reparse 后缩进格式化成多行, 便于阅读 (云端仍收压缩 JSON)
            if (cJSON* pretty = cJSON_Parse(status.c_str())) {
                char* p = cJSON_Print(pretty);
                if (p) {
                    ESP_LOGI(TAG, "上报设备状态:\n%s", p);
                    cJSON_free(p);
                }
                cJSON_Delete(pretty);
            } else {
                ESP_LOGI(TAG, "上报设备状态: %s", status.c_str());
            }
            SendToolText(id, status);
        } else if (cJSON_IsString(name) && strcmp(name->valuestring, "self.control_motor") == 0) {
            SendToolText(id, HandleControlMotor(args));
        } else if (cJSON_IsString(name) && strcmp(name->valuestring, "self.control_light") == 0) {
            SendToolText(id, HandleControlLight(args));
        } else {
            cJSON* e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "message",
                ("Unknown tool: " + (cJSON_IsString(name) ? std::string(name->valuestring) : std::string("?"))).c_str());
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

// tools/call 统一回复: content[0].text = text (cJSON 自动转义)
void BackendClient::SendToolText(int id, const std::string& text) {
    cJSON* result = cJSON_CreateObject();
    cJSON* content = cJSON_CreateArray();
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text.c_str());
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(result, "content", content);
    cJSON_AddBoolToObject(result, "isError", false);
    SendMcpFrame(id, result, false);
}

cJSON* BackendClient::BuildToolsList() const {
    cJSON* result = cJSON_CreateObject();
    cJSON* tools = cJSON_CreateArray();

    cJSON* tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", "self.get_device_status");
    cJSON_AddStringToObject(tool, "description",
        "Provides the real-time status of the device: battery, wifi signal, touch, motor positions, mood"
#if CONFIG_ITOY_REPORT_DEBUG
        ", debug/config values"
#endif
        ".");
    cJSON* schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    cJSON_AddItemToObject(schema, "properties", cJSON_CreateObject());
    cJSON_AddItemToObject(tool, "inputSchema", schema);
    cJSON_AddItemToArray(tools, tool);

    // 云端控制工具 (基础版): 电机 + 灯光, schema 用字面量解析
    auto add_tool = [tools](const char* name, const char* desc, const char* schema_json) {
        cJSON* t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", name);
        cJSON_AddStringToObject(t, "description", desc);
        cJSON* sch = cJSON_Parse(schema_json);
        if (!sch) sch = cJSON_CreateObject();
        cJSON_AddItemToObject(t, "inputSchema", sch);
        cJSON_AddItemToArray(tools, t);
    };
    add_tool("self.control_motor",
        "Control the motors: motor=nod (head up/down) or shake (head left/right). "
        "steps is signed, range +-2048 (about half a turn); optional speed_ms 1-20 (default 4). "
        "Runs in background, one command at a time.",
        "{\"type\":\"object\",\"properties\":{"
        "\"motor\":{\"type\":\"string\",\"enum\":[\"nod\",\"shake\"]},"
        "\"steps\":{\"type\":\"integer\",\"minimum\":-2048,\"maximum\":2048},"
        "\"speed_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":20,\"default\":4}},"
        "\"required\":[\"motor\",\"steps\"]}");
    add_tool("self.control_light",
        "Control the RGB light: action=solid (steady color) / breath (breathing effect) / off. "
        "r,g,b are 0-255 (default 255), brightness is 0-100 (default 50).",
        "{\"type\":\"object\",\"properties\":{"
        "\"action\":{\"type\":\"string\",\"enum\":[\"solid\",\"breath\",\"off\"]},"
        "\"r\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
        "\"g\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
        "\"b\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
        "\"brightness\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100,\"default\":50}},"
        "\"required\":[\"action\"]}");

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

#if CONFIG_ITOY_REPORT_DEBUG
    // debug (扩展: sdkconfig 里按项开关, 见 "云端调试上报" 菜单)
    cJSON* debug = cJSON_CreateObject();
#if CONFIG_ITOY_REPORT_BUILD_CONFIG
    cJSON* build = cJSON_CreateObject();
#if CONFIG_ITOY_RUN_NORMAL
    cJSON_AddStringToObject(build, "run_mode", "normal");
#elif CONFIG_ITOY_ENABLE_DEBUG_MODE
    cJSON_AddStringToObject(build, "run_mode", "debug");
#else
    cJSON_AddStringToObject(build, "run_mode", "motor_selftest");
#endif
#if CONFIG_ITOY_PROVISIONING_BLE
    cJSON_AddStringToObject(build, "provisioning", "ble");
#else
    cJSON_AddStringToObject(build, "provisioning", "ap");
#endif
#ifdef CONFIG_ITOY_ENABLE_MOTOR
    cJSON_AddBoolToObject(build, "motor", true);
#if CONFIG_ITOY_HOME_POT
    cJSON_AddStringToObject(build, "home_mode", "pot");        // 电位器闭环回中
#else
    cJSON_AddStringToObject(build, "home_mode", "open_loop");  // 开环步数回中
#endif
#else
    cJSON_AddBoolToObject(build, "motor", false);
#endif
#ifdef CONFIG_ITOY_ENABLE_IMU
    cJSON_AddBoolToObject(build, "imu", true);
#else
    cJSON_AddBoolToObject(build, "imu", false);
#endif
    cJSON_AddNumberToObject(build, "rgb_led_count", CONFIG_ITOY_RGB_LED_COUNT);
    cJSON_AddNumberToObject(build, "touch_ratio", CONFIG_ITOY_TOUCH_RATIO);
    cJSON_AddItemToObject(debug, "build", build);
#endif  // ITOY_REPORT_BUILD_CONFIG
#if CONFIG_ITOY_REPORT_TOUCH_PARAMS
    cJSON* thr = cJSON_CreateArray();
    cJSON* base = cJSON_CreateArray();
    for (int i = 0; i < TOUCH_PAD_COUNT; i++) {
        cJSON_AddItemToArray(thr, cJSON_CreateNumber((double)(touch_ ? touch_->GetThreshold(i) : 0)));
        cJSON_AddItemToArray(base, cJSON_CreateNumber((double)(touch_ ? touch_->GetBaseline(i) : 0)));
    }
    cJSON_AddItemToObject(debug, "touch_threshold", thr);
    cJSON_AddItemToObject(debug, "touch_baseline", base);
#endif  // ITOY_REPORT_TOUCH_PARAMS
#if CONFIG_ITOY_REPORT_BATTERY_MV
    cJSON_AddNumberToObject(debug, "battery_mv", power_ ? power_->ReadBatteryMv() : 0);
#endif
#if CONFIG_ITOY_REPORT_RUNTIME
    cJSON_AddNumberToObject(debug, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(debug, "free_heap", (double)esp_get_free_heap_size());
#endif
    cJSON_AddItemToObject(root, "debug", debug);
#endif  // ITOY_REPORT_DEBUG

    char* s = cJSON_PrintUnformatted(root);
    std::string json(s);
    cJSON_free(s);
    cJSON_Delete(root);
    return json;
}

// ------------------------------------------------------------------ 云端控制

// self.control_motor: {motor:"nod"|"shake", steps:有符号, speed_ms?:1~20}
// MoveSteps 阻塞 (大步数十几秒), 丢后台任务执行; 同一时间只跑一条, 忙则拒绝
std::string BackendClient::HandleControlMotor(const cJSON* args) {
    if (!motor_) return "{\"ok\":false,\"error\":\"motor disabled\"}";
    cJSON* jm = cJSON_GetObjectItem(args, "motor");
    cJSON* js = cJSON_GetObjectItem(args, "steps");
    if (!cJSON_IsString(jm) || !cJSON_IsNumber(js))
        return "{\"ok\":false,\"error\":\"need motor(nod|shake) and steps\"}";

    MotorId id;
    if (strcmp(jm->valuestring, "nod") == 0) {
        id = MOTOR_NOD;
    } else if (strcmp(jm->valuestring, "shake") == 0) {
        id = MOTOR_SHAKE;
    } else {
        return "{\"ok\":false,\"error\":\"motor must be nod or shake\"}";
    }

    int steps = (int)js->valuedouble;
    if (steps > 2048) steps = 2048;    // 云端收紧到 ~半圈 (调试网页是 +-8192)
    if (steps < -2048) steps = -2048;
    int speed = 4;
    cJSON* jv = cJSON_GetObjectItem(args, "speed_ms");
    if (cJSON_IsNumber(jv)) speed = (int)jv->valuedouble;
    if (speed < 1) speed = 1;
    if (speed > 20) speed = 20;

    if (s_cloud_motor_busy) return "{\"ok\":false,\"error\":\"motor busy, try later\"}";
    auto* cmd = new CloudMotorCmd{motor_, id, steps, speed};
    s_cloud_motor_busy = true;
    if (xTaskCreate(CloudMotorTask, "cloud_motor", 4096, cmd, 5, nullptr) != pdPASS) {
        s_cloud_motor_busy = false;
        delete cmd;
        return "{\"ok\":false,\"error\":\"task create failed\"}";
    }
    ESP_LOGI(TAG, "云端电机指令: %s steps=%d speed=%dms", jm->valuestring, steps, speed);
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"motor\":\"%s\",\"steps\":%d}", jm->valuestring, steps);
    return buf;
}

// self.control_light: {action:"solid"|"breath"|"off", r?, g?, b?, brightness?}
std::string BackendClient::HandleControlLight(const cJSON* args) {
    if (!rgb_) return "{\"ok\":false,\"error\":\"rgb disabled\"}";
    cJSON* ja = cJSON_GetObjectItem(args, "action");
    if (!cJSON_IsString(ja)) return "{\"ok\":false,\"error\":\"need action(solid|breath|off)\"}";
    const char* act = ja->valuestring;

    if (strcmp(act, "off") == 0) {
        rgb_->Off();
        ESP_LOGI(TAG, "云端灯光: off");
        return "{\"ok\":true,\"action\":\"off\"}";
    }
    if (strcmp(act, "solid") != 0 && strcmp(act, "breath") != 0)
        return "{\"ok\":false,\"error\":\"action must be solid, breath or off\"}";

    // 缺省 r/g/b=255 (白), brightness=50; 全部限幅
    auto clamp_arg = [args](const char* key, int lo, int hi, int dft) {
        cJSON* j = cJSON_GetObjectItem(args, key);
        int v = cJSON_IsNumber(j) ? (int)j->valuedouble : dft;
        return v < lo ? lo : (v > hi ? hi : v);
    };
    int r = clamp_arg("r", 0, 255, 255);
    int g = clamp_arg("g", 0, 255, 255);
    int b = clamp_arg("b", 0, 255, 255);
    int p = clamp_arg("brightness", 0, 100, 50);

    if (strcmp(act, "solid") == 0) {
        rgb_->Solid((uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)p);
        ESP_LOGI(TAG, "云端灯光: solid (%d,%d,%d) %d%%", r, g, b, p);
    } else {
        rgb_->Breath((uint8_t)r, (uint8_t)g, (uint8_t)b, 4000, 20, 80);
        ESP_LOGI(TAG, "云端灯光: breath (%d,%d,%d)", r, g, b);
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"action\":\"%s\",\"r\":%d,\"g\":%d,\"b\":%d}", act, r, g, b);
    return buf;
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
