#include "sdkconfig.h"
#if CONFIG_ITOY_ENABLE_DEBUG_MODE

#include "debug_web.h"
#include "config.h"
#include "motor_control.h"
#include "power_control.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#define TAG "DebugWeb"

// ---- 文件级上下文 ----
static MotorControl* s_motor = nullptr;
static PowerControl* s_power = nullptr;

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
"步数:<input id='steps' type='number' value='512' size='6'>"
"<br><button onclick=mv('a',1)>点头 +</button>"
"<button onclick=mv('a',-1)>点头 −</button>"
"<button class=b></button>"
"<button onclick=mv('b',1)>摇头 +</button>"
"<button onclick=mv('b',-1)>摇头 −</button>"
"<h2>状态</h2><div class=st id=state>读取中…</div>"
"<h2>串口日志</h2><textarea id=log readonly></textarea>"
"<script>"
"async function state(){try{let r=await fetch('/api/state');"
"let j=await r.json();"
"document.getElementById('state').innerHTML="
"'点头电位器: '+j.nod+'<br>摇头电位器: '+j.shake"
"+'<br>电池: '+j.batt_mv+' mV ('+j.batt_pct+'%)';}catch(e){}}"
"async function log(){try{let r=await fetch('/api/log');"
"let t=await r.text();let e=document.getElementById('log');"
"e.value=t;e.scrollTop=e.scrollHeight;}catch(e){}}"
"async function mv(m,d){let s=parseInt(document.getElementById('steps').value)||0;"
"if(Math.abs(s)>2000){alert('步数限制 2000');return;}"
"await fetch('/api/motor?m='+m+'&dir='+d+'&steps='+s);}"
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

static esp_err_t handle_state(httpd_req_t* req) {
    char json[160];
    snprintf(json, sizeof(json),
             "{\"nod\":%lu,\"shake\":%lu,\"batt_mv\":%d,\"batt_pct\":%d}",
             s_motor ? s_motor->ReadNodPosition() : 0,
             s_motor ? s_motor->ReadShakePosition() : 0,
             s_power ? s_power->ReadBatteryMv() : 0,
             s_power ? s_power->ReadBatteryPercent() : 0);
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
    int dir = 1, steps = 0;
    if (httpd_query_key_value(query, "m", val, sizeof(val)) == ESP_OK) motor = val[0];
    if (httpd_query_key_value(query, "dir", val, sizeof(val)) == ESP_OK) dir = atoi(val);
    if (httpd_query_key_value(query, "steps", val, sizeof(val)) == ESP_OK) steps = atoi(val);
    if (steps > 2000) steps = 2000;
    if (steps < -2000) steps = -2000;

    int eff = dir >= 0 ? steps : -steps;
    ESP_LOGI(TAG, "motor %c dir=%d steps=%d (eff=%d)", motor, dir, steps, eff);
    if (s_motor) {
        if (motor == 'a') s_motor->NodSteps(eff);
        else              s_motor->ShakeSteps(eff);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
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

    ESP_LOGI(TAG, "调试 AP: SSID='%s' %s  网页: http://192.168.4.1",
             (char*)wc.ap.ssid, wc.ap.authmode == WIFI_AUTH_OPEN ? "(开放)" : "(WPA2)");
}

static void StartHttp() {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;     // 电机移动在 handler 里阻塞, 给足栈
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    static const httpd_uri_t uri_root  = { .uri="/",          .method=HTTP_GET, .handler=handle_root  };
    static const httpd_uri_t uri_state = { .uri="/api/state", .method=HTTP_GET, .handler=handle_state };
    static const httpd_uri_t uri_motor = { .uri="/api/motor", .method=HTTP_GET, .handler=handle_motor };
    static const httpd_uri_t uri_log   = { .uri="/api/log",   .method=HTTP_GET, .handler=handle_log   };
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_state);
    httpd_register_uri_handler(server, &uri_motor);
    httpd_register_uri_handler(server, &uri_log);
}

void DebugWeb::Start(MotorControl* motor, PowerControl* power) {
    s_motor = motor;
    s_power = power;

    LogCaptureInit();   // 尽早装日志捕获 (之后的日志都会进网页)
    StartSoftAp();
    StartHttp();
    ESP_LOGI(TAG, "调试网页就绪");
}

#endif  // CONFIG_ITOY_ENABLE_DEBUG_MODE
