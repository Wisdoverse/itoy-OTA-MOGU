<div align="center">

# 🔧 itoy-OTA

### ESP32-S3 OTA Firmware Framework for XiaoZhi AI Assistant

**ESP32-S3 OTA 固件框架 —— 适配小智 AI 助手**

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5.4-blue)](https://docs.espressif.com/projects/esp-idf/en/latest/)
[![Target](https://img.shields.io/badge/MCU-ESP32--S3-green)](https://www.espressif.com/en/products/socs/esp32-s3)
[![License](https://img.shields.io/badge/License-MIT-yellow)](LICENSE)

[English](#english) | [中文](#中文)

</div>

---

## English

### Overview

**itoy-OTA** is a lightweight, production-ready OTA (Over-The-Air) firmware update framework designed specifically for **ESP32-S3** devices. It serves as a bootstrap firmware that handles WiFi provisioning, firmware version checking, OTA downloading & flashing, and secure device activation.

Built for the **XiaoZhi (小智) AI voice assistant** ecosystem, it adapts to both:
- **XiaoZhi Official Cloud** (`api.tenclass.net`) — Tenclass managed backend
- **XiaoZhi Open-Source Server** — Self-hosted community backends
- **Custom OTA Servers** — Easily configurable via Kconfig

### Key Features

| Feature | Description |
|---------|-------------|
| 🔄 **Dual-Slot OTA** | A/B partition scheme with automatic rollback protection |
| 📡 **WiFi Provisioning** | SoftAP + web portal for credential configuration (SSID: `Itoy-XXXX`) |
| 🔐 **Secure Activation** | HMAC-SHA256 hardware-backed device authentication (ESP32 efuse) |
| 📦 **Multi-Size Flash** | Partition tables for 4MB / 8MB / 16MB / 32MB flash |
| 🎯 **Board Abstraction** | Clean factory pattern for swapping hardware variants |
| ⚡ **Factory Bootstrap** | Minimal first-stage firmware — boots, connects, updates, activates |
| 🔧 **Config Delivery** | OTA server delivers MQTT/WebSocket/time configs to NVS |
| 🛡️ **Rollback Safe** | Invalid firmware auto-reverts to previous working version |

### Architecture

```
┌─────────────────────────────────────────────┐
│                  app_main                   │
│  ┌─────────┐  ┌──────────┐  ┌───────────┐  │
│  │  NVS     │  │  Event   │  │  Board    │  │
│  │  Init    │  │  Loop    │  │  Factory  │  │
│  └─────────┘  └──────────┘  └─────┬─────┘  │
│                                     │        │
│  ┌──────────────────────────────────▼──────┐ │
│  │           WifiBoard                     │ │
│  │  ┌─────────────┐  ┌──────────────────┐  │ │
│  │  │ WiFi Station│  │ AP Config Portal │  │ │
│  │  └─────────────┘  └──────────────────┘  │ │
│  └─────────────────────────────────────────┘ │
│                                               │
│  ┌─────────────────────────────────────────┐ │
│  │              OTA Engine                 │ │
│  │  ┌──────────┐ ┌────────┐ ┌───────────┐ │ │
│  │  │  Version │ │Upgrade │ │ Activate  │ │ │
│  │  │  Check   │ │Download│ │ (HMAC)    │ │ │
│  │  └──────────┘ └────────┘ └───────────┘ │ │
│  └─────────────────────────────────────────┘ │
└─────────────────────────────────────────────┘
```

### Project Structure

```
itoy-OTA/
├── main/
│   ├── main.cc                   # Entry point
│   ├── ota.h / ota.cc            # OTA update & activation engine
│   ├── settings.h / settings.cc  # NVS key-value storage
│   ├── system_info.h / .cc       # Hardware info utilities
│   ├── device_state.h            # XiaoZhi device state enum
│   ├── device_state_event.h/.cc  # State change event system
│   └── boards/
│       ├── common/
│       │   ├── board.h / .cc     # Abstract Board base class
│       │   └── wifi_board.h/.cc  # WiFi board implementation
│       └── itoy-mogu/
│           ├── config.h          # Pin definitions & LCD configs
│           ├── config.json       # Board metadata
│           └── itoy-v1.0.cc      # ItoyMogu board class
├── partitions/
│   ├── v1/                       # Partition tables (no assets)
│   │   ├── 4m.csv / 8m.csv / 16m.csv / 32m.csv
│   └── v2/                       # Partition tables (with assets)
│       ├── 4m.csv / 8m.csv / 16m.csv / 32m.csv
├── CMakeLists.txt                # Top-level build
└── sdkconfig.defaults            # Default ESP-IDF config
```

### Partition Tables

Two versions of partition tables are provided for different flash sizes:

| Flash | V1 (Firmware Only) | V2 (Firmware + Assets) |
|-------|--------------------|------------------------|
| 4 MB  | Factory only, no OTA | Factory (1.5MB) + Assets (1.5MB) |
| 8 MB  | OTA_A + OTA_B (3.5MB each) | OTA_A + OTA_B (~3MB) + Assets (2MB) |
| 16 MB | OTA_A + OTA_B (6MB each) | OTA_A + OTA_B (~4MB) + Assets (8MB) ⭐ Default |
| 32 MB | OTA_A + OTA_B (12MB each) | OTA_A + OTA_B (4MB) + Assets (16MB) |

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

- **ESP-IDF v5.4+** installed ([Official Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/))
- ESP32-S3 development board (e.g., itoy-mogu)
- USB cable for flashing

#### Build & Flash

```bash
# Clone the repository
git clone https://github.com/LinchCHN/itoy-OTA.git
cd itoy-OTA

# Set ESP-IDF environment (adjust path to your installation)
. $HOME/esp/esp-idf/export.sh

# Set target
idf.py set-target esp32s3

# Configure (optional: change OTA URL, board type, etc.)
idf.py menuconfig
# Navigate to: OTA Configuration → OTA Server URL

# Build
idf.py build

# Flash to device
idf.py -p /dev/ttyUSB0 flash monitor
```

#### Custom OTA Server

Configure your own OTA server endpoint via menuconfig:

```bash
idf.py menuconfig
# → OTA Configuration
#   → OTA Server URL: https://your-server.com/api/ota/
```

Or modify `sdkconfig.defaults`:

```ini
CONFIG_OTA_URL="https://your-server.com/api/ota/"
```

#### Adding a New Board

1. Create a new directory under `main/boards/your-board/`
2. Implement the `Board` interface with a `config.h` for pin definitions
3. Register with `DECLARE_BOARD(YourBoardClass)` macro
4. Update `main/CMakeLists.txt` to include your board source files

### Hardware Reference (itoy-mogu)

| Function | GPIO | Function | GPIO |
|----------|------|----------|------|
| I2S MCLK | 38 | Display MOSI | 4 |
| I2S WS | 13 | Display CLK | 5 |
| I2S BCLK | 14 | Display DC | 7 |
| I2S DIN (Mic) | 12 | Display CS | 6 |
| I2S DOUT (Speaker) | 45 | Backlight | 8 |
| Codec I2C SDA | 1 | Built-in LED | 48 |
| Codec I2C SCL | 2 | Boot Button | 0 |
| Speaker Control | 15 | Battery ADC | 3 |
| SD Card Control | 21 | MCU Power | 9 |

**Audio Codec**: ES8311 (Speaker) + ES7210 (Mic) with PCA9557 I2C expander, 24kHz sample rate

**Display**: SPI LCD (ST7789 family), 240×320, with compile-time support for 15+ LCD variants

### Compatibility

| Backend | Status | Notes |
|---------|--------|-------|
| XiaoZhi Official (Tenclass) | ✅ Supported | Default OTA server |
| XiaoZhi Open-Source Server | ✅ Supported | Change `CONFIG_OTA_URL` |
| Custom OTA Server | ✅ Supported | Implement compatible API |
| XiaoZhi MQTT Broker | ✅ Config Delivery | Server delivers MQTT config |
| XiaoZhi WebSocket | ✅ Config Delivery | Server delivers WS endpoint |

### License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.

---

## 中文

### 项目简介

**itoy-OTA** 是一个专为 **ESP32-S3** 设计的轻量级、生产级 OTA（空中升级）固件框架。作为设备出厂引导固件，它负责 WiFi 配网、固件版本检测、OTA 下载烧录和设备安全激活。

本项目专为 **小智（XiaoZhi）AI 语音助手** 生态打造，完美适配：
- **小智官方云服务**（`api.tenclass.net`）—— Tenclass 运营后台
- **小智开源服务端** —— 社区自建后台
- **自定义 OTA 服务器** —— 通过 Kconfig 灵活配置

### 核心功能

| 功能 | 说明 |
|------|------|
| 🔄 **双分区 OTA** | A/B 分区方案，支持自动回滚保护 |
| 📡 **WiFi 配网** | SoftAP + 网页配置（SSID: `Itoy-XXXX`） |
| 🔐 **安全激活** | HMAC-SHA256 硬件认证（ESP32 efuse） |
| 📦 **多尺寸 Flash** | 4MB / 8MB / 16MB / 32MB 分区表 |
| 🎯 **板级抽象** | 工厂模式，轻松切换硬件型号 |
| ⚡ **出厂引导** | 最小化首阶段固件 —— 开机、联网、升级、激活 |
| 🔧 **配置下发** | OTA 服务器下发 MQTT/WebSocket/时间配置到 NVS |
| 🛡️ **安全回滚** | 新固件异常时自动恢复上一版本 |

### 快速开始

#### 环境要求

- **ESP-IDF v5.4+**（[官方安装指南](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/)）
- ESP32-S3 开发板（如 itoy-mogu）
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

# 配置（可选：修改 OTA 地址、板型等）
idf.py menuconfig
# 导航到：OTA Configuration → OTA Server URL

# 编译
idf.py build

# 烧录到设备
idf.py -p /dev/ttyUSB0 flash monitor
```

#### 自定义 OTA 服务器

通过 menuconfig 配置您自己的 OTA 服务器地址：

```bash
idf.py menuconfig
# → OTA Configuration
#   → OTA Server URL: https://your-server.com/api/ota/
```

或直接修改 `sdkconfig.defaults`：

```ini
CONFIG_OTA_URL="https://your-server.com/api/ota/"
```

#### 添加新板型

1. 在 `main/boards/your-board/` 下创建新目录
2. 实现 `Board` 接口，定义 `config.h` 引脚配置
3. 使用 `DECLARE_BOARD(YourBoardClass)` 宏注册
4. 更新 `main/CMakeLists.txt` 添加新板型源文件

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

### 硬件参考（itoy-mogu）

| 功能 | GPIO | 功能 | GPIO |
|------|------|------|------|
| I2S MCLK | 38 | 屏幕 MOSI | 4 |
| I2S WS | 13 | 屏幕 CLK | 5 |
| I2S BCLK | 14 | 屏幕 DC | 7 |
| I2S DIN（麦克风）| 12 | 屏幕 CS | 6 |
| I2S DOUT（扬声器）| 45 | 背光 | 8 |
| 编解码 I2C SDA | 1 | 内置 LED | 48 |
| 编解码 I2C SCL | 2 | Boot 键 | 0 |
| 扬声器控制 | 15 | 电池 ADC | 3 |
| SD 卡控制 | 21 | MCU 电源 | 9 |

**音频编解码**：ES8311（扬声器）+ ES7210（麦克风）+ PCA9557 I2C 扩展器，24kHz 采样率

**显示屏**：SPI LCD（ST7789 系列），240×320，编译时支持 15+ 种 LCD 屏幕方案

### 分区表

提供两种版本的分区表，适用于不同 Flash 大小：

| Flash | V1（仅固件） | V2（固件 + 资源） |
|-------|------------|------------------|
| 4 MB  | 仅出厂固件，不支持 OTA | 出厂 (1.5MB) + 资源 (1.5MB) |
| 8 MB  | OTA_A + OTA_B（各 3.5MB） | OTA_A + OTA_B（约 3MB）+ 资源 (2MB) |
| 16 MB | OTA_A + OTA_B（各 6MB） | OTA_A + OTA_B（约 4MB）+ 资源 (8MB) ⭐ 默认 |
| 32 MB | OTA_A + OTA_B（各 12MB） | OTA_A + OTA_B（4MB）+ 资源 (16MB) |

### 兼容性

| 后端 | 状态 | 说明 |
|------|------|------|
| 小智官方（Tenclass） | ✅ 支持 | 默认 OTA 服务器 |
| 小智开源服务端 | ✅ 支持 | 修改 `CONFIG_OTA_URL` 即可 |
| 自定义 OTA 服务器 | ✅ 支持 | 需实现兼容 API |
| 小智 MQTT 代理 | ✅ 配置下发 | 服务器下发 MQTT 配置 |
| 小智 WebSocket | ✅ 配置下发 | 服务器下发 WS 端点 |

### 致谢

- [ESP-IDF](https://github.com/espressif/esp-idf) — Espressif 官方开发框架
- [XiaoZhi](https://github.com/78/xiaozhi-esp32) — 小智 AI 语音助手开源项目
- [Tenclass](https://tenclass.net) — 小智官方运营团队

### 许可证

本项目采用 MIT 许可证 —— 详见 [LICENSE](LICENSE) 文件。

---

<div align="center">

**⭐ If this project helps you, please give it a star! / 如果这个项目对你有帮助，请点个 Star！**

</div>
