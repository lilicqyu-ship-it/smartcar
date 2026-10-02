SmartCar
S3-CAM / OV5640 车载视觉系统
详细设计说明书
基于 Freenove ESP32-S3-WROOM CAM 板
替换现有 ESP32-C6 Car 的实施设计

本文件以“可落地实现”为目标，覆盖硬件复用、引脚规划、Camera、Wi-Fi、双 WebSocket、SPI/SF、S3 Remote UI、视觉算法、任务调度、安全、OTA、测试与仓库迁移。

# 1. 文档定位与修订说明
本设计文档是对前一版“C6 Car → Freenove S3-CAM”迁移方案的工程化修订。最大的修正是：摄像头型号以本项目实际硬件为 OV5640，而不是 OV2640；同时重新以 Freenove/Freenove_ESP32_S3_WROOM_Board 仓库的板级资料作为 GPIO 与外围资源真源。

## 1.0 修订记录
- **2026-10-02：SD 存储功能整体裁剪**。本项目 S3-CAM 不配备 MicroSD 卡，录像/抓拍（snapshot/record）、SD 容量上报（`cam_state.sd/rec`）、`camera_record` 组件、`sd_record_task` 与 P3 存储阶段全部移出 V1.0；预览、视觉、驾驶不受影响。SD 相关 GPIO38/39/40 保留为备用脚（不参与任何功能）。Remote 侧对应裁剪见 smartcar_remote 设计文档 R-ADR-09。

## 1.1 关键修正

## 1.2 本版设计原则
- TC275 保持实时运动控制权：编码器闭环、FF + PI + Slew、故障与急停仍在 TC275 侧。
- S3-CAM 承担网络、摄像头、视觉算法、视觉辅助驾驶、任务/目标层决策。
- S3 Remote 不再只是遥控器，而是 Human + Camera + Vision + Vehicle Console。
- 现有 proto v2 / SF / SPI 协议尽可能原样保留，减少 TC275 侧改动。
- 控制通道、视频通道、视觉结果通道分别处理，避免大块 JPEG 数据拖垮小控制帧。

# 2. 现有 smartcar 基线分析
当前 smartcar 已经具备一个很适合作为迁移基础的三层软件结构：esp32c6_car 负责网络与 WebSocket/SPI 桥接；smartcar_remote 负责 LVGL、摇杆、链路管理和 OTA；tc275_car 负责车体实时控制。共享 contracts/link/proto_frames.[ch] 定义了 v2 二进制协议。

## 2.1 当前 C6 的职责

## 2.2 当前 Remote 的可复用能力

## 2.3 控制协议基线

```text
PROTO v2
AA 55 | VER | CMD | SEQ | LEN | DATA[<=64] | CRC16-CCITT-FALSE

PROTO_CMD_DRIVE = 0x50
DATA = int16 v(mm/s) + int16 omega(deg/s)
现有 Remote 默认满行程：v=600 mm/s，omega=300 deg/s

PROTO_CMD_TELEMETRY = 0x41
Telemetry = 38 bytes LE

```
这一部分建议继续作为“车辆实时链路”的真源，不把摄像头 JPEG 直接塞进 64-byte proto frame。

# 3. Freenove ESP32-S3-WROOM CAM 硬件基线
Freenove 提供的仓库明确标注该项目适用于 FNK0085；产品资料列出 ESP32-S3 双核最高 240 MHz、8/16 MB Flash、8 MB PSRAM、2.4 GHz Wi-Fi、Bluetooth 5 LE、USB-OTG、板载 Camera 与 MicroSD。注意：本项目实际不配备 MicroSD 卡，SD 存储功能已整体移出 V1.0（见 1.0 修订记录）。

## 3.1 板级 Camera 引脚真源
注意：Freenove 官方 camera_pins.h 当前示例使用 CAMERA_MODEL_ESP32S3_EYE 命名来描述这组板级 Camera 引脚；该命名本身不能证明实际传感器一定是 OV5640。你的项目把实际传感器定义为 OV5640，因此应在驱动层开启 OV5640 支持，并通过 sensor ID 做上电自检。

# 4. OV5640 摄像头设计
OV5640 是 5MP 彩色 CMOS 图像传感器，原始有效阵列为 2592×1944；支持 DVP 并行输出、SCCB 控制以及 JPEG/RAW/RGB565/YUV 等输出能力。对于 ESP32-S3 项目，本设计只把“2592×1944”视为传感器能力上限，不把它作为实时视频工作分辨率。

## 4.1 驱动策略
- 优先使用 Espressif esp32-camera 的 OV5640 支持，而不是从零编写寄存器初始化表。
- 开启 CONFIG_OV5640_SUPPORT；只有手里的 OV5640 模块带 VCM/AF 能力时才打开 CONFIG_CAMERA_AF_SUPPORT。
- 初始化顺序固定为：GPIO/时钟 → SCCB → sensor detect → sensor init → pixel format → frame size → JPEG/质量 → frame buffers。
- 所有 framebuffer 放 PSRAM；控制结构、队列、锁和安全状态保留在内部 SRAM。

## 4.2 Camera profile

# 5. S3-CAM 总体软件架构

```text
                    ┌────────────────────────────┐
                    │          S3 Remote           │
                    │  LVGL / Joystick / Camera UI │
                    └─────────────┬────────────────┘
                                  Wi-Fi
                                    │
                   ┌────────────────▼────────────────┐
                   │            S3-CAM               │
                   │                                  │
                   │ Core 0: Control / Net / SPI     │
                   │ Core 1: Camera / Vision         │
                   │                                  │
                   │  Camera -> Frame Hub ->         │
                   │    ├─ Camera WS -> Remote       │
                   │    └─ Vision -> Vision Event    │
                   └────────────────┬─────────────────┘
                                    │ SPI/SF
                                    ▼
                           ┌─────────────────┐
                           │      TC275      │
                           │ FF+PI+Encoder   │
                           │ Slew + Safety   │
                           └────────┬────────┘
                                    │
                              Motor Driver
```

