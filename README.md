# smartcar_remote — SMART CAR REMOTE (ESP32-S3-LCD-EV-Board-2)

远距离智能遥控器(主驾驶终端)固件。对标需求规格书
[doc/ESP32-S3-LCD-EV-Board v1.5 远距离智能遥控器——LCD UI-UX 产品级需求规格书](doc/ESP32-S3-LCD-EV-Board%20v1.5%20远距离智能遥控器——LCD%20UI-UX%20产品级需求规格书.md),
上行协议与 [`esp32c6_car`](../esp32c6_car)(C6 网关固件)完全对接。

```
ESP32-S3 (本项目)  ──Wi-Fi STA→ C6 softAP ──WebSocket /ws── proto v2
                    (ui/controller/safety)   (c6_car, 不做任何修改)
C6  ──SPI/SF帧──  TC275  ──  车辆
手机 Web           备用控制 / 监视 / OTA / 维护
```

**模块详细设计与完成状态：[`doc/`](doc/00-overview.md)**（每模块一份：架构/接口/时序/完成状态表，架构与格式对齐 esp32c6_car 文档体系）。

## 目录

```
main/            组合根 app_main + app_state（UI State 单一事实源）
  proto/         proto v2 编解码（逐字节复用 c6_car/c6_proto）
  scr_settings   NVS 设置（SSID/token/模式/死区）
  scr_link       Wi-Fi STA → C6 softAP → WebSocket → proto v2 + 配对 + 看门狗
  scr_ctrl       30 Hz DRIVE + STOP/急停 + 失联自动停 + 模式限幅
  ui/            LVGL 9 页面（Boot/Home摇杆/Vehicle/Radio/Diag/Settings/Alert）
test/host/       主机自测（G1：CRC 0x29B1 + 帧回环，make 即跑）
doc/             模块设计文档 + 验证汇总（G1-G4 门）
```

## 硬件

| 项 | 值 |
|---|---|
| 主板 | ESP32-S3-LCD-EV-Board-MB v1.5(ESP32-S3-WROOM-1-N16R16V,16 MB flash + 16 MB octal PSRAM) |
| LCD 子板 | SUB3:4.3" 800×480 RGB(ST7262E43),GT1151 电容触摸 |
| BSP | `espressif/esp32_s3_lcd_ev_board` `BSP_LCD_SUB_BOARD_800_480`,RGB bounce-buffer 模式(与 Wi-Fi 共存防花屏) |
| 工具链 | ESP-IDF v6.1(EIM:`source ~/.espressif/tools/activate_idf_v6.1.sh`) |

## 构建 / 烧录

```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
idf.py set-target esp32s3      # 首次
idf.py build
idf.py -p PORT flash monitor
```

## 链路与协议(与 c6_car 手机控制页同构,spec 95-97)

- Wi-Fi STA 连 C6 softAP(台架默认 `SD-DEV000` / `sddev123456`,NVS 可改)。
- WebSocket `ws://192.168.4.1/ws?token=...`;二进制 = proto v2 帧
  (`AA 55 02 CMD SEQ LEN DATA CRC16-CCITT-FALSE`),编解码源文件
  **原样复用** `c6_car/components/c6_proto/proto_frames.[ch]`(字节级一致)。
- `DRIVE 0x50 {i16 v mm/s, i16 ω deg/s}` @30 Hz,兼作 TC275 心跳;
  满行程 v=600 / ω=300,与手机页 app.js 相同。
- `TELEMETRY 0x41`(38 B LE)50 Hz 广播:速度/电池/里程/故障/链路 RTT。
- 文本面 JSON:`hello{role,ver,tc}`(控制权判定 S3 MASTER / NO CONTROL)、
  `tc{on}`(C6↔TC275 链路)、`ping`/`pong`(S3↔C6 延迟)、`err{e}`(auth)。
- 配对:车侧按键开窗 → `POST /api/pair` → token 存 NVS,重连免按键。

## 模块

```
main/
  app_main.c        组合根: NVS → 状态 → LCD/触摸(BSP) → UI → 链路 → 控制
  app_state.[ch]    Single Source of Truth(spec 98-101): 快照式读取、
                    数据过期判定("--")、事件环形日志(spec 83)、告警状态
  scr_settings.[ch] NVS 设置: SSID/密码/token/模式/死区
  scr_link.[ch]     Wi-Fi STA + WebSocket + JSON 文本面 + 配对 REST +
                    收发/丢包统计 + 静默看门狗(10 s 重连,对齐手机页)
  scr_ctrl.[ch]     控制任务 30 Hz: 摇杆→DRIVE、STOP 锁存、急停 0x32、
                    失联自动停 + 全屏告警(spec 102)、故障/低电告警、模式限幅
  proto/            proto v2 编解码(c6_car 原文件)
  ui/
    ui.[ch]         页面管理 10 Hz 刷新 + P0 开机页 + Toast
    ui_theme.[ch]   深色主题色板/字体层级(spec 51-54)
    ui_joystick.[ch]虚拟摇杆: 死区(可配)、回弹动画、T/S 实时值(spec 13-16)
    ui_home.[ch]    P1 驾驶主页: 状态栏/大速度/摇杆/油门转向/信息行/STOP(spec 70/110)
    ui_pages.[ch]   P2 Vehicle / P3 Radio / P4 Diagnostics / P5 Settings
                    (Control/Radio/Display/About)/ P6 Pairing / Event Log
    ui_alert.[ch]   P9 全屏告警覆盖层: 等级配色、急停 RELEASE、其余 ACK(spec 20-22)
```

## 安全语义

- **STOP**:一次触摸立即发 `DRIVE 0,0` 并锁存;松手不恢复(spec 19)。
- **急停**:长按 STOP ≥1.2 s → `0x32 EMERGENCY_STOP` + 锁存 + 全屏红框,
  必须按 [RELEASE] 明确解除;解除后车辆保持停止,需重新触摸摇杆(spec 63/105)。
- **失联**:遥测超时 600 ms 视为过期;链路断开即发全屏 `RADIO LOST / VEHICLE STOP`
  (DRIVE 心跳随之停发,TC275 心跳看门狗自行停车),恢复后自动清除并记入
  事件日志(spec 82/102)。
- **控制权**:C6 `hello role=ctrl` → S3 MASTER;丢失 → NO CONTROL + 摇杆禁用
  (spec 103/104)。手机是备用端,不抢占(配对窗口语义由 C6/TC275 保证)。

## 主机自测（G1 门）

```bash
cd test/host
make
```

覆盖：CRC check 值 0x29B1（与 c6_car 主机单测同源断言）、DRIVE 帧编解码回环、遥测 38 B 编解码回环、垃圾字节再同步。

## 未做 / 后续(按 spec 分阶段)

- 快速设置抽屉(下滑 Quick Panel,spec 71)、滑动切页(用齿轮按钮替代,
  驾驶中避免误手势,spec 47)、屏幕校准页(P10)、遥控器自身电池(无硬件,
  spec 57)、Radio Test 工程模式(spec 84-85)、主题/动画美化(P6 阶段)。
- RSSI 质量分界(-60/-67/-75/-85 dBm)与 ECO/NORMAL/SPORT 限幅(50/80/100 %)
  均为台架默认值,`main/Kconfig.projbuild` 可调,**须实测标定**(spec 26/33)。

完整缺口索引与真机联调清单见 [doc/07-verification.md](doc/07-verification.md)。
