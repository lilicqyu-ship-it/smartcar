# SmartCar S3 Remote — 车载视觉遥控器详细设计说明书

| 项 | 内容 |
|---|---|
| 文档版本 | V1.0 |
| 日期 | 2026-10-02 |
| 上游真源 | [s3-gateway/doc/SmartCar_S3CAM_OV5640_详细设计说明书_V1.0](../../s3-gateway/doc/SmartCar_S3CAM_OV5640_详细设计说明书_V1.0.md)（下称 **S3CAM LLDD**，引用写作 LLDD §N）→ 本仓库 [doc/00-overview](00-overview.md) 的 [01-app-state](01-app-state.md) / [04-link](04-link.md) / [05-ctrl](05-ctrl.md) / [06-ui](06-ui.md) / [08-architecture-v2](08-architecture-v2.md)（**现有实现基线**）→ [产品规格书](ESP32-S3-LCD-EV-Board%20v1.5%20远距离智能遥控器——LCD%20UI-UX%20产品级需求规格书.md)（spec §N） |
| 硬件 | ESP32-S3-LCD-EV-Board-2（主板 MB v1.5，N16R16V：16 MB flash + 8 MB octal PSRAM）+ SUB3 子板 4.3" 800×480 RGB（ST7262E43）+ GT1151 触摸 |
| 设计目标 | 按 LLDD §13 把 Remote 从"遥控器"升级为 **Human + Camera + Vision + Vehicle Console**：新增 Camera WS 视频客户端、Vision 结果消费与辅助驾驶交互，同时保持既有控制语义与安全边界零回退 |
| 交付范围 | 仅设计。代码落点见 §16，网关侧依赖见 §13 |

## 0. 口径声明（先于一切）

1. **视频通道以 LLDD §8 的 Camera WS（`ws://192.168.4.1/ws/camera` + `CAM_FRAME_HEADER` + JPEG）为准**。s3-gateway 当前已落地的是独立 httpd `:81` MJPEG 单查看者推流（[s3-gateway doc/19](../../s3-gateway/doc/19-camera.md)），保留给手机 `<img>`；Remote **不设计 MJPEG 客户端**，避免同一块板子上出现两套视频消费者。网关侧需新增的组件在 §13 逐项列出，属于本设计的**前置依赖**。
2. **Control WS 与 proto v2 面零改动**（LLDD ADR-003/004、Table 10）：DRIVE/TELEMETRY/PAIR/OTA 全部维持 [04-link](04-link.md) 现状，S3-CAM 对 Remote 而言就是"C6 的替身"，会话、角色、SEQ 闸门、配对流程不变。
3. **安全边界不变**（LLDD ADR-008、spec §102-105）：Camera/Vision 的任何故障**不进入急停链**；Control WS 失联沿用现有 RADIO LOST 逻辑。本文不新增任何"视频触发停车"的路径。

---

## 1. 现有 Remote 基线与本次增量

### 1.1 可直接复用的能力（零改动）

| 能力 | 现有落点 | S3-CAM 场景用法 |
|---|---|---|
| proto v2 编解码 | `main/proto/proto_frames.[ch]`（与网关逐字节同源） | 不变 |
| Control WS 客户端 + 静默看门狗 | `main/scr_link.c` | 不变；hello/tc/pong/err 消费口径一致 |
| 配对 / token / 角色 | `scr_link` + Pairing 页 | 不变；token 同时作为 Camera WS 的身份查询参数 |
| 摇杆 / 30 Hz 控制 / STOP / 急停 | `main/scr_ctrl.c` | 不变（LLDD §2.2 "保持原样"） |
| 快照状态中心 + 告警仲裁 + 事件日志 | `main/app_state.c` | 扩展字段（§6），机制不动 |
| OTA 暂存与升级（C6/TC275） | `scr_svc.c` + fw 页 | 端点/分区名沿用，实际对象变为 S3-CAM（§11） |
| 双核规划 | LLDD 无关；本仓 [08-architecture-v2 §2](08-architecture-v2.md) | 全部新增任务进 **core 0**（§10） |

### 1.2 本次新增（对照 LLDD Table 29 优先级 2/3 的 Remote 侧条目）

| # | 新增 | 一句话定义 |
|---|---|---|
| N-1 | `scr_cam.c/.h` | Camera WS 客户端 + 帧头校验 + JPEG 解码 + 最新帧槽位（视频面唯一 owner） |
| N-2 | `contracts/camera/cam_frame.h` | 帧头二进制 schema，与网关逐字节同源 |
| N-3 | `contracts/vision/vision.h` | vision JSON 字段口径常量 + `drive_mode`/`cam_cmd` 文本消息 schema |
| N-4 | app_state camera/vision 字段组 | 视频与视觉状态进快照（只进元数据，不进帧数据） |
| N-5 | CAMERA 页 / VISION 页 | LLDD §13.2/13.3 布局在 800×480 上的适配 |
| N-6 | HOME 页视频面入口与状态 | 三分区 TAB（DRIVE/CAMERA/VISION）+ CAMERA OFFLINE 徽标 |
| N-7 | Diagnostics 页视频/视觉条目 | fps/drop/解码耗时/PSRAM 水位/vision runtime |

---

## 2. 总体架构（Remote 视角）

```text
                      ┌────────────────────────── S3 Remote ──────────────────────────┐
                      │                                                              │
  触摸/摇杆 ──30Hz──► │ scr_ctrl (core1) ──►┐                                        │
                      │                    │ DRIVE/0x32 (proto v2)                   │
                      │              ┌─────▼──────────┐      ┌───────────────────┐  │
                      │              │ scr_link (core0)│◄───►│  Control WS /ws    │  │ 二进制 proto + JSON
                      │              └─────┬──────────┘      │  (hello/tc/pong/   │  │ (telemetry/vision JSON)
                      │                    │                 │   err/vision/...)   │  │
                      │  app_state ◄───────┤                 └─────────▲──────────┘  │
                      │  (快照互斥)   ┌────▼──────────┐                │             │
                      │              │ scr_cam (core0) │◄──────────────┤ Camera WS   │
                      │  帧槽×3 ───► │  校验/解码/限速  │               │ /ws/camera  │
                      │              └─────────────────┘               │ (JPEG 二进制)│
                      │                    │ 最新帧指针                 └──────┬──────┘
                      │              ┌─────▼──────────┐                        │
                      │              │ taskLVGL (core1)│                        │
                      │              │ CAMERA/VISION页 │◄── 手机浏览器经 :81     │
                      │              └─────────────────┘    MJPEG 并行观看，互不影响
                      └──────────────────────────────────────────────────────────────┘
```