## 5.1 三个平面

# 6. SPI/SF 车体链路迁移设计
现有 C6 使用 SPI half-duplex slave + spi_slave_hd，TC275 为 master，5 MHz 作为当前量产基线，500 ms 主机静默判 DOWN。该链路本身与摄像头没有直接关系，因此应完整保留，只把实现组件从 c6_link 重命名/抽象成 vehicle_link。

## 6.1 S3-CAM SPI GPIO 规划
由于 Camera 占用了 GPIO4/5/6/7/8/9/10/11/12/13/15/16/17/18，SD 占 GPIO38/39/40，PSRAM 占 35/36/37，USB 占 19/20，UART0 占 43/44，因此不能照搬 C6 的 18/19/20/23/21。
这是“本设计候选映射”，最终 PCB 接线前应依据手中板子的丝印、排针位置和 JTAG/调试需求做一次台架确认。尤其 GPIO42 与 JTAG 复用关系必须在硬件 bring-up 记录中注明。

## 6.2 统一配置命名

```text
CONFIG_VEHICLE_LINK_SPI_HOST=SPI2_HOST
CONFIG_VEHICLE_LINK_SPI_SCLK_GPIO=14
CONFIG_VEHICLE_LINK_SPI_MOSI_GPIO=21
CONFIG_VEHICLE_LINK_SPI_MISO_GPIO=47
CONFIG_VEHICLE_LINK_SPI_CS_GPIO=42
CONFIG_VEHICLE_LINK_SPI_IRQ_GPIO=1
CONFIG_VEHICLE_LINK_SPI_CLOCK_HZ=5000000

```
不建议继续使用 CONFIG_C6_* 命名，因为 S3-CAM 已经成为第二代车载网络节点，未来再切到别的 MCU 时，抽象层会更干净。

# 7. Wi-Fi 与网络架构
建议保持当前“车载 SoftAP”形态：S3-CAM 作为 AP，S3 Remote 作为 STA；手机/浏览器作为 spectator。这样不需要外部路由器即可完成完整控制链路。

## 7.1 会话角色
保持当前 token + role gate + SEQ gate 设计。Camera WS 同样要带 session identity，但默认建议“读视频不需要独立 token，改变 Camera 模式需要 CTRL”这一规则；这样手机看画面很方便，而关键配置仍受控。

# 8. 双 WebSocket 设计：Control WS + Camera WS
这是整个替换设计最关键的通信分离。当前 /ws 已经承载二进制 proto 与文本控制协商，继续使用；不要把 JPEG 作为 proto frame，也不要让 Camera WS 与 Control WS 共享一个无限发送队列。

## 8.1 Control WS

```text
Endpoint: ws://192.168.4.1/ws

Binary:
  proto v2：DRIVE / TELEMETRY / DIAG / PAIR / OTA ...

Text:
  S3-CAM -> Remote: hello / pong / tc / vision / cam_state / err
  Remote -> S3-CAM: ping / tcver / cam_cmd

```

## 8.2 Camera WS

```text
Endpoint: ws://192.168.4.1:81/ws/camera   （实现落 :81 独立 httpd，偏差记录见 doc/21 §3.3）

Text control:
  hello            (gw -> remote，连接即发，字段见表 17)
  subscribe        (remote -> gw，开推送)
  profile          (remote -> gw，REMOTE_PREVIEW / WEB_PREVIEW)
  pause            (remote -> gw，停推送、保连接)
  ping / pong      (双向：remote 暂停期每 4 s 一发保活；gw 回 {"op":"pong","ts":..})
  cam_state        (gw -> remote，1 Hz：fps/seq/drop/subscribed/w/h)

Binary data:
  CAM_FRAME_HEADER + JPEG payload

```

> **2026-10-02 实现状态**：本节文本面已在 `s3_camera/camera_stream.c` 落地
> （订阅门、profile 寄存器级切换、ping→pong、hello/cam_state 回读）；
> 消息名与解析常量以 `contracts/vision/vision.h` 为准，帧头以
> `contracts/camera/cam_frame.h` 为准，两侧副本经 `scripts/check-contracts.sh` 校验。
> 细节见 doc/19 §5.2/§5.4。

## 8.3 Camera 帧头
建议所有 header 字段采用 little-endian；Remote 先检查 JPEG 长度和 sequence，再进入解码队列。发现旧帧积压时直接丢弃中间帧，只保留“最新完整帧”。

# 9. Camera / Frame Hub / Vision Pipeline

```text
OV5640 DVP
   │
   ▼
esp_camera_fb_get()
   │
   ▼
Frame Hub（唯一 framebuffer owner）
   ├───────────────────┐
   ▼                   ▼
JPEG Encoder       Vision Preprocess
   │                   │
   ▼                   ▼
Camera WS           Vision Task
（Snapshot/Record/SD Task 分支已随 SD 硬件裁剪移出，见修订记录）

```
Frame Hub 负责帧所有权与引用计数。任何任务都不允许在未获取 frame lease 的情况下直接持有 camera framebuffer 指针。这样可以避免 Camera 一边回收 buffer，一边被 WS/vision 继续使用造成野指针。

## 9.1 推荐的数据流

# 10. 视觉功能路线与边界

## 10.1 第一阶段必须优先实现的视觉能力
- Line detection：为后续 Line Follow 打基础。
- Color blob：非常低成本，可用于颜色路标/停车区。
- QR code：最适合任务路线/动作编排。
- Motion/ROI：用于事件录制和简单目标出现检测。

## 10.2 视觉辅助驾驶模型

