#ifndef DEBUG_WEB_H_
#define DEBUG_WEB_H_

#include "sdkconfig.h"

// 调试网页: WiFi 热点 + 网页调试 (功能验证 ITOY_ENABLE_DEBUG_MODE 或
// 生产调试 ITOY_RUN_PROD_DEBUG 时编译)。两档共用页面:
//   功能验证 — 情绪按钮直接重放手势 (无自动状态机);
//   生产调试 — 完整状态机在跑, 网页命令经 mood 邮箱由其任务执行。
#if CONFIG_ITOY_ENABLE_DEBUG_MODE || CONFIG_ITOY_RUN_PROD_DEBUG

class MotorControl;
class PowerControl;
class TouchPad;
class MoodController;
class RgbLed;

class DebugWeb {
public:
    // 启动 WiFi AP + HTTP 服务 (后台常驻), 立即返回
    // mood 用于网页"情绪演示"按钮 -> DemoState() 重放该情绪的灯光+手势;
    // rgb 用于网页"灯光控制" -> Solid/Off 直接点灯 (与情绪演示共用, 后点覆盖)
    void Start(MotorControl* motor, PowerControl* power, TouchPad* touch,
               MoodController* mood, RgbLed* rgb);
};

#endif  // CONFIG_ITOY_ENABLE_DEBUG_MODE || CONFIG_ITOY_RUN_PROD_DEBUG

#endif  // DEBUG_WEB_H_