三条平面在 Remote 侧的兑现（LLDD Table 9）：

| 平面 | Remote 组件 | 队列/缓冲 | 失败语义 |
|---|---|---|---|
| Control Plane | `scr_ctrl` + `scr_link` | 现状（try-only 发送、零等待） | RADIO LOST 覆盖层 + DRIVE=0（不变） |
| Video Plane | `scr_cam` | 独立 WS 实例 + 3 帧槽，latest-only | CAMERA OFFLINE 徽标；**不触发任何控制动作** |
| Vision Plane | `scr_link`（文本面） + VISION 页 | 单快照槽（只留最新） | STALE 置灰 ASSIST，禁止沿用旧结果 |

## 3. 会话、端点与权限

| 项 | 值 | 依据 |
|---|---|---|
| Control WS | `ws://192.168.4.1/ws?token=<pair_token>` | 现状（04 §2） |
| Camera WS | `ws://<同一 host>/ws/camera?token=<pair_token>` | LLDD §8.2；token 缺失只降为 spectator，**不拒绝连接**（LLDD §7.1：读视频不需要独立 CTRL） |
| 角色判定 | 以 Control WS `hello.role` 为唯一裁决；Camera WS 不重复发角色 | 两连接指向同一网关会话表，避免双源 |
| 权限闸门 | profile 切换 / Vision 模式 / ASSIST 切换均需 `ctrl_role==true` | LLDD §13.4；Remote 本地禁用按钮 + 网关侧 `err{e:"auth"}` 双保险 |

要点：**Camera WS 与 Control WS 生命周期完全独立**——各自连接、各自重连（3 s 退避，同 `esp_websocket_client` auto-reconnect 口径）、各自静默看门狗。Control WS 断开时 Camera WS 一并断开（同一网关进程必然同步失效），但反向不成立：Camera WS 断开不触碰 Control WS 任何状态。

## 4. Camera WS 客户端（scr_cam）

### 4.1 帧头（与网关逐字节同源，LLDD Table 14）

```text
contracts/camera/cam_frame.h   —— 全部 little-endian

offset  size  field        校验规则
0       2     MAGIC        0xCA 0x56，不符 → 计 frame_err，丢整帧
2       1     VERSION      == 0x01，不符 → 同上（提示网关版本过旧/过新）
3       1     FLAGS        bit0 keyframe / bit1 event / bit2..7 reserved（忽略）
4       4     SEQ          单调；Δ∈(1,512) 计丢帧，回绕（重启）容许
8       4     TIMESTAMP_MS 网关 uptime，仅用于端到端时延诊断
12      2     WIDTH
14      2     HEIGHT       w×h 必须等于已订阅 profile，不符计 frame_err
16      4     JPEG_LEN     0 < len ≤ CONFIG_SCR_CAM_MAX_JPEG_BYTES，否则丢整帧
20      N     PAYLOAD      JPEG（DVP 直出，无进度式扫描）
```

头部解析在 WS 事件回调上下文内完成（拷贝到解码槽后立即返回，不在回调里解码）。

**分片语义（易错点，曾有 blocker）**：`esp_websocket_client` **不会**替我们重组分片。头文件对 `esp_websocket_event_data_t` 的定义是：`data_len` = 本事件携带的字节数；`payload_len` = **整帧**总长（注释原文 "payloads exceeding buffer will be posted through multiple events"）；`payload_offset` = 本事件数据在帧内的偏移。客户端 `buffer_size = 4096`，而 VGA JPEG 约 20–25 KB ⇒ 一条 WS 帧会拆成 5–6 个事件投递，每个事件的 `payload_len` 都等于同一个整帧长度。所以重组必须按 `memcpy(asm + payload_offset, data_ptr, data_len)` 写入、以 `ev->fin` 判定帧尾，拷贝长度**绝不能**取 `payload_len`（会越读堆、并让后片覆盖前片，最终 EOI 校验必挂 → 永远 NO SIGNAL）。取 `data_len` 后，`total = payload_offset + data_len` 恰为运行中的结束偏移，`total > ASM_CAP` 的越界检查也随之成立。对照 `scr_link.c` 控制面二进制路径即为本写法。

**一条 WS 帧 = 一个完整 CAM_FRAME** 是对网关的硬约束（§13 G-2）：Remote 侧只在 `fin` 后解析一次，不处理一帧多包/一包多帧。

### 4.2 订阅生命周期（LLDD §13.4 交互规则的代码化）

```text
状态机： IDLE ──进入CAMERA/VISION页──► CONNECTING ──cam_hello──► SUBSCRIBING
            ▲                                                     │ subscribe
   离开两页且无其他消费者 ── pause ──────────────────────────────► SUBSCRIBED
                                                                  │ 二进制帧流
                                             静默 > SCR_CAM_FRAME_TIMEOUT_MS
                                                                  ▼
                                                            STALE（徽标变灰）
```

| 事件 | 动作 |
|---|---|
| CAMERA 页 on_show | 确保 Camera WS 已连（首次连接在 `scr_link_start` 后即可拉起，见 §4.4）→ 文本发 `{"op":"subscribe"}`，**紧接着再发一条 `{"op":"profile","name":..}`**（档位取本机 `CONFIG_SCR_CAM_PROFILE`）：`framesize` 是网关传感器的一组全局寄存器，`/stream` 与 `/ws/camera` 共用，只 subscribe 不声明就等于接受网关自己的默认（VGA 640×480），而 UI 的 320/640 radio 按 106 只跟随回传尺寸——于是 Kconfig 写着 REMOTE_PREVIEW、radio 却永远指着 640 FULL。现在"谁订阅谁声明"，网关在会话退役时把寄存器恢复成它自己的默认（doc 19 §5.4） |
| VISION 页 on_show | 同上，并保证 Control WS 侧 vision 消费已使能 |
| 离开两页（切回 DRIVE 页/设置页） | 文本发 `{"op":"pause"}`；**保持 WS 连接不断开**（重连成本与首帧延迟远高于 idle） |
| Control WS 断开重连后 | 若当前仍在 Camera 页，重新 subscribe（网关重启后旧订阅作废） |
| `cam_hello`/`cam_state` profile 变化 | 更新 w/h，重建匹配尺寸的帧槽（见 §5.3 尺寸变更处理） |

进入页面只订阅视频、**不自动开启 Vision**；ASSIST/AUTO 一律用户显式触发（LLDD §13.4 逐条兑现）。

### 4.3 静默与重连判据（两级）