```text
e_x = (x_line - x_center) / x_center
e_theta = line_angle

omega_final = clamp(omega_remote + Kx * e_x + Ktheta * e_theta, -Wmax, +Wmax)
v_final     = v_remote * confidence_gate

confidence_gate = 1.0          confidence >= C_high
                 = f(conf)      C_low < confidence < C_high
                 = 0            confidence <= C_low

```
该控制只产生“高层 v/ω 指令”，最终 PWM、编码器闭环、限速、故障和急停仍由 TC275 决定。

# 11. Vision JSON 协议
视觉结果数据量小，不需要进入 64-byte proto v2。建议作为 Control WS 的 text JSON 消息。

```text
{
  "t":"vision",
  "ver":1,
  "mode":"line",
  "valid":true,
  "confidence":0.92,
  "cx":163,
  "error":-0.018,
  "angle":-4.2,
  "ts":123456
}

```

```text
{
  "t":"vision",
  "ver":1,
  "mode":"object",
  "count":1,
  "objects":[{"class":1,"score":0.91,"x":55,"y":30,"w":80,"h":120}]
}

```
Remote 只保留最新 vision snapshot，不建立无限历史缓存。需要事件记录时，再把事件摘要落到低频日志（SD 落盘路径已随硬件裁剪移出）。

# 12. Camera 控制协议

```text
Remote -> S3-CAM
{"t":"cam_cmd","op":"profile","name":"REMOTE_PREVIEW"}
{"t":"vision_cmd","mode":"line","enable":true}

```

# 13. S3 Remote 详细交互设计
S3 Remote 的升级目标不是“增加一个 Camera 页面”，而是把 Camera/vision 纳入已有状态机、告警、Control Owner、Telemetry、OTA 的统一 UX。

## 13.1 页面结构

## 13.2 Camera 页面布局

```text
┌──────────────────────────┐
│ CAMERA   ● LIVE   9.8 FPS│
├──────────────────────────┤
│                          │
│       320 x 240          │
│        VIDEO             │
│                          │
├──────────────────────────┤
│ Profile  REMOTE_PREVIEW  │
│ Vision   LINE / OFF      │
│ Drop     2 / 1200        │
│                          │
│ [VISION]                 │
└──────────────────────────┘

```

## 13.3 Vision 页面

```text
┌──────────────────────────┐
│ VISION   LINE       92%  │
├──────────────────────────┤
│     camera preview       │
│       ───────            │
│          ╲               │
│           ╲              │
├──────────────────────────┤
│ X ERROR       -0.018     │
│ ANGLE         -4.2°      │
│ CONFIDENCE    92%       │
│ ASSIST        ON         │
│                          │
│ [MANUAL] [ASSIST] [AUTO]│
└──────────────────────────┘

```

## 13.4 交互规则
- 进入 Camera 页面自动订阅视频，不自动开启 Vision。
- 进入 Vision 页面才提高视觉结果推送频率。
- REC/PHOTO/Assist/Auto 均需要 CTRL 角色；spectator 只读。
- Control WS 掉线时 Remote 立即触发现有 radio lost 逻辑，并发送 DRIVE=0；Camera WS 掉线不触发车辆急停，但页面显示 CAMERA OFFLINE。
- Camera 没有图像时，Vision 结果必须显示 stale/invalid，不允许沿用上一帧结果继续控制。

# 14. Remote ↔ S3-CAM 交互时序

```text
启动：
S3 Remote              S3-CAM
   │                     │
   │ Wi-Fi association   │
   ├────────────────────>│
   │                     │
   │ WS /ws              │
   ├────────────────────>│
   │      hello          │
   │<────────────────────┤
   │                     │
   │ WS /ws/camera       │
   ├────────────────────>│
   │   cam_hello         │
   │<────────────────────┤
   │ subscribe           │
   ├────────────────────>│
   │ JPEG frame #1       │
   │<────────────────────┤
   │ JPEG frame #2       │
   │<────────────────────┤

```

```text
驾驶 + 视觉辅助：
Remote                   S3-CAM                    TC275
  │                        │                         │
  │ DRIVE(v,w)             │                         │
  ├───────────────────────>│                         │
  │                        │ vision correction      │
  │                        │                         │
  │                        │  DRIVE(v,w_corr)       │
  │                        ├────────────────────────>│
  │                        │                         │ FF+PI+Encoder+Slew
  │                        │                         │
  │ telemetry              │<────────────────────────
  │<───────────────────────│                         │

```

# 15. FreeRTOS 双核任务设计
目标是把网络和车辆控制服务与 Camera/Vision 分离，同时禁止视频/AI任务抢占关键控制资源。

## 15.1 控制与视频的资源隔离
- 控制 WS tx queue 与 Camera WS tx queue 必须分离。
- Camera JPEG 不经过 vehicle_link。
- Vision 任务在帧处理超时时，丢当前视觉样本并记录 drop，不阻塞下一次 Camera capture。
- 任何 Camera/vision 任务都不得拥有 TC275 SPI mutex。

# 16. 内存与带宽预算

## 16.1 网络预算示例

```text
Remote preview：
  320x240 JPEG，平均 25 KB/frame，10 fps
  ≈ 250 KB/s ≈ 2.0 Mbps（仅有效载荷）

Web preview：
  640x480 JPEG，平均 60 KB/frame，8 fps
  ≈ 480 KB/s ≈ 3.8 Mbps

因此：
  1) Remote 默认走 320x240 / 8-10 fps
  2) 手机/Web 默认 640x480 / 5-8 fps
  3) 多客户端时按订阅者分别限速
  4) 控制与 telemetry 永远拥有更高 QoS

```

# 17. 安全与故障处理
关键规则：Camera 是非安全传感器，不能成为“急停唯一依据”；任何依赖视觉的自动/辅助功能都必须存在 confidence 和 freshness gate。

# 18. OTA / 日志 / 诊断设计

## 18.1 OTA 分层

## 18.2 /api/diag 建议字段

