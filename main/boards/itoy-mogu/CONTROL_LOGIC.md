# itoy-mogu 控制逻辑

> 本文档描述 itoy-mogu PCB 的引脚映射与触摸→电机控制行为。
> 引脚定义来源：PCB PADS 网表（2026-07-22）。对应代码：`config.h`、`motor_control.*`、`touch_pad.*`、`power_control.*`、`itoy-v1.0.cc`。

## 1. 引脚总表（ESP32-S3-WROOM-1U）

| 功能 | GPIO | 网表来源 | 备注 |
|---|---|---|---|
| **摇头电机 U16**（左右）via U8 | 21 / 18 / 17 / 16 (A/B/C/D) | A1/B1/C1/D1 | ULN2003 半步 |
| **点头电机 U17**（前后）via U9 | 41 / 40 / 48 / 47 (A/B/C/D) | A2/B2/C2/D2 | ULN2003 半步 |
| 摇头电位器 U5 | 5 | ADC1 | ADC1_CH4，位置反馈 |
| 点头电位器 U12 | 6 | ADC2 | ADC1_CH5，位置反馈 |
| 电池电压 BAT | 7 | BAT | ADC1_CH6，R7/R6 分压 |
| 触摸铜片 ×4 | 1 / 2 / 3 / 4 | IO1/2/3/4 | Touch1–4，不含 GPIO0 |
| IMU QMI8658A | SDA 14 / SCL 15 | I2C_SDA/SCL | I2C_NUM_0, 400kHz, 0x6B |
| **RGB 灯带 WS2812B** | 38 (DIN) | U13 连接器 | GND/3V3/DIN 三线，灯珠数见 Kconfig |
| 电源 ON 锁存 | 42 | IO42 | 拉低触发软关机 |
| 电源按键检测 | 39 | IO39 | SW4 经 Q3，长按 3s 关机 |
| 麦克风 I2S | WS 8 / SD 9 / SCK 11 | I2S_WS/SD/SCK | INMP441 |
| 喇叭 I2S | DIN 10 / BCLK 12 / LRCLK 13 | DIN/BCLK/IO13 | MAX98357 |
| USB D+/D- | 19 / 20 | IO19/IO20 | 烧录口，不可占用 |

> ADC1 通道对照（ESP32-S3）：GPIO5=CH4，GPIO6=CH5，GPIO7=CH6。
> GPIO0 为 BOOT 键，**不是**触摸通道。

## 2. 触摸铜片 → 电机 映射

共 4 个触摸铜片，2 个电机各管正/反两个方向。**按住即动，松开即停**。

| 触摸通道 | GPIO | 动作 |
|---|---|---|
| ch0 (Touch1) | GPIO1 | 点头正转（向前） |
| ch1 (Touch2) | GPIO2 | 点头反转（向后） |
| ch2 (Touch3) | GPIO3 | 摇头正转（向左） |
| ch3 (Touch4) | GPIO4 | 摇头反转（向右） |

- 同一电机的两个方向互斥（前/后取先按下的那个）。
- 两个电机可同时动作（例如同时点头+摇头）。
- 任意 pad 状态变化时，按当前 4 路实时触摸状态重算两个电机方向，因此**释放**会正确停掉对应电机（见 `itoy-v1.0.cc::OnTouchEvent`）。

## 3. 行为细节

### 按住即动
- 触摸扫描任务 30ms 轮询，检测按下/松开 → 设置 `MotorControl::Drive(id, ±1)`。
- 电机步进消费任务以 `MOTOR_STEP_DELAY_MS`（默认 2ms）节奏推进活跃电机；空闲时 50ms 长睡省电。
- 松开 → 该电机断电（线圈 off），不发热。

### 电位器软限位（防机械顶死）
- 每次步进前读该轴电位器；超出 `[POT_RANGE_MIN_PCT, POT_RANGE_MAX_PCT]`（默认 10%~90% 量程）则该方向停止并断电。
- 反方向仍可移动。限位值在 `config.h` 的 `POT_RANGE_MIN_PCT / POT_RANGE_MAX_PCT` 调。