| 级 | 判据 | 动作 |
|---|---|---|
| 帧级 stale | 已 SUBSCRIBED 且 `now - last_frame_ms > CONFIG_SCR_CAM_FRAME_TIMEOUT_MS`（默认 2000，≈3 帧名义周期） | 视频区盖 `NO SIGNAL` 遮罩、FPS 显示 `--`、vision 若引用画面则按 stale 处理（§7.3）；**不断连** |
| 连接级 restart | `now - 任意cam_rx > CONFIG_SCR_CAM_RX_WATCHDOG_MS`（默认 10000，对齐 Control WS 10 s 口径） | `esp_websocket_client` 整链 restart；页面显示 CAMERA OFFLINE |

半开连接判据沿用 04 §2 的经验：iOS 式无 FIN 断连只能靠静默计时，不依赖协议层 close 事件。

**连接唯一所有者 = `cam_monitor_task`（250 ms 节拍监督者，实现回写）**：

- `disable_auto_reconnect=true`：`esp_websocket_client` 的内置盲重连被显式关闭，所有拨号/拆链都经监督者裁决，保证 Control 面状态先于视频面被检查。
- **链路联动拆链**：`!wifi_up || Control WS != CONNECTED` 时 `stop+destroy` 整个 Camera WS（状态置 OFFLINE）——对齐 §10"Control 断开则 Camera 一并断开"，且不会对已死的网关进程做风暴重试。
- **3 s 固定节拍重拨**：冷启动、DISCONNECTED/ERROR 边沿、看门狗 restart 全部收敛到同一条 `last_dial_ms` 节拍下恢复；每次拨号重读 pair token（角色/令牌轮换天然被覆盖）。恢复回连后 ≤250 ms 内自动重发 subscribe（订阅幂等）。
- **看门狗只在应流时刻静默判活**：`want_stream==false`（页面暂停、DRIVE 页驻留）时网关不发流是正常态，静默不触发拆链——消除"pause 被误判死链"的周期性断连。
- **暂停期 4 s keepalive ping**（2026-10 文本面落地新增）：`want_stream==false` 且已连接时，监督者每 4 s 发一条 `{"op":"ping"}`（CAM_WS_PING），pong 采样本平面 RTT 进 DIAG（`cam.ping_rtt_ms`）并在串口打一行 `camera pong: rtt=..ms`——这条 ping **不是续命租约**：`esp_http_server` 只在 socket 可读时进 handler，`recv_wait_timeout` 只是 accept 时给 socket 设的 `SO_RCVTIMEO`，网关不会因入站静默回收空闲 WS 会话（旧版本此处写的"5 s 不 ping 就被网关收走"是错的，已更正）。它的价值是三样：DIAG 有本平面自己的 RTT 样本（不等于 Control 面）、证明这条第二连接仍双向可用、以及让网关侧的入站 ops 路径在不开 CAMERA 页时也被台架验证到。

### 4.4 任务与并发结构

| 任务/上下文 | 核 | prio | 栈 | 职责 |
|---|---|---|---|---|
| `cam_ws` 客户端任务（`esp_websocket_client`，第二实例，`task_core_id=0`） | 0 | 5 | 6144 B | 收发回调；二进制帧转交 `scr_cam`，文本帧（cam_hello/cam_state）转 `app_state` setter |
| `cam_decode` 任务（新增，本组件私有） | 0 | 4 | 8192 B | 等帧信号量 → JPEG 解码 → 写显示槽 → 通知 LVGL |
| Control WS 客户端任务 | 0 | 5 | 6144 B | 现状不变（08 §2） |

约束：
- Camera WS **必须用独立的 `esp_websocket_client` 实例**，不与 Control WS 共享（LLDD §15.1：控制 tx 队列与视频 tx 队列分离；`esp_websocket_client` 的单连接单事件队列天然是共享的）。
- `cam_ws` 事件回调里**禁止**：JPEG 解码、磁盘/HTTP 类 IO、任何阻塞发送。回调仅 `memcpy` 进 JPEG 槽 + `xSemaphoreGive`。
- 发送 `subscribe/pause/cam_cmd` 走该实例的 send_text（内部互斥），调用方为 LVGL 任务（页面事件）与 ctrl（不参与）——发送超时 try-only，失败丢弃等下节拍（订阅消息由状态机幂等重发）。

## 5. 解码与显示管线

### 5.1 解码器选择

ESP32-S3 无 JPEG 硬解，采用 `esp_jpeg` 1.x（TJpgDec 封装，IDF 组件管理引入；初稿写的 `esp_new_jpeg` 是 2.x 的另一套 API，锁 1.3.x 后按此回写，见 §12）：

| 项 | 口径 |
|---|---|
| 输入 | `JPEG_IMAGE_FORMAT_RGB565` + `swap_color_bytes` 直接输出，一次解码整帧 |
| 320×240 耗时 | 实测基线目标 ≤25 ms/帧（240 MHz、释放 RGB 交织）；10 fps 占空 ≤25%，core 0 可承受 |
| 内存 | 解码输出 buffer（帧槽）与 JPEG 输入槽一律 `MALLOC_CAP_SPIRAM`（内部 RAM 已被 Wi-Fi/lwIP/DMA 专属，08 §2.2 教训）；TJpgDec scratch ≈8 KB 单独分配、优先内部 RAM |
| 失败帧 | 截断 JPEG（丢包产物）：`esp_jpeg_decode` 返回错误 → 计 `decode_err`，保留上一帧显示并尽快按 stale 处理，**不崩帧管线** |

### 5.2 帧槽模型（latest-only，LLDD Table 15 的 Remote 侧兑现）

```
        写入侧（cam_decode）                读取侧（LVGL 任务，10 Hz 定时器）
   ┌────────────────────────┐          ┌──────────────────────────────┐
   │ 解码到 slot[free]       │          │ 取 slot[latest]（seq 比较）   │
   │ slot[free].seq = SEQ    │──交换──► │ lv_image_set_src(→ RGB565)    │
   │ 原子指针交换，不持锁渲染 │          │ 若 seq 未变 → 不触碰对象      │
   └────────────────────────┘          └──────────────────────────────┘
   3 槽 × w×h×2 B（320×240 → 150 KB；640×480 → 600 KB）
   + 1 JPEG 输入槽（64 KB）—— 环形深度 1：解码中又来新帧则覆盖待解槽，计 drop
```

- **不做帧队列**。任何时刻每个尺寸只保留最新完整帧（LLDD §8.3"只保留最新完整帧"）。
- 双缓冲够用于渲染，第三槽用于解码目标（避免 LVGL 正在读的槽被就地覆写产生半帧）；读侧只按指针换槽，写侧永不触碰 `slot[displayed]`。
- profile 变更（320×240 ↔ 640×480）：分配新尺寸三槽 → 释放旧槽 → 清屏占位；期间丢弃尺寸不符帧。上限受 `CONFIG_SCR_CAM_MAX_DECODE_PX`（默认 640×480）约束，超出则拒绝订阅该 profile 并提示。