```text
{
  "ver":"s3cam-v1.0.0",
  "heap_min":...,
  "psram_free":...,
  "link":{"up":true,"clock":5000000,"rtt":2},
  "camera":{"sensor":"OV5640","up":true,"fps":9.8,"drop":2,"w":320,"h":240},
  "vision":{"mode":"line","valid":true,"runtime_ms":18,"drop":1},
  "sd":{"mounted":true,"free_mb":782},
  "uptime_s":...
}

```

# 19. 仓库迁移设计
推荐新增 esp32s3_cam_car，而不是直接删除 esp32c6_car。先让 S3-CAM 与 C6 共存一段时间，等 TC275 HIL 回归结束后再决定是否归档 C6 工程。

## 19.1 新目录建议

```text
esp32s3_cam_car/
├── main/
│   ├── app_main.c
│   ├── app_state.c/.h
│   ├── camera_state.c/.h
│   └── vision_state.c/.h
├── components/
│   ├── vehicle_link/
│   ├── vehicle_bridge/
│   ├── vehicle_net/
│   ├── vehicle_pair/
│   ├── vehicle_ota/
│   ├── camera/
│   ├── camera_ws/
│   ├── vision/
│   ├── http/
│   └── led/
└── assets_src/
    ├── index.html
    ├── camera.html
    ├── vision.html
    └── ...

```

# 20. CMake / Kconfig 设计建议

## 20.1 Component 划分

```text
REQUIRES
  esp_driver_gpio
  esp_driver_spi
  esp_driver_i2c
  esp_driver_ledc
  esp_timer
  esp_wifi
  esp_http_server
  esp_http_client
  esp32-camera
  nvs_flash
  fatfs / sdmmc

```

## 20.2 关键 Kconfig

```text
S3CAM_CAMERA_OV5640=y
S3CAM_CAMERA_WIDTH=320
S3CAM_CAMERA_HEIGHT=240
S3CAM_CAMERA_FPS=10
S3CAM_CAMERA_JPEG_QUALITY=55
S3CAM_CAMERA_FB_COUNT=2
S3CAM_CAMERA_AF=n

S3CAM_CAMERA_WS=y
S3CAM_CAMERA_WS_MAX_CLIENTS=2
S3CAM_CAMERA_WS_MAX_FPS=10

S3CAM_VISION=y
S3CAM_VISION_LINE=y
S3CAM_VISION_COLOR=y
S3CAM_VISION_QR=y
S3CAM_VISION_OBJECT=n

```

# 21. 分阶段实施计划

# 22. Bring-up 测试清单

## 22.1 Camera

## 22.2 SPI/TC275

## 22.3 Remote

# 23. 关键设计决策记录（ADR）

# 24. 代码落地时的第一批文件

# 25. 参考资料与证据链
以下资料用于本设计中的硬件、传感器、软件驱动和现有工程事实核对。
资料核验说明：Freenove 当前公开仓库的 Camera 示例采用 CAMERA_MODEL_ESP32S3_EYE 命名，并给出了本板 DVP 摄像头的具体 GPIO；该仓库当前 datasheet 目录可见 OV2640/OV3660 资料，但并不以文字声明“本板摄像头必然为 OV5640”。因此本设计把“OV5640 是你手上实物模块”作为项目硬件事实，并以启动时 sensor ID 检测 + Espressif OV5640 driver 作为软件确认手段。

# 26. 最终目标状态

```text
                    SmartCar Vision Platform

         ┌─────────────────────────────────────┐
         │              S3 Remote               │
         │ Human control / LVGL / Camera UI     │
         │ Vision monitor / Mission / OTA       │
         └──────────────────┬──────────────────┘
                            │ Wi-Fi
                            ▼
         ┌─────────────────────────────────────┐
         │               S3-CAM                │
         │ OV5640 / Video / Vision / Network   │
         │ Line / Color / QR / Object / Assist │
         │ HTTP / Control WS / Camera WS       │
         └──────────────────┬──────────────────┘
                            │ SPI/SF
                            ▼
         ┌─────────────────────────────────────┐
         │                TC275                │
         │ Encoder / FF + PI / Slew / Safety  │
         │ Final actuator authority            │
         └─────────────────────────────────────┘

```
完成这一版后，系统的定位就从“带网络控制的智能车”升级为“具备实时运动控制、车载视觉、视觉辅助驾驶和可扩展 AI 的 SmartCar Vision Platform”。其中最重要的工程边界仍然不变：人类意图由 Remote 给出，视觉理解由 S3-CAM 完成，最终的物理执行和安全裁决由 TC275 完成。
--- 文档结束 ---

### Table 1
| 文档版本 | V1.0 |
|---|---|
| 日期 | 2026-10-02 |
| 摄像头 | OV5640，5MP，DVP 8-bit（按实物模块确认） |
| 主控板 | Freenove ESP32-S3-WROOM CAM / FNK0085 系列 |
| 现有基线 | lilicqyu-ship-it/smartcar main 分支：esp32c6_car + smartcar_remote + tc275_car |
| 设计目标 | S3-CAM = 车载视觉/网络主机；TC275 = 实时运动控制；S3 Remote = 人机交互与视觉终端 |

### Table 2
| 项目 | 此前口径 | 本版工程口径 |
|---|---|---|
| 摄像头 | 按 OV2640 思路描述 | OV5640，5MP；优先使用 DVP 8-bit + SCCB |
| 板卡 | 泛化 S3-CAM | Freenove ESP32-S3-WROOM CAM / FNK0085 系列 |
| Camera GPIO | 引用泛化 CAM 示例 | 采用 Freenove 仓库中 ESP32S3_EYE 对应的板级 Camera 复用：GPIO4/5/6/7/8/9/10/11/12/13/15/16/17/18 |
| SD | 不启用（V1.0 裁剪） | 板载 SD 1-bit：GPIO38/39/40 保留为备用脚；无卡即无录像/抓拍 |
| USB | 仅作为烧录口 | GPIO19/20 为原生 USB；GPIO43/44 为 USB-UART，必须保留 |
| SPI | 沿用 C6 默认 GPIO | 必须重新规划，避免 Camera/SD/USB/PSRAM 冲突 |
| 控制与视频 | 存在混用风险 | Control Plane 与 Video Plane 分离；视频拥塞不得影响车辆控制 |