### 方向反转开关
- 实际转动方向与预期相反时，改 `config.h`：
  - `MOTOR_NOD_INVERT = 1` / `MOTOR_SHAKE_INVERT = 1` —— 翻转正负方向。
  - `MOTOR_NOD_POT_CW_INC` / `MOTOR_SHAKE_POT_CW_INC` —— 正转是否使电位器读数增大；**若软限位在刚起步就立刻触发，把对应项取反**。

## 4. 手势 API（供应用层 / AI 后续调用）

`MotorControl`（见 `motor_control.h`）：

| 方法 | 说明 |
|---|---|
| `Drive(MotorId, ±1/0)` | 触摸驱动入口（按住即动） |
| `StopAll()` | 停止并断电所有电机 |
| `NodSteps(int n)` / `ShakeSteps(int n)` | 点头/摇头固定步数（受软限位保护） |
| `MoveToPercent(MotorId, 0~100)` | 步进到目标电位器百分比（带最大步数保护与 ~4% 死区） |
| `ReadNod/ShakePosition()` | 读电位器原始值（0~4095） |

## 5. 电源
- 开机：SW4 → Q3 → GPIO39 拉低启动 → ESP32 启动后 GPIO42 置高锁存供电。
- 关机：长按 SW4 ≥3s 软关机，或软件 `PowerControl::RequestShutdown()`（拉低 GPIO42）。
- 电池电压：`ReadBatteryMv()`，分压比 `BATT_DIVIDER_RATIO`（默认 2.0，待知 R6/R7 阻值后校准）。
- ADC 共享：`MotorControl` 创建唯一 `ADC_UNIT_1` 单元，配置 3 通道（点头/摇头/电池）；`PowerControl::SetBatteryAdc()` 复用同一句柄，避免重复创建冲突。

## 6. RGB 灯带 (WS2812B)

- **硬件**：WS2812B 灯带经 **U13 连接器** 接入（GND / 3V3 / DIN 三线），**GPIO38 = DIN**。
- **灯珠数量**：通过 Kconfig 配置 —— `menuconfig` → itoy OTA 配置 → `WS2812B RGB 灯珠数量`（`CONFIG_ITOY_RGB_LED_COUNT`，默认 1）。按实际灯带长度修改后重新编译。
- **驱动**：`RgbLed`（`rgb_led.h/.cc`），基于 `led_strip` 组件的 RMT 后端，GRB 顺序，10MHz 时序。

API（`MotorControl`/`RgbLed` 等通过 `ItoyMogu::GetRgb()` 获取）：

| 方法 | 说明 |
|---|---|
| `Initialize()` | 初始化灯带（板级已调用），默认熄灭 |
| `SetPixel(idx, r, g, b)` | 单灯颜色（0~count-1），需再 `Refresh()` |
| `Fill(r, g, b)` | 全部灯同色 |
| `Clear()` | 全部熄灭 |
| `Refresh()` | 把缓冲推送到灯带（设色后必须调用） |
| `SetBrightness(0~255)` | 全局亮度缩放 |
| `count()` | 当前灯珠数 |

示例（应用层点亮第一个灯红色）：
```cpp
auto& rgb = board.GetRgb();
rgb.SetBrightness(60);        // 限流/限亮
rgb.SetPixel(0, 255, 0, 0);  // GRB: 红
rgb.Refresh();
```

> 上电默认熄灭，避免长灯带在 3V3 上瞬间大电流。需要状态指示（如触摸时变色）可在 `OnTouchEvent` 回调里驱动 `rgb_`。

## 7. 待确认 / 后续可调项
- [ ] 实测确认两个电机的正负方向是否需要 `*_INVERT`。
- [ ] 实测确认电位器方向 `*_POT_CW_INC`（软限位是否误触发）。
- [ ] 标定软限位区间 `POT_RANGE_MIN/MAX_PCT` 与电池分压比 `BATT_DIVIDER_RATIO`。
- [ ] 触摸阈值：当前按基线 ×0.8，可按铜片尺寸/灵敏度调 `TouchPad::Calibrate()` 的 ratio。
- [ ] RGB 灯带：按实物在 menuconfig 设 `CONFIG_ITOY_RGB_LED_COUNT`；注意 U13 由 3V3 供电，灯珠多时需确认供电与亮度（用 `SetBrightness` 限流）。
- [ ] （可选）待机随机动作、IMQ 加速度检测拍打互动等，后续按需加。