### 5.3 显示与缩放策略

- **1:1 显示，禁用 LVGL transform 放大**。缩放走 CPU 双线性是 core 1 上最贵的逐像素路径（06 §4 R-9 同类教训），画面观感交给网关侧 profile：默认 `REMOTE_PREVIEW` 320×240（占 800×480 屏左 40%），可一键切 `WEB_PREVIEW` 640×480 全屏宽显示。
- 渲染刷新跟随 LVGL 既有 100 ms 单实例定时器（06 §1），不为视频新开定时器；`seq` 未变不 invalidate，防止 RGB bounce-buffer 空转。
- 视频区 `lv_image` 使用裸 RGB565 `lv_image_dsc_t`（`LV_COLOR_FORMAT_RGB565`），解码输出布局与之直接对齐，零拷贝引用 PSRAM 槽。

## 6. app_state 扩展（保持快照小对象原则）

`scr_state_t` 新增两组字段；**帧数据不进快照**（视频面只进元数据，快照维持 ≈500 B 量级）：

```c
/* video plane（写入方：scr_cam / cam_ws 文本回调） */
struct {
    scr_cam_conn_t conn;        /* IDLE/CONNECTING/CONNECTED      */
    bool     subscribed;
    char     sensor[8];         /* "OV5640"，来自 cam_hello       */
    uint16_t w, h;              /* 当前 profile                   */
    uint8_t  fps_x10;           /* cam_state 名义帧率×10          */
    uint32_t seq, drop, frame_err, decode_err;
    uint16_t decode_ms_max;     /* 近 1 s 最大解码耗时            */
    uint16_t e2e_ms;            /* TIMESTAMP_MS 差值滑动均值（诊断）*/
    bool     stale;             /* §4.3 帧级 stale                */
    /* record_state / sd_free_kb 已随 SD 硬件裁剪移除（2026-10，见 §17 R-ADR-09） */
} cam;

/* vision plane（写入方：scr_link 文本回调） */
struct {
    uint8_t  mode;              /* OFF/LINE/COLOR/QR/OBJECT       */
    bool     valid;
    uint8_t  confidence;        /* 0..100 %                       */
    int16_t  cx, error_x1000;   /* 线检测：中心偏移与误差×1000     */
    int16_t  angle_x10;         /* ×0.1°                          */
    uint32_t ts_ms;             /* 上行快照时算 fresh              */
    bool     fresh;             /* < CONFIG_SCR_VISION_STALE_MS    */
    uint8_t  drive_mode;        /* MANUAL/ASSIST/AUTO（回读自 cam_state/vision ack） */
    uint16_t objects_count;     /* object 模式仅计数 + 首框（面板级摘要） */
} vision;
```

- `vision.fresh` 判定在 `app_state_snapshot()` 内进行，与遥测 `tele_fresh` 同构（spec §101：过期不展示旧值冒充实时）。
- 告警槽位从 5 扩到 8（camera/vision 类告警可与 radio/battery/fault 并存，互不挤占）。原设计的 `ALERT_CAM_SD_FULL` 随 SD 硬件裁剪取消（R-ADR-09）。**CAMERA OFFLINE 不做全屏覆盖层**，只走页面徽标 + 状态栏着色（LLDD Table 21 行 2 的 Remote 动作是"页面显示"，spec §22"普通信息不全屏"）。
- 系统状态推导 `app_state_derive()`（01 §3.2）**不引入任何 cam/vision 输入**——保持"Camera 是非安全传感器"（LLDD §17）在状态机层面的兑现。

## 7. Vision 面设计

### 7.1 结果消费（Control WS 文本面，LLDD §11）

`scr_link` DATA(op=0x01) 分支新增 `{"t":"vision",...}`：单快照直接覆盖写入（无历史缓存，LLDD §11），字段解析容错（未知 mode/多余字段忽略并计数）。推送频率由网关按页面可见性调节（进 VISION 页 → `{"op":"vision_rate","hz":10}` 建议消息，见 §13 G-5）；Remote 只被动消费。

### 7.2 指令上行（全部 CTRL 门控，非 CTRL 禁用按钮 + 说明）

| 交互 | 消息（Control WS 文本，契约常量落 `contracts/vision/vision.h`） |
|---|---|
| VISION 页 [LINE]/[COLOR]/[QR]/[OBJECT]/[OFF] | `{"t":"vision_cmd","mode":"line","enable":true}` |
| [MANUAL]/[ASSIST]/[AUTO] | `{"t":"drive_mode","mode":"assist"}`（LLDD Table 17 `vision_mode` 的驾驶侧细化；§13 G-6） |
| 网关回执 | `{"t":"drive_mode","mode":"assist","ok":true}` → 更新 `vision.drive_mode` |

**AUTO 仅置灰占位**（LLDD §10 Table 16：全自动不在 V1.0），按下提示"未实现"。

### 7.3 stale 与安全联锁（LLDD §13.4 / Table 21 行 4）

- `vision.fresh==false` 或 `cam.stale==true` 时：VISION 页数值区显示 `STALE`，overlay 线隐藏，[ASSIST] 按钮强制回 [MANUAL] 显示并禁用——**Remote 端不沿用旧视觉结果做状态展示**（视觉修正本身在网关侧有独立 confidence/freshness 门，双端各自兜底）。
- Control WS 掉线：`drive_mode` 显示 UNKNOWN，overlay 走现有 RADIO LOST 逻辑（不变）。

## 8. UI 设计（800×480 适配）

### 8.1 导航变更（实现后回写：底部 TAB + 视频页可见）

三 TAB 采用**底部 TAB 条（44 px），仅在 CAMERA/VISION 页可见**。相对初稿的两处偏差及理由：

1. 初稿"主页顶栏下方常驻三 TAB"→ 实装为底部、非视频页隐藏：DRIVE 页布局已真机验证（摇杆半径/STOP 触控区），顶栏下常驻会纵向压缩约 40 px，风险大于收益（对应 §18 原风险 4，已消解）。
2. DRIVE 页进入 CAMERA 的入口 = 顶栏 **CAM 徽标**（可点击，`ui_nav_open(UI_PAGE_CAMERA)`），兼作状态显示。