### Table 3
| 模块 | 现有职责 | 迁移到 S3-CAM 后 |
|---|---|---|
| app_main | 启动编排、factory、link、bridge、net、http、OTA | 保留启动编排思想，新增 camera/vision 启动 |
| c6_link | SPI Slave / SF 链路 / 500ms health | 改名 vehicle_link，SPI Slave / SF 保留 |
| c6_bridge | v2 ↔ SPI/SF、遥测广播、OTA 中继 | 保留并扩展 camera/vision event relay |
| c6_http | HTTP + /ws + /api/health + /api/diag + OTA | 保留控制 API；新增 /ws/camera 与 camera API |
| c6_net | SoftAP、captive DNS、mDNS | 保留 SoftAP 作为车载 WLAN |
| c6_pair | 配对、token、控制角色 | 直接迁移为 vehicle_pair |
| c6_ota | C6 自升级 + TC275 中继 | 调整为 S3-CAM 自升级 + TC275 中继 |
| ADXL345/LED | 诊断/状态指示 | 可继续保留，但 GPIO 要重新规划 |

### Table 4
| 现有能力 | 当前代码落点 | S3-CAM 场景下的用法 |
|---|---|---|
| Single Source of Truth | smartcar_remote/main/app_state.* | 增加 camera/vision/mission 状态字段（record 字段已随 SD 裁剪移出） |
| Joystick / ctrl task | scr_ctrl.c | 保持原样，目标地址由 C6 概念改成 S3-CAM |
| WebSocket client | scr_link.c | 保留 control WS；新增 camera WS 客户端 |
| Telemetry | proto_telemetry_t，38B | 继续来自 TC275；新增 vision telemetry 独立结构 |
| Pair / token | scr_link + scr_svc | 继续使用 S3-CAM 端会话管理 |
| OTA staging | scr_svc.c | S3-CAM OTA 与 TC275 OTA 均可继续复用 |

### Table 5
| 资源 | 本设计口径 | 设计约束 |
|---|---|---|
| CPU | ESP32-S3 双核，最高 240 MHz | Core 0 负责网络/控制服务，Core 1 负责 Camera/Vision |
| PSRAM | 8 MB | 优先给 frame buffer、JPEG、vision scratch；不要把控制队列放 PSRAM |
| Flash | 8 MB 或 16 MB | 推荐 16 MB 作为量产/开发基线；允许按手头板型切换 |
| Camera | OV5640 实物模块 | 使用 DVP 8-bit 采集；传感器控制走 SCCB/I2C 兼容接口 |
| SD | 板载 MicroSD 插槽（本项目不使用） | V1.0 无录像/抓拍；CMD=38, CLK=39, D0=40 保留备用 |
| Native USB | GPIO19/20 | 保留用于 CDC/OTG，不拿来做业务 SPI |
| USB-UART | GPIO43/44 | 保留烧录与日志，避免运行期占用 |
| PSRAM pins | GPIO35/36/37 | 不得作为普通 IO 使用 |
| Boot/strap/JTAG | GPIO0、3、45、46 等需谨慎 | 不作为第一选择的 SPI/控制线 |

### Table 6
| Camera 信号 | GPIO | 说明 |
|---|---|---|
| CAM_SIOD / SCCB SDA | 4 | Camera 控制 |
| CAM_SIOC / SCCB SCL | 5 | Camera 控制 |
| CAM_VSYNC | 6 | 帧同步 |
| CAM_HREF | 7 | 行有效 |
| CAM_Y2 / D0 | 11 | 8-bit DVP 数据 |
| CAM_Y3 / D1 | 9 | 8-bit DVP 数据 |
| CAM_Y4 / D2 | 8 | 8-bit DVP 数据 |
| CAM_Y5 / D3 | 10 | 8-bit DVP 数据 |
| CAM_Y6 / D4 | 12 | 8-bit DVP 数据 |
| CAM_Y7 / D5 | 18 | 8-bit DVP 数据 |
| CAM_Y8 / D6 | 17 | 8-bit DVP 数据 |
| CAM_Y9 / D7 | 16 | 8-bit DVP 数据 |
| CAM_PCLK | 13 | 像素时钟 |
| CAM_XCLK | 15 | 输入时钟 |
| PWDN/RESET | NC / -1 | 以实物 OV5640 模块的 FPC/板级电路最终确认 |

### Table 7
| 参数 | 设计值 | 工程意义 |
|---|---|---|
| Sensor | OV5640 | 必须在启动时读 ID 并记录 |
| Max image | 2592×1944 | 传感器能力上限；V1.0 无抓拍落盘，不作为持续视频 |
| Interface | DVP 8-bit + SCCB | 与 Freenove 板级 Camera GPIO 兼容 |
| Preview profile | 320×240 / JPEG | S3 Remote 预览，目标 8-10 fps |
| Web profile | 640×480 / JPEG | 手机/Web 预览，目标 5-10 fps |
| AI profile | 160×120 或 320×240，YUV/RGB | 低分辨率视觉推理，降低 CPU/内存 |
| Snapshot | UXGA/更高按驱动稳定性选择 | V1.0 裁剪（无 SD 落盘），保留为传感器模式规划 |
| AF | 可选 | OV5640 AF 模块才开启；固定焦模块关闭 AF |
| Image controls | AEC/AWB/增益/曝光/镜像/翻转/质量 | 在 Camera service 统一管理 |

