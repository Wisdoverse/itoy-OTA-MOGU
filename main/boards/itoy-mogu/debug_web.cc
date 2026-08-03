#include "sdkconfig.h"
#if CONFIG_ITOY_ENABLE_DEBUG_MODE

#include "debug_web.h"
#include "config.h"
#include "motor_control.h"
#include "power_control.h"
#include "touch_pad.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_http_server.h>
#include <lwip/sockets.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#define TAG "DebugWeb"

// 调试 AP 的 IP (192.168.4.1) —— 同时用于 DNS 劫持 (captive portal 自动弹页)
static const uint8_t kApIp[4] = {192, 168, 4, 1};

// ---- 文件级上下文 ----
static MotorControl* s_motor = nullptr;
static PowerControl* s_power = nullptr;
static TouchPad* s_touch = nullptr;

// 非阻塞电机指令: handler 填 s_cmd 后 give, MotorTask take 后执行 (大步数不卡网页)
typedef struct { uint8_t motor; int eff; int delay_ms; } MotorCmd;
static MotorCmd s_cmd;
static SemaphoreHandle_t s_motor_sem = NULL;

// ---- 日志环形缓冲 (esp_log_set_vprintf 捕获) ----
static constexpr size_t kLogBufSize = 3072;
static char s_logbuf[kLogBufSize];
static size_t s_log_len = 0;
static SemaphoreHandle_t s_log_mutex = nullptr;
static vprintf_like_t s_prev_vprintf = nullptr;

static int debug_vprintf(const char* fmt, va_list ap) {
    char line[192];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n > 0) {
        size_t len = ((size_t)n < sizeof(line)) ? (size_t)n : sizeof(line) - 1;
        xSemaphoreTake(s_log_mutex, portMAX_DELAY);
        if (s_log_len + len + 1 > kLogBufSize) {
            size_t drop = s_log_len / 2 + 1;          // 满了丢掉最旧一半
            memmove(s_logbuf, s_logbuf + drop, s_log_len - drop);
            s_log_len -= drop;
        }
        memcpy(s_logbuf + s_log_len, line, len);
        s_log_len += len;
        s_logbuf[s_log_len] = '\0';
        xSemaphoreGive(s_log_mutex);
    }
    int r = s_prev_vprintf ? s_prev_vprintf(fmt, ap2) : 0;   // 照常输出到串口
    va_end(ap2);
    return r;
}

static void LogCaptureInit() {
    s_log_mutex = xSemaphoreCreateMutex();
    s_prev_vprintf = esp_log_set_vprintf(debug_vprintf);
}