```
┌──────────────────────────────────────────────────────────────┐
│ SMART CAR      ● CONNECTED  ● CAM 9.8fps  [⚙ 设置]            │ ← 顶栏沿用，新增 CAM 徽标（可点）
├──────────────────────────────────────────────────────────────┤
│                                                              │
│   CAMERA / VISION 页底部：[ DRIVE ]  [ CAMERA ]  [ VISION ]   │ ← TAB 条仅视频页显示
```

- TAB 只在 `conn==CONNECTED` 下可点（其余置灰 + toast）；DRIVE 页即现有 Home 驾驶布局（06 §3）整体保留，STOP 按钮仅 DRIVE 页可见（LLDD §13.4：Camera 页不驾驶——进 CAMERA/VISION 页时摇杆隐藏并清零 `scr_ctrl` 输入，DRIVE 恒发 (0,0) 心跳，与标定页 08 §5 同一安全模式）。
- CAM 徽标三态：绿 `9.8fps` / 灰 `STALE` / 红 `OFFLINE`（色+字+图标三重表达，spec §51）。
- **"置灰 + toast" 不能用 `LV_STATE_DISABLED`**：LVGL 对 DISABLED 对象不派发 `CLICKED`（`lv_indev.c` 的 `is_enabled` 前置判断），按钮变灰后 tap 根本进不到回调，toast 就成了死代码。故这类**信息型门控**（三 TAB、CAMERA profile、VISION mode/drive）统一走 `ui_set_blocked()`：保持可点、用递归 `OPA` 淡出（子 label 随父一起变暗）、取消 PRESSED 高亮，tap 时回调读同一份原因串既出 toast 也填侧栏文案。反例是标定/OTA/清故障（08 §5）——那些是**安全型硬禁**，必须用真 `LV_STATE_DISABLED`，不可改回 blocked。

### 8.2 CAMERA 页（LLDD §13.2 布局的 800×480 展开）

```
┌──────────────────────────────────────────────────────────────┐
│ ● LIVE   9.8 fps   320×240   drop 2/1200   e2e 210 ms          │ ← 36 px 信息条（非模态）
├───────────────────────────────┬──────────────────────────────┤
│                               │ CAM OV5640                   │
│                               │ [320 PREVIEW]  (checked)     │
│   视频区 640×400（1:1 居中，    │ [640 FULL]                   │
│   黑底，NO SIGNAL/PAUSED 遮罩   │ [VISION PAGE]                │
│   盖于其上）                    │ （置灰原因文案）               │
├───────────────────────────────┴──────────────────────────────┤
│ [ DRIVE ]  [ CAMERA ]  [ VISION ]                    44 px TAB │
└──────────────────────────────────────────────────────────────┘
```

- WEB_PREVIEW 640×480 时进入沉浸式：信息条隐藏、视频区占满左列全高、TAB 条隐藏（由回读的实时分辨率驱动，见 5.3）；退出档位自动恢复。
- PROFILE 按钮非 CTRL 角色置灰并在侧栏底部显示原因文案（"CAM LINK DOWN / NEED CONTROL / LINK NOT READY"），与标定页禁用文案同风格（08 §5）。
- PHOTO / REC / SD 行已随网关硬件裁剪移除（R-ADR-09）：S3-CAM 无 MicroSD，snapshot/record 无落盘介质，`cam_state` 的 `sd`/`rec` 字段 Remote 侧直接忽略。

### 8.3 VISION 页（LLDD §13.3 展开）

```
┌──────────────────────────────────────────────────────────────┐
│ VISION   MODE: LINE   conf 92%   18 ms   [STALE 时整页变灰]    │
├───────────────────────────────┬──────────────────────────────┤
│  视频区 320×240（复用同一帧槽）│ X ERROR      -0.018          │
│   + overlay 图层：            │ ANGLE        -4.2°           │
│     LINE: lv_line 折线         │ CONFIDENCE   92 %            │
│     COLOR: 四角色块            │ ASSIST       ON / OFF        │
│     QR: 中心框+ID 文本         │ RTT(vision)  143 ms          │
│                               ├──────────────────────────────│
│                               │ [OFF][LINE][COLOR][QR][OBJ]  │
│                               │ [MANUAL] [ASSIST] [AUTO]     │
├───────────────────────────────┴──────────────────────────────┤
│ [ DRIVE ]  [ CAMERA ]  [ VISION ]                             │
└──────────────────────────────────────────────────────────────┘
```

- overlay 用 `lv_line`/`lv_obj` 原始坐标按当前 w/h 等比映射（vision 输出为网关图像坐标，§13 G-4 约定坐标系）。
- overlay 刷新跟随帧显示节拍（≤10 Hz），数值区跟随 LVGL 100 ms 定时器。
- 视觉 overlay 绘制在 core 1 上是小面积 invalidate，实测预算内；若真机帧率不达标，降级为纯数值显示 + 静态线（预留给 `CONFIG_SCR_CAM_OVERLAY` 开关）。

### 8.4 Diagnostics / Settings 增补

| 页 | 条目 |
|---|---|
| Diagnostics | Camera：sensor/up/fps/seq/drop/frame_err/decode_ms/e2e_ms、PSRAM free；Vision：mode/valid/runtime/drop；网关 `/api/diag` 轮询 JSON 增加 camera/vision 段解析（LLDD §18.2 字段名对齐） |
| Settings → Display | 新增 `CAM STREAM` 开关（默认 ON；关闭 = 永不建 Camera WS，纯省电/调试用） |
| Radio 页 | 现有 RTT 行下补一行 `CAM LINK`：Camera WS 连接态 + cam 静默计时（诊断半开用） |

## 9. 数据流与端到端预算

```
网关 capture(~100 ms) → WS 分帧 → Wi-Fi 空口(~10-40 ms) → cam_ws 回调拷贝(<1 ms)
→ 信号量 → cam_decode(≤25 ms) → 换槽 → LVGL 定时器(≤100 ms) → bounce 上屏(≤1 帧周期)

capture→display 名义 ≤ 280 ms，验收线 ≤ 350 ms（LLDD 未定量化，本设计取"摇杆可见响应不迟滞"档）
```