### Table 8
| Profile | 分辨率 | 格式 | 目标帧率 | 主要消费者 |
|---|---|---|---|---|
| REMOTE_PREVIEW | 320×240 | JPEG | 8-10 fps | S3 Remote |
| WEB_PREVIEW | 640×480 | JPEG | 5-10 fps | 手机/浏览器 |
| VISION | 160×120 或 320×240 | YUV/RGB565 | 10-20 fps视算法 | vision_task |
（SNAPSHOT/EVENT profile 已随 SD 硬件裁剪移出 V1.0，见修订记录）

### Table 9
| 平面 | 方向 | 内容 | 实时性要求 |
|---|---|---|---|
| Control Plane | S3 Remote ↔ S3-CAM ↔ TC275 | Drive / stop / emergency / telemetry / pair | 高；控制必须优先 |
| Video Plane | S3-CAM ↔ S3 Remote / Web | JPEG frame | 可丢帧；低延迟优先于无损 |
| Vision Plane | Camera → S3-CAM → Remote/TC275 | line/object/QR/assist result | 中高；结果小、频率低于视频 |

### Table 10
| 项目 | 现有 C6 | S3-CAM 本版 |
|---|---|---|
| Master | TC275 QSPI3 | 不变 |
| Slave peripheral | ESP32-C6 SPI2 | ESP32-S3 SPI2，接口能力需以所选 IDF 版本实机确认 |
| Clock baseline | 5 MHz | 5 MHz 首版；再按 G5 逐档压测 |
| Segment | <=512 B | 不变 |
| RX DMA | 2×512 B | 不变 |
| TX DMA | 1×512 B | 不变 |
| Watchdog | 500 ms host activity | 不变 |
| SF CRC/SEQ | 不变 | 不变 |
| v2 codec | contracts/link/proto_frames.[ch] | 不变 |

### Table 11
| 信号 | 建议 GPIO | 选择理由 | 风险/备注 |
|---|---|---|---|
| SCLK | GPIO14 | 板上明确标出为普通 GPIO/ADC，未被 Camera/SD/USB 占用 | 优先候选 |
| MOSI | GPIO21 | 板上可用，且不是 Camera/SD/USB 专用脚 | 优先候选 |
| MISO | GPIO47 | 板上可用，避免 35-40/43-46 | 优先候选 |
| CS | GPIO42 | 板上引出，但为 MTMS/JTAG 关联脚 | 量产可用；若调试依赖 JTAG，准备备用 CS |
| IRQ | GPIO1 | 板上引出，非 Camera/SD/USB 专用脚 | 优先候选；上升沿/电平语义保持与现有 C6 一致 |

### Table 12
| 项目 | 设计值 |
|---|---|
| AP IP | 192.168.4.1 |
| mDNS | mycar.local |
| 最大 STA | 4，首版保持与 C6 一致 |
| AP channel | 出厂默认 6，可配置 |
| Web server | 80 |
| Control WS | /ws |
| Camera WS | /ws/camera |
| Camera HTTP snapshot | /api/camera/snapshot（内存即时返回，不落盘；与已裁剪的 SD snapshot 功能无关） |
| Camera config | /api/camera/config |
| Vision status | /api/vision |

### Table 13
| 角色 | 权限 |
|---|---|
| CTRL | Drive/Stop/E-Stop/Camera control/Vision mode/OTA |
| SPECTATOR | Telemetry/Camera/Vision read-only |
| NONE | 未握手或被踢下线 |

### Table 14
| 字段 | 长度 | 说明 |
|---|---|---|
| MAGIC | 2B | 0xCA 0x56 |
| VERSION | 1B | 0x01 |
| FLAGS | 1B | keyframe / event / reserved |
| SEQ | 4B | 单调帧号 |
| TIMESTAMP_MS | 4B | S3-CAM uptime |
| WIDTH | 2B | 图像宽 |
| HEIGHT | 2B | 图像高 |
| JPEG_LEN | 4B | payload长度 |
| PAYLOAD | N | JPEG bytes |

### Table 15
| 环节 | 设计 |
|---|---|
| Capture | camera_task 固定周期抓帧 |
| Buffer count | 起步 2 帧；有余量再扩到 3 帧 |
| Remote | 取最新 JPEG；不等待旧帧 |
| Vision | 独立低频/降分辨率输入 |
| Backpressure | 视频消费者满 -> 丢旧帧；Vision 满 -> 丢旧样本；Control 永不被这些队列阻塞 |

### Table 16
| 功能 | S3-CAM承担 | Remote显示/交互 | 是否影响TC275实时环 |
|---|---|---|---|
| 实时预览 | Camera + JPEG + WS | Camera 页面 | 否 |
| 颜色检测 | ROI + HSV/阈值 | Color tag/置信度 | 可选 |
| 线/道路检测 | ROI + edge + line fit | Line overlay | 可选辅助驾驶 |
| QR | 解码 | 显示 ID / mission | 可驱动任务层 |
| 目标检测 | 轻量模型/ESP-DL | 框选/类别 | 仅通过高层速度指令影响 |
| 人脸/人体 | ESP-DL | 检测结果 | 默认不直接闭环 |
| 视觉辅助驾驶 | 视觉误差→修正 omega | ASSIST ON/OFF | 通过 v/ω 指令进入 TC275 |
| 全自动驾驶 | 任务层规划 | START/STOP | 不在 V1.0 直接实现 |

### Table 17
| 消息 | 方向 | 示例 | 权限 |
|---|---|---|---|
| cam_hello | S3-CAM→Remote | sensor/profile/fps/w/h（`sd` 字段随 SD 裁剪取消，见 1.0 修订记录） | 全部 |
| cam_cmd start | Remote→S3-CAM | start preview | CTRL |
| cam_cmd stop | Remote→S3-CAM | stop preview | CTRL |
| cam_cmd profile | Remote→S3-CAM | REMOTE_PREVIEW | CTRL |
| cam_state | S3-CAM→Remote | fps/seq/drop | 全部 |
| vision_mode | Remote→S3-CAM | line/qr/object | CTRL |
| vision | S3-CAM→Remote | result snapshot | 全部 |