// =====================================================================
// 调试网页 (内嵌 HTML + CSS + JS)
// =====================================================================
static const char kIndexHtml[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"
"<title>Itoy 调试</title><style>"
"body{font-family:sans-serif;margin:12px;background:#111;color:#eee}"
"h2{margin:14px 0 4px}button{padding:8px 12px;margin:3px;font-size:15px}"
"input{padding:6px;font-size:15px}.st{font-size:14px;line-height:1.7}"
"#log{width:100%;height:240px;font-family:monospace;font-size:12px}"
".b{display:inline-block;width:90px}</style></head><body>"
"<h2>电机控制</h2>"
"步数:<input id='steps' type='number' value='1024' size='6'>"
" ms/步:<input id='delay' type='number' value='4' size='4' min='1' max='20'>"
"(不转就调大, 如 6/8)"
"<br><button onclick=mv('a',1)>点头 +</button>"
"<button onclick=mv('a',-1)>点头 −</button>"
"<button class=b></button>"
"<button onclick=mv('b',1)>摇头 +</button>"
"<button onclick=mv('b',-1)>摇头 −</button>"
"<h2>状态</h2><div class=st id=state>读取中…</div>"
"<h2>触摸 (GPIO1-4)</h2><div class=st id=touch>读取中…</div>"
"<h2>串口日志</h2><textarea id=log readonly></textarea>"
"<script>"
"async function state(){try{let r=await fetch('/api/state');"
"let j=await r.json();"
"document.getElementById('state').innerHTML="
"'点头电位器: '+j.nod+'<br>摇头电位器: '+j.shake"
"+'<br>电池: '+j.batt_mv+' mV ('+j.batt_pct+'%)';"
"if(j.t){let h='';for(let i=0;i<4;i++){h+='CH'+i+' (GPIO'+(i+1)+'): '+j.t[i]+' '+"
"(j.p[i]?'<b style=color:#f44>触摸</b>':'未触')+'<br>';}document.getElementById('touch').innerHTML=h;}"
"}catch(e){}}"
"async function log(){try{let r=await fetch('/api/log');"
"let t=await r.text();let e=document.getElementById('log');"
"e.value=t;e.scrollTop=e.scrollHeight;}catch(e){}}"
"async function mv(m,dir){let s=parseInt(document.getElementById('steps').value)||0;"
"let dl=parseInt(document.getElementById('delay').value)||4;"
"if(Math.abs(s)>8192){alert('步数上限 8192');return;}"
"await fetch('/api/motor?m='+m+'&dir='+dir+'&steps='+s+'&delay='+dl);}"
"state();log();setInterval(state,1000);setInterval(log,1000);"
"</script></body></html>";

// =====================================================================
// HTTP handlers
// =====================================================================
static esp_err_t handle_root(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// 通配: 任何非 /api/* 的 GET 都返回网页 (captive portal 探测路径也命中 -> 自动弹页)
static esp_err_t handle_catchall(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handle_state(httpd_req_t* req) {
    // 触摸 4 路
    uint32_t tv[4] = {0,0,0,0};
    bool tp[4] = {false,false,false,false};
    if (s_touch) {
        for (int i = 0; i < 4; i++) {
            tv[i] = s_touch->GetRawValue(i);
            tp[i] = s_touch->IsPressed(i);
        }
    }
    char json[300];
    snprintf(json, sizeof(json),
             "{\"nod\":%lu,\"shake\":%lu,\"batt_mv\":%d,\"batt_pct\":%d,"
             "\"t\":[%lu,%lu,%lu,%lu],\"p\":[%s,%s,%s,%s]}",
             s_motor ? s_motor->ReadNodPosition() : 0,
             s_motor ? s_motor->ReadShakePosition() : 0,
             s_power ? s_power->ReadBatteryMv() : 0,
             s_power ? s_power->ReadBatteryPercent() : 0,
             tv[0], tv[1], tv[2], tv[3],
             tp[0]?"true":"false", tp[1]?"true":"false",
             tp[2]?"true":"false", tp[3]?"true":"false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handle_motor(httpd_req_t* req) {
    char query[80], val[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no query");
        return ESP_OK;
    }
    char motor = 'a';
    int dir = 1, steps = 0, delay_ms = 4;
    if (httpd_query_key_value(query, "m", val, sizeof(val)) == ESP_OK) motor = val[0];
    if (httpd_query_key_value(query, "dir", val, sizeof(val)) == ESP_OK) dir = atoi(val);
    if (httpd_query_key_value(query, "steps", val, sizeof(val)) == ESP_OK) steps = atoi(val);
    if (httpd_query_key_value(query, "delay", val, sizeof(val)) == ESP_OK) delay_ms = atoi(val);
    if (steps > 8192) steps = 8192;       // 上限放宽到 8192 (~2 圈), 非阻塞
    if (steps < -8192) steps = -8192;
    if (delay_ms < 1) delay_ms = 4;
    if (delay_ms > 20) delay_ms = 20;

    int eff = dir >= 0 ? steps : -steps;
    ESP_LOGI(TAG, "motor %c dir=%d steps=%d delay=%dms (eff=%d) -> 后台执行", motor, dir, steps, delay_ms, eff);
    // 交给后台电机任务, 立即返回 (大步数也不卡网页)
    s_cmd.motor = motor;
    s_cmd.eff = eff;
    s_cmd.delay_ms = delay_ms;
    xSemaphoreGive(s_motor_sem);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"queued\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handle_log(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    httpd_resp_send(req, s_logbuf, s_log_len);
    xSemaphoreGive(s_log_mutex);
    return ESP_OK;
}

// =====================================================================
// DNS 劫持 (captive portal): 所有域名都解析到 192.168.4.1
// 手机连上热点后会探测联网(如 captive.apple.com / generate_204), 经此劫持
// 都打到本机 HTTP, 返回的不是预期响应 -> 系统判定为"需登录" -> 自动弹出网页
// =====================================================================
static void DnsTask(void* /*arg*/) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "dns socket fail"); vTaskDelete(NULL); return; }
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind fail"); close(sock); vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "DNS 劫持就绪 (任意域名 -> 192.168.4.1)");
    uint8_t rx[512], tx[600];
    while (true) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr*)&src, &slen);
        if (n < 12 || (rx[2] & 0x80)) continue;   // 太短或是响应包, 跳过

        memcpy(tx, rx, n);
        tx[2] |= 0x80;   // QR = 响应
        tx[3] |= 0x80;   // RA = 递归可用
        tx[6] = 0; tx[7] = 1;   // ANCOUNT = 1
        int p = n;
        tx[p++] = 0xC0; tx[p++] = 0x0C;            // 指向偏移12的问题名
        tx[p++] = 0; tx[p++] = 1;                  // TYPE A
        tx[p++] = 0; tx[p++] = 1;                  // CLASS IN
        tx[p++] = 0; tx[p++] = 0; tx[p++] = 0; tx[p++] = 60;  // TTL 60
        tx[p++] = 0; tx[p++] = 4;                  // RDLENGTH 4
        tx[p++] = kApIp[0]; tx[p++] = kApIp[1]; tx[p++] = kApIp[2]; tx[p++] = kApIp[3];
        sendto(sock, tx, p, 0, (struct sockaddr*)&src, slen);
    }
}