| 资源 | 预算 | 放置 |
|---|---|---|
| 视频带宽 | 320×240 JPEG ~25 KB ×10 fps ≈ 2.0 Mbps（LLDD §16.1）；叠加 Control WS 遥测回程 ≈ 0.1 Mbps | STA 侧 2.4 G，AP 由网关限速兜底 |
| 帧槽 | 3×150 KB + JPEG 槽 64 KB ≈ 0.5 MB（REMOTE_PREVIEW）；640×480 档 3×600 KB + 64 KB ≈ 1.9 MB | PSRAM（屏三帧缓冲 + LVGL 堆之外仍充裕，见下） |
| PSRAM 全账 | 显示 3×768 KB + LVGL 堆 1-2 MB + 视频 ≤2 MB ≤ 5.5 MB / 8 MB | 余量 ≥25%；分配失败 → 订阅自动降回 REMOTE_PREVIEW（§11 OOM 降级） |
| 内部 RAM | cam_ws 任务栈 + 信号量 + 帧头解析 < 20 KB | 控制队列与 Wi-Fi DMA 不受挤占（08 §2.2） |
| core 0 CPU | cam_decode ≤25% + WS 事件；低于 lwIP(18)/wifi(23) 优先级 | 控制面抢占绝对占优 |
| core 1 | 无新增任务；仅视频区渲染（1:1、≤10 Hz、有 seq 门） | ctrl 6 > LVGL 4 节拍不变 |

防卡顿不变量（实现回写，任何后续改动不得破坏）：

1. **队列深度恒为 1**：WS 回调→解码之间只有一个 depth-1 双缓冲 pend 槽（新帧覆盖旧帧），不存在任何会积压延迟的帧队列；解码慢于收流的表现是丢帧（计数可见），不是延迟增长。
2. **渲染永不读半写槽**：3 槽环 + `display_idx/newest_idx` 双重互斥，解码目标恒避开 LVGL 正在显示的槽；分辨率切换走一代延迟释放（`s_retire`），LVGL 持有的指针在换代前始终有效。
3. **seq 变化是唯一 invalidate 源**：10 Hz 泵只在新解码帧到达时触发重绘，静态画面零渲染成本，不与 LVGL 全屏刷新抢带宽。
4. **视频面任何路径不触碰控制面**：独立 WS 实例、独立任务、core 0 优先级低于 lwIP/Wi-Fi；解码/拷贝再慢也只拖 FPS，不拖 DRIVE 节拍。
5. **断流恢复自动化**：帧级 2 s 显示 NO SIGNAL → 静默强制重订阅；连接级 10 s 静默整链 restart；Control 断链立即拆 Video；恢复后 ≤3 s 重拨 + ≤250 ms 重订阅，全程无需用户操作。

## 10. 安全与故障矩阵（LLDD Table 21 Remote 列的逐条兑现）

| 故障 | 检测点 | Remote 行为 | 不做什么 |
|---|---|---|---|
| Control WS 断开 | 现有静默看门狗 | RADIO LOST 覆盖层 + DRIVE 停发（TC275 兜底停车）；Camera WS 连带清理重建 | 行为零改动 |
| Camera WS 断开 / cam 静默 | §4.3 两级判据 | 徽标 OFFLINE、页面遮罩、vision 引用置 stale | **不弹全屏、不发停车帧、不触碰 emerg/stop 锁存** |
| Vision stale | `!fresh` | STALE 文案 + ASSIST 回退显示 + 按钮禁用 | 不停车（网关侧另有 confidence_gate） |
| 解码连续失败（>10 帧） | `decode_err` 滑窗 | 自动 pause→resubscribe 一次，再失败显示 `CAM DEGRADED` 并停止订阅重试风暴 | 不影响 Control WS |
| PSRAM 分配失败 | 帧槽分配路径 | 降档 REMOTE_PREVIEW；再失败则 unsubscribed + `CAM DEGRADED` | **不重启任何任务、不释放显示帧缓冲** |
| E-STOP | 现有 05 §3 | 现有语义逐字保留；急停锁存期间 TAB 仍可切页（停车态浏览画面安全） | — |

发送优先级规则（Remote 本地）：`STOP/DRIVE(0,0) > DRIVE 心跳 > ping > cam 订阅/控制文本 > 其余`。前两者在 ctrl 任务、订阅文本在 LVGL 事件上下文、且分属两个 WS 实例，天然互不排队——该表是对未来合并实现者的约束说明。

## 11. OTA / 版本口径

- LLDD Table 22 的"Remote staging → S3-CAM → TC275 relay"在本仓库**已实现**（08 §3/§4），本次仅换称谓不改字节面：
  - `fw_c6` 分区、`C6FW` bundle magic、`POST /ota/c6` 端点**全部沿用**（网关协议面零改动，`s3-gateway/doc/00-overview` 明确"协议面（proto v2 / SF 帧 / `/ota/c6` / `C6FW`）零改动"）。
  - UI 文案统一从 "C6" 改为 "车端网关 / GATEWAY"（About、FW 页、诊断条目），版本号显示 hello `ver` 字段（`s3cam-v…`）。
- 新增约束：**Camera WS 建立/重建不影响 OTA 流**（不同 TCP 连接、不同 httpd 实例语义），但 FW 页升级前置检查增加一条"当前不在 CAMERA/VISION 页"（避免驾驶向带宽与升级流并发，代价小的保守项）。

## 12. 配置项（main/Kconfig.projbuild 新增，实现后回写为实装一致）

```text
menu "Camera / Vision (S3-CAM video plane)"
  config SCR_CAM_WS_ENABLE        bool "Camera WS 客户端"          default y
  config SCR_CAM_STREAM_PORT      int    "Camera WS 端口（网关 :81 流实例）" default 81 range 1..65535
  choice SCR_CAM_PROFILE          首选档位（UI 内可切换）
    default SCR_CAM_PROFILE_REMOTE            # REMOTE_PREVIEW 320×240
    alt   SCR_CAM_PROFILE_WEB                 # WEB_PREVIEW    640×480
  config SCR_CAM_MAX_JPEG_BYTES   int    "单帧 JPEG 协议上限"      default 65536  range 8192..131072
  config SCR_CAM_MAX_FPS          int    "订阅名义帧率上限"         default 10     range 1..30
  config SCR_CAM_FRAME_TIMEOUT_MS int    "帧级 stale 阈值"         default 2000   range 500..10000
  config SCR_CAM_RX_WATCHDOG_MS   int    "Camera WS 静默重连"      default 10000  range 3000..60000
  config SCR_CAM_SLOTS            int    "最新帧槽环数量"           default 3      range 2..4
  config SCR_CAM_OVERLAY          bool   "VISION 页画面叠加"        default y
  config SCR_VISION_STALE_MS      int    "视觉结果过期阈值"         default 1000   range 200..5000
endmenu
```

