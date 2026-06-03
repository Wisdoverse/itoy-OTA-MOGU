#ifndef _DEVICE_STATE_H_
#define _DEVICE_STATE_H_

enum DeviceState {
    kDeviceStateUnknown,          // 未知状态 - 设备初始状态或状态不确定时
    kDeviceStateStarting,         // 启动中 - 设备正在启动初始化
    kDeviceStateWifiConfiguring,  // WiFi配置中 - 正在配置WiFi网络连接
    kDeviceStateIdle,             // 空闲 - 设备待机状态，等待用户交互
    kDeviceStateConnecting,       // 连接中 - 正在连接到语音服务器
    kDeviceStateListening,        // 监听中 - 正在听取用户语音输入
    kDeviceStateSpeaking,         // 说话中 - 正在播放AI回复的语音
    kDeviceStateUpgrading,        // 升级中 - 正在进行OTA固件升级
    kDeviceStateActivating,       // 激活中 - 设备正在进行激活流程
    kDeviceStateAudioTesting,     // 音频测试中 - 正在测试音频输入输出功能

    kDeviceStateDisplaying,       // 显示中 - 专门用于显示内容的模式（预留状态）

    kDeviceStateFatalError        // 致命错误 - 发生不可恢复的系统错误
};

#endif // _DEVICE_STATE_H_ 