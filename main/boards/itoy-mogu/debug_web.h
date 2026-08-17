#ifndef DEBUG_WEB_H_
#define DEBUG_WEB_H_

#include "sdkconfig.h"

// 调试模式: WiFi 热点 + 网页调试 (仅 CONFIG_ITOY_ENABLE_DEBUG_MODE 时编译)
// 网页: 控电机 / 控灯光 / 看电位器 / 电池 / 串口日志。关闭调试模式则本模块整体不编译。
#if CONFIG_ITOY_ENABLE_DEBUG_MODE

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

#endif  // CONFIG_ITOY_ENABLE_DEBUG_MODE

#endif  // DEBUG_WEB_H_