与初稿的差异（回写）：
- 删除 `SCR_CAM_URI_PATH`：路径固定 `/ws/camera?token=`，拼接自 `CONFIG_SCR_C6_IP`，可配路径是无意义自由度。
- 新增 `SCR_CAM_STREAM_PORT`（2026-10 回写）：网关把 `/ws/camera` 开在**独立流 httpd 的 :81**
  （与 `CONFIG_S3_CAMERA_STREAM_PORT` 成对，doc/21 §3.3 的端口偏差记录），URI 形如
  `ws://<ip>:<port>/ws/camera?token=`；初稿按 LLDD 表 12 写的 :80 同实例并未实现。
- 删除 `SCR_CAM_MAX_DECODE_PX_W/H`：改由 `SCR_CAM_SLOTS × 640×480` 槽预算封顶（解码器无上限配置，超限分配失败即降档，见 §10 OOM 行）。
- 新增 `SCR_CAM_SLOTS` 与 `SCR_CAM_PROFILE` choice（档位可切换 + 首选档位）。
- `SCR_CAM_MAX_JPEG_BYTES` 与 `contracts/camera/cam_frame.h` 的 `CAM_JPEG_LEN_MAX` 保持等值（协议天花板 65536）。

解码组件：`esp_jpeg` 锁定 **1.x**（`main/idf_component.yml: espressif/esp_jpeg "^1.3.0"`），其实为 TJpgDec 封装，`esp_jpeg_decode()` 无状态调用、需 ≤8 KB 内部 RAM scratch（scr_cam 启动时一次性分配，失败回退 per-call 分配）；RGB565 输出 `swap_color_bytes=1` 对齐 LVGL 小端序。

## 13. 网关侧依赖清单（本设计成立的前置，交付给 s3-gateway）

| # | 依赖 | 对应 LLDD 条款 | 现状 |
|---|---|---|---|
| G-1 | `camera_ws`：`/ws/camera` 端点、per-client 只发最新帧 | §8.2、§15.1 | 🟩 **已实现**（s3_camera 独立 :81 httpd 上的会话式 handler + 出站泵任务，见网关 doc/19 §5.2；**偏差**：不在 :80 同实例、单查看者而非 max 2——理由与对拉口径见网关 doc/21 §3.3，Remote 侧以 `SCR_CAM_STREAM_PORT` 对齐端口） |
| G-2 | 每 WS 帧恰好携带一个 `CAM_FRAME_HEADER + JPEG`，LE，SEQ 单调 | Table 14 | 🟩 **已实现**（网关 TX 已改用共享契约 `cam_frame.h` 的 `cam_frame_build()`，双仓副本进 `check-contracts.sh` 校验） |
| G-3 | 文本面 `subscribe/pause/profile` + `cam_hello`/`cam_state` 回读（含 sensor/fps 字段；`sd`/`rec` 字段已随硬件裁剪取消） | §8.2、Table 17 | 🟩 **已实现**（hello/1 Hz cam_state/pong；profile 走 `set_framesize` 寄存器路径，订阅时由本机声明、网关会话退役时恢复自身默认；REMOTE 侧暂停期 4 s keepalive ping 取本平面 RTT 样本，**不是**续命租约——网关不会因入站静默回收空闲 WS 会话，见 §4.2；`:80` 的 `cam_cmd` 文本面仍未实现——本平面 ops 直接走 Camera WS，不依赖它） |
| G-4 | Vision JSON `{"t":"vision"}` 经 Control WS 文本下行；坐标系为当前视觉 profile 像素 | §11 | 🔴（vision 组件本身未落地，属网关 P4） |
| G-5 | 按消费者可见性的 vision 频率（`vision_rate` 建议消息，或网关自定） | §13.4 | 🔴 |
| G-6 | `drive_mode` manual/assist/auto 与 confidence_gate | §10.2 | 🔴（网关 P5 Assist 阶段） |
| G-7 | `/api/diag` 增加 camera/vision 段（字段名按 LLDD §18.2 样例） | §18.2 | 🟡 s3_camera 接口已有、未接入 diag（19 §6 缺口） |
| G-8 | ~~snapshot/record 命令（PHOTO/REC 按钮语义）~~ **已取消**：S3-CAM 无 MicroSD，存储功能整体移出 V1.0（R-ADR-09） | §12 | ✅ 不再是依赖 |

**视频流畅性的网关侧前置（G-1/G-2 的验收含义）**：`camera_ws` 必须 per-client 只发最新完整帧、服务端不排队积压、TCP 写不阻塞 capture 路径——否则 Remote 的 latest-only 管线会把积压体现在空口延迟上，§9 的 e2e ≤350 ms 验收线直接失守；该条随 G-1 一并作为网关联调门禁。

**:81 MJPEG 的去留**：保留给手机浏览器 `<img>`（现网验证过、成本最低）；Remote 不消费。若网关未来把手机也迁到 Camera WS（LLDD §7 设想），:81 可退役，Remote 设计不受影响。

## 14. 分阶段实施计划（Remote 侧，对齐 LLDD Table 24 的 P1-P5）

| 阶段 | 目标 | 交付 | 验收 |
|---|---|---|---|
| R0 | contracts 冻结 | `cam_frame.h` 编解码 + 主机自测（构造/校验/坏帧注入）；`vision.h` 消息常量 | 双仓同源文件逐字节一致；host test 绿 |
| R1 | 链路打通 | `scr_cam` 连接/握手/订阅状态机 + 静默双级判据（依赖 G-1..G-3） | 拔网线/重启网关：OFFLINE→自动恢复；Control WS 无感 |
| R2 | 画面 | `cam_decode` + 三槽 + CAMERA 页 1:1 显示 + profile 切换 | 320×240 连续 10 min：实测 ≥8 有效 fps、e2e ≤350 ms、控制 RTT 无可测恶化 |
| R3 | 视觉面 | vision JSON 消费 + VISION 页 + overlay + drive_mode 按钮（依赖 G-4..G-6） | stale/无控制权/掉线三类联锁逐条演示通过 |
| R4 | 收尾 | 诊断页扩展、OOM 降档、事件日志接入、文案 GATEWAY 化 | §15 全表通过；老化 2 h 无泄漏 |

## 15. Bring-up 测试清单（Remote，LLDD Table 27 的 Remote 行展开）