### Table 18
| 页面 | 核心内容 |
|---|---|
| HOME | 系统总览：连接、TC275、电池、当前速度、Camera 状态 |
| DRIVE | 双向摇杆/停止/急停/驾驶模式 |
| CAMERA | 实时画面、FPS、分辨率、PHOTO、REC、镜像/翻转 |
| VISION | Line/Color/QR/Object，置信度与辅助驾驶开关 |
| VEHICLE | 左右轮目标/实测速度、故障码、里程、电池 |
| RADIO | RSSI、信道、S3↔S3-CAM 延迟、丢包 |
| DIAGNOSTICS | Camera frame drop、heap、PSRAM、SPI health、Vision runtime |
| FIRMWARE | S3-CAM/TC275 OTA |

### Table 19
| 任务 | Core | 优先级建议 | 周期/触发 | 职责 |
|---|---|---|---|---|
| safety_watch | 0/1 | 最高 | 事件+50ms | 急停、断链、故障门控 |
| vehicle_link_task | 0 | 12 | 事件/100ms fallback | SPI/SF RX/TX、健康监测 |
| control_ws_task | 0 | 8 | 事件 | WS control RX/TX |
| vehicle_bridge_task | 0 | 10 | 20ms/事件 | v2↔vehicle_link、telemetry |
| camera_capture_task | 1 | 9 | 10-30Hz | OV5640 capture |
| jpeg_task | 1 | 7 | frame queue | JPEG encode/prepare |
| vision_task | 1 | 6 | 5-20Hz | line/color/QR/object |
| camera_ws_tx | 0 | 5 | frame ready | 只发最新完整帧 |
| diag_task | 0 | 2 | 1-2Hz | 状态统计 |

### Table 20
| 资源 | 估算 | 建议 |
|---|---|---|
| 320×240 RGB565 | 150 KB/frame左右 | 仅用于 vision/preview 辅助，不长期复制 |
| 320×240 JPEG | 约 10-40 KB/frame，视场景/质量而变 | Remote 预览优先 |
| 640×480 JPEG | 约 20-100+ KB/frame | Web/手机按需 |
| PSRAM frame buffers | 2×~150 KB起步 | 放 PSRAM |
| Vision scratch | 100-500 KB，视算法 | 放 PSRAM |
| Control queues | <50 KB | 优先 internal RAM |
（5M snapshot 内存项已随 SD 裁剪移出）

### Table 21
| 故障 | S3-CAM动作 | S3 Remote动作 | TC275动作 |
|---|---|---|---|
| Control WS断开 | 清控制 owner；发送 zero/安全事件 | 现有 RADIO LOST overlay | 继续执行其安全超时逻辑 |
| Camera WS断开 | 仅停 video tx；保留 Camera/Vision | CAMERA OFFLINE | 不直接动作 |
| SPI LINK DOWN | bridge 标记 TC down；停止向 TC 发新高层指令 | VEHICLE OFFLINE / telemetry stale | 按现有链路 watchdog/安全策略处理 |
| Vision stale | vision_valid=false；禁止辅助驾驶输出 | 显示 STALE | 不接受旧视觉指令 |
| OOM/PSRAM不足 | 降低 profile / drop frame / stop AI | 显示 DEGRADED | 保持已有控制链路 |
| E-STOP | 本地立即切断高层命令并发送 emergency stop | 立即显示 critical overlay | TC275 最终执行 emergency stop |

### Table 22
| 对象 | 方式 |
|---|---|
| S3-CAM firmware | 自身双分区 OTA（沿用 c6_ota 设计思想） |
| TC275 App/SBL | S3 Remote staging → S3-CAM → SPI/SF OTA relay |
| Camera/vision config | 不单独做固件 OTA；NVS 配置即可 |
| Web assets | 随 S3-CAM firmware 同版本发布，避免协议漂移 |

### Table 23
| 现有路径 | 迁移动作 | 目标路径 |
|---|---|---|
| esp32c6_car/main/app_main.c | 重构启动编排 | esp32s3_cam_car/main/app_main.c |
| components/c6_link | 重命名/抽象 | components/vehicle_link |
| components/c6_bridge | 保留逻辑、扩展 vision | components/vehicle_bridge |
| components/c6_http | 保留/拆 camera WS | components/http + components/camera_http |
| components/c6_net | 大部分原样迁移 | components/vehicle_net |
| components/c6_pair | 重命名 | components/vehicle_pair |
| components/c6_ota | 重命名 | components/vehicle_ota |
| assets_src | 扩展 Camera/Vision UI | esp32s3_cam_car/assets_src |
| contracts/link | 不动 | contracts/link |
| contracts/camera | 新建 | camera control/frame schema |
| contracts/vision | 新建 | vision schema |

### Table 24
| 阶段 | 目标 | 主要交付 | 验收 |
|---|---|---|---|
| P0 | 硬件 bring-up | OV5640 ID、Camera preview、USB、SPI GPIO确认 | 串口/示波器/逻辑分析仪通过 |
| P1 | C6 等价替换 | SoftAP + /ws + pair + TC275 SPI + telemetry + drive | 现有 Remote 不改即可控制车 |
| P2 | Camera Plane | /ws/camera + 320x240 JPEG + Remote Camera 页 | 10 fps附近稳定预览，控制不受影响 |
| P3 | ~~存储~~ **已取消**（2026-10 SD 硬件裁剪，见修订记录） | — | — |
| P4 | Vision | line/color/QR + vision JSON | 识别结果在 Remote 正确展示 |
| P5 | Assist | 视觉辅助驾驶 | MANUAL/ASSIST 切换和失效保护通过 |
| P6 | AI | ESP-DL/轻量目标检测 | AI 与控制并行，runtime 可诊断 |
| P7 | 量产化 | OTA、故障、老化、Wi-Fi、功耗、EMC相关验证 | 形成发布基线 |

