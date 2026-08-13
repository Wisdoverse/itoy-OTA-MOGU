<div align="center">

# 🍄 itoy-OTA

### ESP32-S3 OTA Firmware + Emotional Companion for the XiaoZhi AI Assistant

**ESP32-S3 OTA 固件框架 —— 适配小智 AI 助手的「情绪蘑菇」陪伴机器人**

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5.4-blue)](https://docs.espressif.com/projects/esp-idf/en/latest/)
[![Target](https://img.shields.io/badge/MCU-ESP32--S3--WROOM--1U-green)](https://www.espressif.com/en/products/socs/esp32-s3)
[![License](https://img.shields.io/badge/License-MIT-yellow)](LICENSE)

[English](#english) | [中文](#中文)

</div>

---

## English

### Overview

**itoy-OTA** started as a lightweight, production-ready OTA (Over-The-Air) firmware update framework for **ESP32-S3** devices — handling WiFi provisioning, firmware version checking, OTA download & flashing, and secure device activation.

On top of that bootstrap it now ships a complete **"itoy-mogu" (情绪蘑菇 / Emotional Mushroom)** board: an expressive companion robot that reacts to touch, battery and AI events through an RGB mood ring and two stepper-driven axes (nod + shake). It connects to the **XiaoZhi (小智) AI assistant** backend over MQTT/WebSocket.

It adapts to multiple backends:
- **gold-goat XiaoZhi backend** (`gold-goat-28797.zap.cloud`) — shared with `itoy-esp32`, MQTT-first transport, line-protocol over WS/MQTT + MCP
- **XiaoZhi Official Cloud** (`api.tenclass.net`) — Tenclass managed backend
- **XiaoZhi Open-Source Server** — self-hosted community backends
- **Custom OTA Servers** — configurable via Kconfig

### Key Features

| Feature | Description |
|---------|-------------|
| 🔄 **Dual-Slot OTA** | A/B partition scheme with automatic rollback protection |
| 📡 **WiFi Provisioning** | SoftAP captive portal **or** BLE, both with web credential configuration |
| 🔐 **Secure Activation** | HMAC-SHA256 hardware-backed device authentication (ESP32 efuse) |
| 📦 **Multi-Size Flash** | Partition tables for 4 MB / 8 MB / 16 MB / 32 MB flash |
| 🎯 **Board Abstraction** | Clean factory pattern (`DECLARE_BOARD`) for swapping hardware variants |
| 🍄 **Mood State Machine** | 11 emotional states driven by touch / battery / AI events, each with RGB + gesture choreography |
| 🤖 **Dual Stepper Gesture Engine** | 2× ULN2003 + 28BYJ-48 steppers (nod + shake), single-axis scripts **and** Bresenham dual-axis interpolation for diagonal/arc motion |
| 👆 **Capacitive Touch** | 4 touch pads grouped into left/right "hands" (short tap, hold, both-hold, rapid tap) |
| 🌈 **RGB Mood Ring** | WS2812B LED strip, per-mood color + brightness / breathing effects |
| 📟 **6-Axis IMU** | QMI8658A accelerometer + gyroscope over I2C |
| 🔊 **Voice I/O** | INMP441 I2S microphone + MAX98357 I2S amplifier for XiaoZhi audio |
| 🌐 **Web Debug Mode** | Captive-portal page to drive motors, replay moods, record zero / return-to-center, view live logs |
| ⚙️ **Three Run Modes** | Normal / Debug (web portal) / Motor self-test — mutually exclusive via Kconfig `choice` |
| 🛡️ **Rollback Safe** | Invalid firmware auto-reverts to the previous working version |

### Architecture

```
┌──────────────────────────────────────────────────┐
│                    app_main                       │
│  ┌─────────┐  ┌──────────┐  ┌────────────────┐   │
│  │  NVS    │  │  Event   │  │  Board Factory │   │
│  │  Init   │  │  Loop    │  │  (itoy-mogu)   │   │
│  └─────────┘  └──────────┘  └───────┬────────┘   │
│                                     │             │
│  ┌──────────────────────────────────▼──────────┐ │
│  │   WifiBoard  (Station + AP/BLE Provisioning)│ │
│  └──────────────────────┬──────────────────────┘ │
│                         │                         │
│  ┌──────────────────────▼──────────────────────┐ │
│  │   OTA Engine: version → download → flash    │ │
│  │   → mark valid (rollback) → activate (HMAC) │ │
│  └─────────────────────────────────────────────┘ │
│                                                   │
│  ┌─────────────────── itoy-mogu app ───────────┐ │
│  │ MoodController ◀── TouchPad / PowerControl  │ │
│  │      │                                       │ │
│  │      ├──▶ RgbLed   (mood colors)            │ │
│  │      ├──▶ MotorControl (nod + shake gesture)│ │
│  │      └──▶ BackendClient (XiaoZhi WS/MQTT)   │ │
│  └─────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────┘
```

### Project Structure

```
itoy-OTA/
├── main/
│   ├── main.cc                    # Entry point: NVS init, board factory, OTA
│   ├── ota.h / ota.cc             # OTA update & HMAC activation engine
│   ├── settings.h / settings.cc   # NVS key-value storage
│   ├── system_info.h / .cc        # Hardware info utilities
│   ├── device_state.h             # XiaoZhi device state enum
│   ├── device_state_event.h/.cc   # State change event system
│   └── boards/
│       ├── common/
│       │   ├── board.h / .cc              # Abstract Board base class
│       │   ├── wifi_board.h/.cc           # WiFi station + provisioning
│       │   └── wifi_provisioning_ble.h/.cc# BLE provisioning transport
│       └── itoy-mogu/                     # 🍄 Emotional Mushroom board
│           ├── config.h                   # Pin definitions & motor tunables
│           ├── config.json                # Board metadata
│           ├── itoy-v1.0.cc               # ItoyMogu board wiring class
│           ├── mood_controller.h/.cc      # Mood state machine (11 states)
│           ├── motor_control.h/.cc        # Dual stepper + gesture engine
│           ├── touch_pad.h/.cc            # 4-channel capacitive touch
│           ├── rgb_led.h/.cc              # WS2812B mood ring
│           ├── power_control.h/.cc        # Power latch / battery
│           ├── imu_qmi8658a.h/.cc         # QMI8658A 6-axis IMU
│           ├── backend_client.h/.cc       # XiaoZhi WS/MQTT + MCP
│           ├── debug_web.h/.cc            # Captive-portal debug page
│           └── CONTROL_LOGIC.md           # Architecture reference
├── partitions/
│   ├── v1/                   # Partition tables (firmware only)
│   └── v2/                   # Partition tables (firmware + assets)
├── CMakeLists.txt            # Top-level build
└── sdkconfig.defaults        # Default ESP-IDF config
```

### The itoy-mogu Companion Robot

#### Mood State Machine

The `MoodController` runs a 100 ms task that polls touch and battery, classifies events, and transitions between **11 emotional states**. Each state sets an RGB effect and plays a motor gesture on entry:

| State | Trigger | Reaction |
|-------|---------|----------|
| `POWER_ON` | Boot | Wake-up animation |
| `CALM` | Default idle | Soft warm light |
| `HAPPY` | Short tap (left/right) | Bright color + nod |
| `COMFORT` | Single-hand hold ≥3 s | Gentle hold pose + warm glow |
| `DEEP_BREATH` | Both-hands hold ≥5 s | Forward lean breathing |
| `SLEEPY` | 10 min no interaction | Dim breathing |
| `DISTURBED` | Rapid taps (5 s) | Shaky/disturbed motion |
| `LOW_BATTERY` | Battery below threshold | Slight head-down + warning |
| `NIGHT_LIGHT` | App / toggle | Low-brightness warm lamp |
| `HEAD_TURN` | Web debug demo | Coordinated neck-twist (nod + shake) |

Events: short tap, single-hand hold, both-hands hold, touch end, no-touch timeout, rapid touch, battery low/ok, night-light toggle. The backend can also push a mood via `RequestExternalMood()`.

#### Dual-Axis Stepper Gesture Engine

Two ULN2003-driven 28BYJ-48 steppers (8192 half-steps/rev): a **nod** motor (head, front/back) and a **shake** motor (neck, left/right). The engine provides two APIs:

- **`PlayGesture()`** — single-axis script of `GestureStep{motor, steps, delay}` (nod *or* shake per step).
- **`PlayDualGesture()`** — dual-axis coordinated motion using **Bresenham integer DDA interpolation**: the major axis (more steps) steps every tick, the minor axis steps by an error accumulator, producing smooth diagonal/arc trajectories. This is what powers the human-like "neck twist" in `HEAD_TURN`.

**Heat & positioning:** every "action" gesture calls with `auto_home=true`, so the motors **return to the recorded zero position and de-energize** on completion (preventing coil heat build-up). Because the potentiometer is not yet wired in, positioning is **open-loop step counting** — a signed `pos_` counter accumulates each step relative to the recorded zero. The web page exposes **记录零位 (record zero)** and **回中 (return to center)** buttons. A compile-time flag `MOTOR_HAS_POT` (in `config.h`) re-enables potentiometer soft-limits once the pots are connected — no other code changes needed.

#### Capacitive Touch

4 touch pads on GPIO1–4, grouped into a **left hand** (GPIO1+2) and **right hand** (GPIO3+4). Trigger ratio is configurable (`CONFIG_ITOY_TOUCH_RATIO`, default ×1.15 over baseline).

### Hardware Reference (itoy-mogu)

MCU: **ESP32-S3-WROOM-1U (N8R8)** — 8 MB flash, 8 MB PSRAM.

| Function | GPIO | Function | GPIO |
|----------|------|----------|------|
| Touch 1 (L) | 1 | Motor Nod A/B/C/D | 40 / 41 / 48 / 47 |
| Touch 2 (L) | 2 | Motor Shake A/B/C/D | 21 / 18 / 17 / 16 |
| Touch 3 (R) | 3 | Pot Nod (ADC1_CH5) | 6 |
| Touch 4 (R) | 4 | Pot Shake (ADC1_CH4) | 5 |
| Boot Button | 0 | Battery ADC (ADC1_CH6) | 7 |
| Mic I2S WS | 8 | Speaker I2S DIN | 10 |
| Mic I2S SD | 9 | Speaker I2S BCLK | 12 |
| Mic I2S SCK | 11 | Speaker I2S LRCLK | 13 |
| IMU I2C SDA | 14 | RGB LED (WS2812B) | 38 |
| IMU I2C SCL | 15 | Power ON detect | 39 |
| QMI8658A addr | 0x6B | Power Latch (soft-off) | 42 |
| USB D+ / D− | 19 / 20 | | |

**Audio**: INMP441 I2S microphone + MAX98357 I2S amplifier (XiaoZhi voice I/O).
**IMU**: QMI8658A 6-axis accelerometer + gyroscope (I2C @ 400 kHz).
**Motors**: 2× ULN2003 + 28BYJ-48 steppers (nod + shake).
**LED**: WS2812B RGB strip (count via `CONFIG_ITOY_RGB_LED_COUNT`, default 1).

### Run Modes (Kconfig `choice`)

Three mutually exclusive modes selected at compile time via `Component config → itoy Configuration → Run Mode`:

| Mode | Kconfig | Behavior |
|------|---------|----------|
| **Normal** | `ITOY_RUN_NORMAL` | Full mood robot + OTA |
| **Debug** | `ITOY_ENABLE_DEBUG_MODE` | Boots a captive-portal web page (motor control, mood replay, record zero / return-to-center, live logs) |
| **Self-test** | `ITOY_ENABLE_MOTOR_SELFTEST` | Spins each motor N revolutions forward/reverse for bench validation |

Other Kconfig options: provisioning transport (SoftAP/BLE), OTA server URL, RGB LED count, enable motor, enable IMU, touch ratio & baselines.

### Partition Tables

Two versions for different flash sizes:

| Flash | V1 (Firmware Only) | V2 (Firmware + Assets) |
|-------|--------------------|------------------------|
| 4 MB  | Factory only, no OTA | Factory (1.5 MB) + Assets (1.5 MB) |
| 8 MB  | OTA_A + OTA_B (3.5 MB each) | OTA_A + OTA_B (~3 MB) + Assets (2 MB) |
| 16 MB | OTA_A + OTA_B (6 MB each) | OTA_A + OTA_B (~4 MB) + Assets (8 MB) ⭐ Default |
| 32 MB | OTA_A + OTA_B (12 MB each) | OTA_A + OTA_B (4 MB) + Assets (16 MB) |

### OTA Flow

```
Power On → WiFi Connect → Check Version ──→ No Update → Idle
                               │
                               ▼ Yes
                          Download Firmware
                               │
                               ▼
                          Flash to Alternate Partition
                               │
                               ▼
                          Reboot into New Firmware
                               │
                               ▼
                          Mark Valid (Rollback Protection)
                               │
                               ▼
                          Device Activation (Optional)
```

### Quick Start

#### Prerequisites

- **ESP-IDF v5.5.4** ([Official Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/))
- ESP32-S3-WROOM-1U board (itoy-mogu)
- USB cable for flashing

#### Build & Flash

```bash
# Clone
git clone https://github.com/LinchCHN/itoy-OTA.git
cd itoy-OTA

# Set ESP-IDF environment (adjust to your install path)
. $HOME/esp/esp-idf/export.sh

# Set target
idf.py set-target esp32s3

# Configure: run mode, OTA URL, provisioning transport, etc.
idf.py menuconfig
# → Component config → itoy Configuration

# Build
idf.py build

# Flash & monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

#### Debug Web Mode

Select **Debug** run mode in menuconfig, flash, and join the provisioning AP. The captive portal lets you:
- Jog nod/shake motors by N steps
- Replay any mood's RGB + gesture (including **转头 / HEAD_TURN**)
- **记录零位** (record current position as zero) and **回中** (return to center)
- Watch live system state, touch readings, battery, and logs

#### Adding a New Board

1. Create a directory under `main/boards/your-board/`
2. Implement the `Board` interface with a `config.h` for pin definitions
3. Register with the `DECLARE_BOARD(YourBoardClass)` macro
4. Add your sources to `main/CMakeLists.txt`

### Compatibility

| Backend | Status | Notes |
|---------|--------|-------|
| gold-goat XiaoZhi (MQTT-first) | ✅ Supported | Default; shares backend with `itoy-esp32`, WS/MQTT + MCP |
| XiaoZhi Official (Tenclass) | ✅ Supported | `api.tenclass.net` |
| XiaoZhi Open-Source Server | ✅ Supported | Change `CONFIG_OTA_URL` |
| Custom OTA Server | ✅ Supported | Implement compatible API |

### License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.

---

## 中文

### 项目简介

**itoy-OTA** 最初是一个为 **ESP32-S3** 设计的轻量级、生产级 OTA（空中升级）固件框架，负责 WiFi 配网、固件版本检测、OTA 下载烧录和设备安全激活。

在此引导框架之上，现已搭载完整的 **「itoy-mogu 情绪蘑菇」** 板型：一款会表达的陪伴机器人，通过 RGB 情绪灯环和两个步进电机轴（点头 + 摇头）对触摸、电量与 AI 事件做出反应，并通过 MQTT/WebSocket 接入 **小智（XiaoZhi）AI 助手** 后端。

适配多种后端：
- **gold-goat 小智后端**（`gold-goat-28797.zap.cloud`）—— 与 `itoy-esp32` 共用，MQTT 优先传输，WS/MQTT 线格式 + MCP
- **小智官方云服务**（`api.tenclass.net`）—— Tenclass 运营后台
- **小智开源服务端** —— 社区自建后台
- **自定义 OTA 服务器** —— 通过 Kconfig 灵活配置

### 核心功能

| 功能 | 说明 |
|------|------|
| 🔄 **双分区 OTA** | A/B 分区方案，支持自动回滚保护 |
| 📡 **WiFi 配网** | SoftAP 强制门户 **或** BLE，均带网页配网 |
| 🔐 **安全激活** | HMAC-SHA256 硬件认证（ESP32 efuse） |
| 📦 **多尺寸 Flash** | 4MB / 8MB / 16MB / 32MB 分区表 |
| 🎯 **板级抽象** | 工厂模式（`DECLARE_BOARD`），轻松切换硬件型号 |
| 🍄 **情绪状态机** | 11 种情绪状态，由触摸/电量/AI 事件驱动，各带 RGB + 手势编排 |
| 🤖 **双轴步进手势引擎** | 2 路 ULN2003 + 28BYJ-48 步进（点头 + 摇头），支持单轴脚本与 Bresenham 双轴插值的斜向/弧线运动 |
| 👆 **电容触摸** | 4 路触摸铜片分为左/右「手」（短按、长按、双手长按、连按） |
| 🌈 **RGB 情绪灯环** | WS2812B 灯带，按情绪切换颜色 + 亮度/呼吸效果 |
| 📟 **六轴 IMU** | QMI8658A 加速度计 + 陀螺仪（I2C） |
| 🔊 **语音输入输出** | INMP441 I2S 麦克风 + MAX98357 I2S 功放，对接小智音频 |
| 🌐 **网页调试模式** | 强制门户页面：控制电机、重放情绪、记录零位/回中、查看实时日志 |
| ⚙️ **三种运行模式** | 正常 / 调试（网页门户）/ 电机自检 —— Kconfig `choice` 三选一互斥 |
| 🛡️ **安全回滚** | 新固件异常时自动恢复上一版本 |

### 架构

```
┌──────────────────────────────────────────────────┐
│                    app_main                       │
│  ┌─────────┐  ┌──────────┐  ┌────────────────┐   │
│  │  NVS    │  │  事件    │  │  板级工厂      │   │
│  │  初始化 │  │  循环    │  │  (itoy-mogu)   │   │
│  └─────────┘  └──────────┘  └───────┬────────┘   │
│                                     │             │
│  ┌──────────────────────────────────▼──────────┐ │
│  │   WifiBoard  (Station + AP/BLE 配网)        │ │
│  └──────────────────────┬──────────────────────┘ │
│                         │                         │
│  ┌──────────────────────▼──────────────────────┐ │
│  │   OTA 引擎: 版本检测 → 下载 → 烧录          │ │
│  │   → 标记有效(回滚) → 激活(HMAC)             │ │
│  └─────────────────────────────────────────────┘ │
│                                                   │
│  ┌─────────────────── itoy-mogu 应用 ──────────┐ │
│  │ MoodController ◀── TouchPad / PowerControl  │ │
│  │      │                                       │ │
│  │      ├──▶ RgbLed   (情绪灯光)               │ │
│  │      ├──▶ MotorControl (点头+摇头手势)      │ │
│  │      └──▶ BackendClient (小智 WS/MQTT)      │ │
│  └─────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────┘
```

### 项目结构

```
itoy-OTA/
├── main/
│   ├── main.cc                    # 入口: NVS 初始化、板级工厂、OTA
│   ├── ota.h / ota.cc             # OTA 升级 + HMAC 激活引擎
│   ├── settings.h / settings.cc   # NVS 键值存储
│   ├── system_info.h / .cc        # 硬件信息工具
│   ├── device_state.h             # 小智设备状态枚举
│   ├── device_state_event.h/.cc   # 状态变更事件系统
│   └── boards/
│       ├── common/
│       │   ├── board.h / .cc              # 抽象 Board 基类
│       │   ├── wifi_board.h/.cc           # WiFi 联网 + 配网
│       │   └── wifi_provisioning_ble.h/.cc# BLE 配网传输
│       └── itoy-mogu/                     # 🍄 情绪蘑菇板型
│           ├── config.h                   # 引脚定义 & 电机参数
│           ├── config.json                # 板型元数据
│           ├── itoy-v1.0.cc               # ItoyMogu 板级装配类
│           ├── mood_controller.h/.cc      # 情绪状态机 (11 态)
│           ├── motor_control.h/.cc        # 双步进 + 手势引擎
│           ├── touch_pad.h/.cc            # 4 路电容触摸
│           ├── rgb_led.h/.cc              # WS2812B 情绪灯环
│           ├── power_control.h/.cc        # 电源锁存 / 电池
│           ├── imu_qmi8658a.h/.cc         # QMI8658A 六轴 IMU
│           ├── backend_client.h/.cc       # 小智 WS/MQTT + MCP
│           ├── debug_web.h/.cc            # 强制门户调试页
│           └── CONTROL_LOGIC.md           # 架构说明
├── partitions/
│   ├── v1/                   # 分区表 (仅固件)
│   └── v2/                   # 分区表 (固件 + 资源)
├── CMakeLists.txt            # 顶层构建
└── sdkconfig.defaults        # 默认 ESP-IDF 配置
```

### itoy-mogu 陪伴机器人

#### 情绪状态机

`MoodController` 运行一个 100ms 任务，轮询触摸与电量、分类事件，并在 **11 种情绪状态** 间转换。每个状态在进入时设置 RGB 效果并播放电机手势：

| 状态 | 触发 | 反应 |
|------|------|------|
| `POWER_ON` | 开机 | 开机动画 |
| `CALM` | 默认待机 | 柔和暖光 |
| `HAPPY` | 短按（左/右） | 明亮颜色 + 点头 |
| `COMFORT` | 单手握 ≥3s | 轻柔保持姿态 + 暖光 |
| `DEEP_BREATH` | 双手握 ≥5s | 前倾深呼吸 |
| `SLEEPY` | 10 分钟无互动 | 暗淡呼吸 |
| `DISTURBED` | 频繁短按 (5s 内) | 抖动/受扰动作 |
| `LOW_BATTERY` | 电量低于阈值 | 轻微低头 + 预警 |
| `NIGHT_LIGHT` | App / 切换 | 低亮度暖灯 |
| `HEAD_TURN` | 网页调试演示 | 点头+摇头联动的扭脖 |

事件：短按、单手长按、双手长按、触摸结束、无互动超时、频繁短按、电量低/正常、夜灯切换。后端也可通过 `RequestExternalMood()` 下发情绪。

#### 双轴步进手势引擎

两路 ULN2003 驱动的 28BYJ-48 步进电机（半步 8192 步/圈）：**点头**电机（头部，前后）和**摇头**电机（颈部，左右）。引擎提供两套 API：

- **`PlayGesture()`** —— 单轴脚本 `GestureStep{motor, steps, delay}`（每步只动点头或摇头之一）。
- **`PlayDualGesture()`** —— 双轴联动，采用 **Bresenham 整数 DDA 插值**：步数多的主轴每个 tick 走一步，副轴按误差累加器决定是否走一步，生成平滑的斜向/弧线轨迹。这正是 `HEAD_TURN` 中类人「扭脖」的实现方式。

**发热与定位**：每个「动作」类手势都以 `auto_home=true` 调用，电机在动作结束后**回到已记录的零位并断电**（避免线圈持续通电发热）。由于电位器尚未接入，定位采用**开环步数累计** —— 用带符号的 `pos_` 计数器累计每一步相对于零位的偏移。网页提供 **记录零位** 和 **回中** 按钮。编译期开关 `MOTOR_HAS_POT`（在 `config.h`）在电位器接好后重新启用软限位，无需改动其它代码。

#### 电容触摸

GPIO1–4 上的 4 路触摸铜片，分为**左手**（GPIO1+2）和**右手**（GPIO3+4）。触发倍率可调（`CONFIG_ITOY_TOUCH_RATIO`，默认基线的 ×1.15）。

### 硬件参考（itoy-mogu）

MCU：**ESP32-S3-WROOM-1U (N8R8)** —— 8MB Flash，8MB PSRAM。

| 功能 | GPIO | 功能 | GPIO |
|------|------|------|------|
| 触摸 1（左） | 1 | 点头电机 A/B/C/D | 40 / 41 / 48 / 47 |
| 触摸 2（左） | 2 | 摇头电机 A/B/C/D | 21 / 18 / 17 / 16 |
| 触摸 3（右） | 3 | 点头电位器 (ADC1_CH5) | 6 |
| 触摸 4（右） | 4 | 摇头电位器 (ADC1_CH4) | 5 |
| Boot 键 | 0 | 电池 ADC (ADC1_CH6) | 7 |
| 麦克风 I2S WS | 8 | 喇叭 I2S DIN | 10 |
| 麦克风 I2S SD | 9 | 喇叭 I2S BCLK | 12 |
| 麦克风 I2S SCK | 11 | 喇叭 I2S LRCLK | 13 |
| IMU I2C SDA | 14 | RGB 灯 (WS2812B) | 38 |
| IMU I2C SCL | 15 | 电源按键检测 | 39 |
| QMI8658A 地址 | 0x6B | 电源锁存（软关机）| 42 |
| USB D+ / D− | 19 / 20 | | |

**音频**：INMP441 I2S 麦克风 + MAX98357 I2S 功放（小智语音输入输出）。
**IMU**：QMI8658A 六轴加速度计 + 陀螺仪（I2C @ 400kHz）。
**电机**：2 路 ULN2003 + 28BYJ-48 步进（点头 + 摇头）。
**LED**：WS2812B RGB 灯带（数量由 `CONFIG_ITOY_RGB_LED_COUNT` 配置，默认 1）。

### 运行模式（Kconfig `choice`）

编译期在 `Component config → itoy Configuration → Run Mode` 三选一互斥：

| 模式 | Kconfig | 行为 |
|------|---------|------|
| **正常** | `ITOY_RUN_NORMAL` | 完整情绪机器人 + OTA |
| **调试** | `ITOY_ENABLE_DEBUG_MODE` | 启动强制门户网页（电机控制、情绪重放、记录零位/回中、实时日志）|
| **自检** | `ITOY_ENABLE_MOTOR_SELFTEST` | 每个电机正/反各转 N 圈，便于裸板验证 |

其它 Kconfig 项：配网方式（SoftAP/BLE）、OTA 服务器地址、RGB 灯珠数、启用电机、启用 IMU、触摸倍率与基线。

### 分区表

提供两种版本，适用于不同 Flash 大小：

| Flash | V1（仅固件） | V2（固件 + 资源） |
|-------|------------|------------------|
| 4 MB  | 仅出厂固件，不支持 OTA | 出厂 (1.5MB) + 资源 (1.5MB) |
| 8 MB  | OTA_A + OTA_B（各 3.5MB） | OTA_A + OTA_B（约 3MB）+ 资源 (2MB) |
| 16 MB | OTA_A + OTA_B（各 6MB） | OTA_A + OTA_B（约 4MB）+ 资源 (8MB) ⭐ 默认 |
| 32 MB | OTA_A + OTA_B（各 12MB） | OTA_A + OTA_B（4MB）+ 资源 (16MB) |

### OTA 工作流程

```
开机上电 → WiFi 连接 → 检查版本 ──→ 无更新 → 待机
                           │
                           ▼ 有新版本
                      下载固件
                           │
                           ▼
                      烧录到备用分区
                           │
                           ▼
                      重启至新固件
                           │
                           ▼
                      标记有效（回滚保护）
                           │
                           ▼
                      设备激活（可选）
```

### 快速开始

#### 环境要求

- **ESP-IDF v5.5.4**（[官方安装指南](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/)）
- ESP32-S3-WROOM-1U 开发板（itoy-mogu）
- USB 数据线

#### 编译与烧录

```bash
# 克隆仓库
git clone https://github.com/LinchCHN/itoy-OTA.git
cd itoy-OTA

# 设置 ESP-IDF 环境（请根据实际安装路径调整）
. $HOME/esp/esp-idf/export.sh

# 设置目标芯片
idf.py set-target esp32s3

# 配置：运行模式、OTA 地址、配网方式等
idf.py menuconfig
# → Component config → itoy Configuration

# 编译
idf.py build

# 烧录并监视
idf.py -p /dev/ttyUSB0 flash monitor
```

#### 网页调试模式

在 menuconfig 选择 **调试** 模式，烧录后连接配网 AP，强制门户页面可：
- 按步数点动 点头/摇头 电机
- 重放任一情绪的 RGB + 手势（含 **转头 / HEAD_TURN**）
- **记录零位**（把当前位置设为零点）与 **回中**（回到零点）
- 实时查看系统状态、触摸读数、电量与日志

#### 添加新板型

1. 在 `main/boards/your-board/` 下创建新目录
2. 实现 `Board` 接口，定义 `config.h` 引脚配置
3. 使用 `DECLARE_BOARD(YourBoardClass)` 宏注册
4. 在 `main/CMakeLists.txt` 添加新板型源文件

### 兼容性

| 后端 | 状态 | 说明 |
|------|------|------|
| gold-goat 小智（MQTT 优先）| ✅ 支持 | 默认；与 `itoy-esp32` 共用后端，WS/MQTT + MCP |
| 小智官方（Tenclass） | ✅ 支持 | `api.tenclass.net` |
| 小智开源服务端 | ✅ 支持 | 修改 `CONFIG_OTA_URL` 即可 |
| 自定义 OTA 服务器 | ✅ 支持 | 需实现兼容 API |

### 致谢

- [ESP-IDF](https://github.com/espressif/esp-idf) —— Espressif 官方开发框架
- [XiaoZhi](https://github.com/78/xiaozhi-esp32) —— 小智 AI 语音助手开源项目
- [Tenclass](https://tenclass.net) —— 小智官方运营团队

### 许可证

本项目采用 MIT 许可证 —— 详见 [LICENSE](LICENSE) 文件。

---

<div align="center">

**⭐ If this project helps you, please give it a star! / 如果这个项目对你有帮助，请点个 Star！**

</div>