| 场景 | 操作 | 通过标准 |
|---|---|---|
| Drive + preview | CAMERA 页订阅同时摇杆驾驶 | ctrl 30 Hz 节拍不抖（串口节拍日志）、RADIO LOST 0 次 |
| Camera WS loss | 拔网关电源 / 关 :81 之外整进程 | CAMERA OFFLINE；驾驶不被误急停；恢复后 ≤5 s 自动重订阅 |
| Control WS loss | 同上 | 现有 RADIO LOST + DRIVE=0 路径原样触发 |
| 帧级 stale | 网关相机挂死（test pattern 关闭帧流） | 2 s 内 NO SIGNAL；连接级 10 s 前不误重连 |
| 坏帧注入 | host 工具向流插入截断 JPEG / 错 MAGIC | frame_err/decode_err 增长，画面不花屏不崩 |
| Vision stale | 停 vision 任务（网关侧） | 1 s 内 STALE、ASSIST 自动弹回 MANUAL 且禁用 |
| 非 CTRL 权限 | 手机先配对抢占 | profile/vision 按钮禁用 + 原因文案 |
| profile 切换 | 320→640→320 | 换槽无泄漏（heap_caps 对账）、无尺寸不符帧显示 |
| PSRAM 降档 | 临时把帧槽上限调小触发失败 | 自动降 REMOTE_PREVIEW / CAM DEGRADED，不 abort |
| OTA 并发 | CAMERA 页发起 TC275 OTA | FW 页要求先离页；升级流无中断 |
| 老化 | preview+vision+30Hz 控制 2 h | drop 率稳定、堆水位不降、LVGL 帧率不衰减 |

## 16. 代码落点（实现后回写，与实际文件一致）

```text
contracts/camera/cam_frame.h        帧头结构 + parse/seq 记账（header-only，双仓同源）
contracts/vision/vision.h           消息类型/mode 常量 + builders（header-only；snapshot/record 已随 R-ADR-09 移除）
smartcar_remote/main/scr_cam.c/.h   Camera WS 客户端 + 状态机 + cam_decode 任务
smartcar_remote/main/app_state.[ch] §6 两组字段、setter、告警槽位扩 8、fresh 判定
smartcar_remote/main/scr_link.[ch]  DATA(0x01) 增 "vision"/"drive_mode" 分支 + scr_link_wifi_up()
smartcar_remote/main/app_main.c     scr_cam_start() 接线
smartcar_remote/main/ui/ui_camera.[ch]  CAMERA 页 + 共享 1:1 视频视图/遮罩
smartcar_remote/main/ui/ui_vision.[ch]  VISION 页（overlay/联锁）
smartcar_remote/main/ui/ui.c        底部三 TAB（仅视频页可见）+ 页切换 → scr_cam 订阅生命周期
smartcar_remote/main/ui/ui_home.c   顶栏 CAM 徽标（三态 + 点击进入 CAMERA）
smartcar_remote/main/ui/ui_pages.c  Diagnostics "CAMERA / VISION" 段 + Radio CAM link 行
smartcar_remote/main/Kconfig.projbuild  §12 menu（实装口径）
smartcar_remote/main/idf_component.yml  espressif/esp_jpeg "^1.3.0"
smartcar_remote/test/host/          cam_frame / vision builders host 自测（make check）
```

模块边界硬规则（违反即打回，沿用 00 一致性约束）：`scr_cam` 不 include 任何 LVGL 头；UI 不直接触碰 WS 句柄；跨任务数据只走 app_state 快照 + 帧槽指针交接；帧槽所有权唯一在 `scr_cam`。

## 17. 设计决策记录（Remote ADR）

| ADR | 决策 | 理由 |
|---|---|---|
| R-ADR-01 | Remote 视频通道取 Camera WS（LLDD §8），不消费现网 :81 MJPEG | 与 LLDD ADR-002 的分离面一致；MJPEG 单查看者/无帧头/无 SEQ，无法做丢帧策略与 e2e 诊断；两套客户端长期是维护负债 |
| R-ADR-02 | 解码放 core 0 独立任务，不进 LVGL 任务 | 08 §2 铁律：等网络、大块 memcpy 不上核 1；decode ≤25% CPU 与 Wi-Fi 同核可接受 |
| R-ADR-03 | latest-only 三槽环，不建帧队列 | LLDD Table 15"只发/只取最新完整帧"；队列只会累积延迟，与低延迟目标背道而驰 |
| R-ADR-04 | Camera/Vision 状态不进 `app_state_derive` 优先级链 | LLDD §17：Camera 是非安全传感器；防止视频抖动被状态机放大为"车辆异常" |
| R-ADR-05 | 1:1 显示 + profile 切换，不做 LVGL 缩放 | 缩放是核 1 最贵渲染路径；分辨率档位本来就是网关 profile 职责 |
| R-ADR-06 | 页面离开只 pause 不断连 | 重连握手 + cam_hello + 首帧 >1 s，远贵于 idle 保活 |
| R-ADR-07 | 角色裁决唯一在 Control WS hello | 双连接各发角色会产生两个真源；Camera 连接只继承 `ctrl_role` |
| R-ADR-08 | OTA 面沿用 `fw_c6`/`C6FW`/`/ota/c6` 命名，只改 UI 称谓 | 网关协议面零改动是其 00-overview 的自declared 约束；改名是纯 churn |
| R-ADR-09 | SD 存储功能整体裁剪（2026-10）：S3-CAM 取消 MicroSD 硬件，PHOTO/REC/SD 容量显示、`cam_state.sd/rec` 字段、`ALERT_CAM_SD_FULL` 全部移除 | snapshot/record 的落盘介质就是 SD，无卡即无功能；协议面收窄比留死字段更诚实，网关侧对应收窄见 S3CAM LLDD 同步修订 |

## 18. 风险与开放问题

1. **G-1..G-8 未落地前，R2 之后阶段无法联调**——R0/R1 可用 host 侧假流（`test/host/` 扩展一个 cam frame 生成器）先行。
2. `esp_jpeg` 1.x（TJpgDec）在 640×480 档实测耗时可能超 40 ms，届时 WEB_PREVIEW 名义 8 fps 会退化——档位降级策略（§10 OOM 行同款）覆盖此场景，需在 R2 实测表中单列。
3. Vision overlay 坐标系依赖网关视觉 profile 与预览 profile 是否同尺寸；LLDD Table 8 中 VISION 档可为 160×120——G-4 必须约定坐标以**预览帧宽高**归一或声明缩放系数，R0 冻结 schema 时钉死。
4. ~~三 TAB 常驻压缩驾驶页纵向空间约 40 px~~ 已消解：TAB 条只在 CAMERA/VISION 页显示（§8.1 回写），DRIVE 页保持真机验证过的原布局。
5. 手机 :81 MJPEG 单查看者 + Remote Camera WS 2 客户端并存时，网关上行总带宽 ≈ 2+4 Mbps 空口竞争——首版接受（spectator 非驾驶常态），若 RTT 实测受影响，由网关侧 per-client 限速（LLDD §16 第 3 条）解决，Remote 无需改动。

---
（完 — 本说明书评审通过后，R0 contracts 冻结作为第一个工作包启动。）