### Table 25
| 测试 | 方法 | 通过标准 |
|---|---|---|
| Sensor ID | 启动日志读取 OV5640 ID | ID 正确且可重复 |
| Color bar | 开启 sensor test pattern | 像素正确，无乱序 |
| 320x240 | 连续 capture | 10 fps附近稳定 |
| 640x480 | 连续 capture | 不出现持续 OOM |
| Mirror/flip | 切换配置 | Remote/WB显示正确 |

### Table 26
| 测试 | 通过标准 |
|---|---|
| SF G1 | TC275 ↔ S3-CAM 前导/命令/数据字节一致 |
| G3 latency | 控制延迟符合原 C6 基线 |
| G4 safety | 断链/坏帧/旧 SEQ 不导致错误持续运动 |
| G5 speed | 5→10→20 MHz 逐档门禁；失败即回退 |
| G6 aging | 长时间 telemetry + preview 无资源泄漏 |

### Table 27
| 场景 | 通过标准 |
|---|---|
| Drive + preview | 连续驾驶同时看 Camera，Control 无明显抖动/失联 |
| Camera WS loss | Camera 页 OFFLINE；Drive 不被误急停 |
| Control WS loss | 现有 RADIO LOST/zero output 逻辑触发 |
| Vision stale | 视觉结果过期时 Assist 自动解除 |
| OTA | S3-CAM OTA 与 TC275 OTA 都能从 Remote 完成 |

### Table 28
| ADR | 决策 | 原因 |
|---|---|---|
| ADR-001 | S3-CAM 替换 C6，不替换 TC275 | C6 与 TC275 之间的实时闭环边界不应因增加 Camera 而改变 |
| ADR-002 | Camera WS 独立于 Control WS | JPEG 大、可丢帧；控制小、不可被视频阻塞 |
| ADR-003 | vision JSON 不进入 proto v2 | 视觉结果小且变化快，减少共享协议复杂度 |
| ADR-004 | 保留 contracts/link 原样 | TC275 侧变更最小 |
| ADR-005 | OV5640 使用 Espressif camera driver | 已有官方传感器支持，降低驱动维护成本 |
| ADR-006 | SoftAP 保持 192.168.4.1 | 最大限度复用现有 Remote 与 Web 架构 |
| ADR-007 | Camera 默认 320x240 / 8-10fps | 更适合 S3 Remote 和 2.4GHz 带宽，同时给 Vision 留余量 |
| ADR-008 | Vision 不直接拥有 PWM 权限 | TC275 保持最终运动与安全执行权 |

### Table 29
| 优先级 | 文件 | 动作 |
|---|---|---|
| 1 | esp32s3_cam_car/CMakeLists.txt | 新工程骨架 |
| 1 | esp32s3_cam_car/sdkconfig.defaults | S3 + PSRAM + OV5640 + SPI |
| 1 | components/camera/cam_driver.c | OV5640 init |
| 1 | components/vehicle_link/* | 从 c6_link 迁移 |
| 1 | components/vehicle_bridge/* | 从 c6_bridge 迁移 |
| 1 | components/camera_ws/* | 新增 |
| 1 | main/app_state.* | 加入 camera/vision |
| 2 | smartcar_remote/main/scr_link.c | 加 camera WS |
| 2 | smartcar_remote/main/app_state.h/.c | 新增 camera/vision state |
| 2 | smartcar_remote/main/ui/ui_pages.* | Camera/Vision 页面 |
| 3 | contracts/camera/* | 帧头/命令 schema |
| 3 | contracts/vision/* | vision event schema |
| 4 | doc/* | 迁移记录与验证报告 |

### Table 30
| 编号 | 资料 | 链接 |
|---|---|---|
| R1 | Freenove ESP32-S3-WROOM Board GitHub | https://github.com/Freenove/Freenove_ESP32_S3_WROOM_Board |
| R2 | Freenove camera_pins.h（ESP32S3_EYE 板级 Camera 引脚） | https://github.com/Freenove/Freenove_ESP32_S3_WROOM_Board/blob/main/C/Sketches/Sketch_07.2_As_VideoWebServer/camera_pins.h |
| R3 | Freenove README / FNK0085 资料入口 | https://github.com/Freenove/Freenove_ESP32_S3_WROOM_Board/blob/main/README.md |
| R4 | Freenove Product FNK0085 / FNK0085B | https://store.freenove.com/products/fnk0085 |
| R5 | Espressif esp32-camera README / OV5640 support | https://github.com/espressif/esp32-camera |
| R6 | Espressif esp32-camera Kconfig（OV5640_SUPPORT / AF_SUPPORT） | https://github.com/espressif/esp32-camera/blob/master/Kconfig |
| R7 | OV5640 Product Brief / DVP / 2592x1944 | https://cdn.sparkfun.com/datasheets/Sensors/LightImaging/OV5640_PB.pdf |
| R8 | lilicqyu-ship-it/smartcar | https://github.com/lilicqyu-ship-it/smartcar |
| R9 | smartcar esp32c6_car doc/04-link.md | https://github.com/lilicqyu-ship-it/smartcar/blob/main/esp32c6_car/doc/04-link.md |
| R10 | smartcar esp32c6_car doc/07-http.md | https://github.com/lilicqyu-ship-it/smartcar/blob/main/esp32c6_car/doc/07-http.md |
| R11 | smartcar contracts/link/proto_frames.h | https://github.com/lilicqyu-ship-it/smartcar/blob/main/contracts/link/proto_frames.h |
| R12 | smartcar smartcar_remote/main/scr_link.c | https://github.com/lilicqyu-ship-it/smartcar/blob/main/smartcar_remote/main/scr_link.c |