// ---- 非阻塞电机指令: 避免长步进阻塞 httpd (步数多也不卡网页) ----
static void MotorTask(void* /*arg*/) {
    while (true) {
        xSemaphoreTake(s_motor_sem, portMAX_DELAY);
        MotorCmd c = s_cmd;
        if (s_motor) {
            if (c.motor == 'a') s_motor->MoveSteps(MOTOR_NOD, c.eff, c.delay_ms);
            else                 s_motor->MoveSteps(MOTOR_SHAKE, c.eff, c.delay_ms);
        }
    }
}

// =====================================================================
// WiFi AP + HTTP 启动
// =====================================================================
static void StartSoftAp() {
    esp_netif_init();   // 上层已初始化则忽略
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wc = {};
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf((char*)wc.ap.ssid, sizeof(wc.ap.ssid), "Itoy-Debug-%02X%02X", mac[4], mac[5]);
    const char* pwd = CONFIG_ITOY_DEBUG_AP_PASSWORD;
    if (pwd && pwd[0]) {
        strncpy((char*)wc.ap.password, pwd, sizeof(wc.ap.password) - 1);
        wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wc.ap.authmode = WIFI_AUTH_OPEN;
    }
    wc.ap.max_connection = 2;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 降低发射功率(单位 0.25dBm, 40=10dBm; 默认~20dBm), 减小电流尖峰, 缓解供电 brownout
    // 近距离调试 10dBm 足够。仍是"连上就掉线"则基本就是电源撑不住 WiFi。
    esp_wifi_set_max_tx_power(40);

    ESP_LOGI(TAG, "调试 AP: SSID='%s' %s  连上后自动弹出网页 (captive portal)",
             (char*)wc.ap.ssid, wc.ap.authmode == WIFI_AUTH_OPEN ? "(开放)" : "(WPA2)");
}

static void StartHttp() {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;     // 电机移动在 handler 里阻塞, 给足栈
    config.uri_match_fn = httpd_uri_match_wildcard;   // 支持 /* 通配 (captive portal)
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    // 先注册精确的 API/根, 再注册 /* 兜底 (httpd 按注册顺序首个匹配生效)
    static const httpd_uri_t uri_root  = { .uri="/",          .method=HTTP_GET, .handler=handle_root  };
    static const httpd_uri_t uri_state = { .uri="/api/state", .method=HTTP_GET, .handler=handle_state };
    static const httpd_uri_t uri_motor = { .uri="/api/motor", .method=HTTP_GET, .handler=handle_motor };
    static const httpd_uri_t uri_log   = { .uri="/api/log",   .method=HTTP_GET, .handler=handle_log   };
    static const httpd_uri_t uri_catch = { .uri="/*",         .method=HTTP_GET, .handler=handle_catchall };
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_state);
    httpd_register_uri_handler(server, &uri_motor);
    httpd_register_uri_handler(server, &uri_log);
    httpd_register_uri_handler(server, &uri_catch);
}

void DebugWeb::Start(MotorControl* motor, PowerControl* power, TouchPad* touch) {
    s_motor = motor;
    s_power = power;
    s_touch = touch;

    LogCaptureInit();   // 尽早装日志捕获 (之后的日志都会进网页)
    StartSoftAp();
    xTaskCreate(DnsTask, "dns_hijack", 3072, NULL, 5, NULL);   // captive portal DNS 劫持
    s_motor_sem = xSemaphoreCreateBinary();
    xTaskCreate(MotorTask, "db_motor", 4096, NULL, 5, NULL);    // 后台电机执行 (非阻塞)
    StartHttp();
    ESP_LOGI(TAG, "调试网页就绪 (连上热点自动弹页)");
}

#endif  // CONFIG_ITOY_ENABLE_DEBUG_MODE
