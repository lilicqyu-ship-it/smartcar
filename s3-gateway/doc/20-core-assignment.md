# 20 双核功能分配（ESP32-S3）

| 项 | 内容 |
|---|---|
| 代码位置 | 任务创建点：`components/s3_link/link.c`、`s3_bridge/bridge.c`、`s3_ota/ota_self.c`、`s3_led/led.c`、`s3_net/{captive_dns,mdns_lite}.c`、`s3_legacy/legacy_tcp.c`、`s3_http/http_server.c`、`s3_camera/camera_stream.c`、`main/app_state.c`；核配置：`sdkconfig.defaults` 的 "core split" 段 |
| 上游需求 | LLDD §2.3（任务与优先级）/ §2.4（数据流）；本工程从单核 C6 移植到双核 S3，C6 侧文档无对应章节 |
| 状态 | 🟩 **已实现** — 推流 + 联机下的 RTT 分布待 HIL（§5） |

## 1. 为什么要显式分配

ESP32-S3 是两个 Xtensa LX7：核 0（PRO CPU）+ 核 1（APP CPU）。C6 版全部用无亲和的
`xTaskCreate`（单核，无所谓亲和），移植到 S3 后这些任务由调度器自由摆放，最坏情况是
板级外设任务与射频栈挤在同一个核上。移植完成后必须把摆放规则写下来，否则每次改优先级
都要重新推理一遍。

三条硬约束决定了唯一的合理切法：

1. **射频面搬不走**：WiFi 驱动任务（prio 23）绑核 0（`CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0`），
   `esp_timer` 守护任务（prio 22）绑核 0，`app_main` 默认亲和也是核 0。凡是与它们同上下文
   的东西只能跟着留在核 0。
2. **采集是突发带宽型负载**：`cam_task` 的优先级是 `configMAX_PRIORITIES - 2`（= 22），
   比 lwIP `tcpip`（18）还高。它若留在核 0，每一帧 JPEG 落 PSRAM 的拷贝窗口都会抢占
   `tcpip`，直接体现为摇杆命令的 RTT 抖动。
3. **关 cache 是全片事件**：flash 写（自板 OTA、otadata、NVS、coredump）会临时关 cache，
   两个核的 XIP 同时停。分配方案能保证"抖动不是常驻负载造成的"，不能保证 OTA 写入期间
   无抖动。

结论：**核 0 = 面向 socket 的一切，核 1 = 面向引脚的一切。**

## 2. 分配表

### 核 0 — RF/IP 面

| 任务 | 优先级 | 栈 | 摆放方式 | 职责 |
|---|---|---|---|---|
| `wifi` | 23 | IDF | IDF 固定 | 射频驱动 |
| `esp_timer` | 22 | 3584 | IDF 固定 | 6 个定时器回调：`bridge_tick` 20 ms、`sf_alive` 10 ms、`heap_guard` 10 s、`rollback_chk` 45 s 单次、http 套接字表巡检 1 s、OTA 重启延时；**只做置位/通知，不做 IO** |
| `tcpip` | 18 | IDF | `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y`（原 `NO_AFFINITY`，会漂到核 1） | lwIP |
| `httpd_server` | 5 | 8192 | `cfg.core_id = 0` | :80 控制页 + assets + REST + WS 会话表 |
| `httpd`（推流） | 3 | 4096 | `config.core_id = 0` | :81 MJPEG 写 socket |
| `captive_dns` / `mdns_lite` | 4 | 3072 / 3584 | 显式核 0 | UDP 53 端口劫持 / mDNS 应答 |
| `legacy` / `legacy_cli` | 4 | 3072 | 显式核 0 | `CONFIG_S3_LEGACY_TCP`（默认关）TCP 8080 |
| `app_main` | 5 | 6144 | IDF 默认 CPU0 | 组合根，编排完即 60 s 节拍空转 |
| `idle0` | 0 | 1536 | — | `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y` |

### 核 1 — 板级/控制面

| 任务 | 优先级 | 栈 | 摆放方式 | 职责 |
|---|---|---|---|---|
| `cam_task` | 22 | 4096 | `CONFIG_CAMERA_CORE1=y` | GDMA 帧落地 + fb 轮转（`CAMERA_FB_IN_PSRAM`，2 帧） |
| `link_task` | 12 | 5120 | `LINK_TASK_CORE` | SF 帧收发、共享寄存器握手、链路健康监测 |
| `bridge` | 10 | 6144 | `BRIDGE_TASK_CORE` | 三台泵：命令下行 / 50 Hz 遥测广播 / OTA 中继信用窗 |
| `ota_task` | 8 | 6144 | `OTA_TASK_CORE` | bundle 解析 + ed25519 验签 + flash 写 |
| `rb_chk` | 5 | 3072 | 显式核 1 | 回滚确认（NVS / otadata 写），45 s 后一次性 |
| `s3_led` | 3 | 2048 | 显式核 1 | WS2812 图案（RMT 发送） |
| `idle1` | 0 | 1536 | — | `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y` |

核 1 上的抢占关系是一条干净的阶梯：`cam(22) > link(12) > bridge(10) > ota(8) > led(3)`，
重要性从上到下递减，且都低于核 0 的射频中断。

## 3. 接缝与代价

- **ISR 落在哪个核**：`esp_intr_alloc()`（不带亲和参数的旧接口）把中断分配到调用它的任务
  所在核，未绑核则核 0。本仓所有外设 init 都在 `app_main`（核 0）里执行，所以 SPI-HD、
  摄像头 GDMA、RMT、GPIO 的 ISR 全在核 0；核 1 的任务只是被 ISR 唤醒，多一次跨核 IPI
  （µs 级）。刻意不搬：这些 handler 是 `IRAM_ATTR`、只做置位 + notify，短；要把 SPI/GDMA
  ISR 挪到核 1，就得把 `link_init()`/`camera_start()` 挪进一个绑核 1 的任务里执行，代价是
  启动时序与错误返回路径重排。若将来 TC275 侧发现 CS 握手抖动，再按此路改。
- **唯一需要盯的抢占**：`cam_task(22)` 抢占 `link_task(12)`。VGA JPEG 一帧的落地拷贝在
  ms 级以内，而 LINK 侧重发容忍是 `LINK_WATCHDOG_MS = 500`，遥测节拍 20 ms 允许丢帧 →
  判定可接受。超标的两个旋钮：降分辨率/帧率（`CONFIG_S3_CAMERA_FRAME_SIZE_ID`），或把
  `LINK_TASK_PRIO` 提到 20 以上（单行宏）。
- **`bridge` 放核 1 的理由**：它的 WS 广播最终都要进 lwIP（核 0），无论 bridge 在哪个核都
  是一次跨核 IPC；而 `tcpip(18)` 本来就高于 `bridge(10)`，同核也照样抢占它。放核 1 换来
  的是"50 Hz 打包 + 发送的 CPU 时间不再与射频同核"，时延近似不变。
- **推流与遥控的相对顺序**：推流 httpd 在核 0、优先级 3，低于控制 httpd(5)，所以画面带宽
  再大也压不过摇杆命令的 socket。采集在核 1、写在核 0，两者之间只经 fb 队列传指针。
- **BLE 维护通道**（`CONFIG_S3_MAINT_BLE`，默认关）：BLE 控制器与 WiFi 必须同核（IDF 约束），
  `maint` 任务也留在核 0，本表不变。
- **PSRAM 争用**：八线 PSRAM 与 flash 共用 SPI 控制器。帧缓冲在 PSRAM，而
  `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` 又允许射频缓冲落 PSRAM。若实测"一开推流射频吞吐
  就明显下降"，第一个实验开关是把它设为 `n`，让射频缓冲留在内部 RAM（代价：内部 RAM 压力）。

## 4. 台架验证

- **开机 `task map`**：`CONFIG_S3_BENCH_CTRL` + `CONFIG_FREERTOS_USE_TRACE_FACILITY` 时
  `app_main` 末尾逐行打印 `name / core / prio / state / hwm`，把本表直接落到实物上核对。
  hwm 是 IDF 口径（剩余最小栈，字节）；低于 ~512 B 就加栈。
- **回归观测点**：手机一边看 :81 画面一边推摇杆，看 `/api/diag` 的 RTT/丢帧计数与
  `s3_link` 的 `TX stalled, re-armed` 是否上升。
- **待 HIL**：接 TC275 后实测推流下的 50 Hz 遥测 RTT 分布（tc275_car doc 22 §8 G5）。

## 5. 完成状态

| 项 | 状态 |
|---|---|
| 任务绑核（link / bridge / ota / led / rb_chk / 两个 httpd / dns / mdns / legacy） | 🟩 已实现 |
| `tcpip` 与 `cam_task` 的核配置 | 🟩 已实现 |
| 开机任务→核映射打印 | 🟩 已实现（台架门） |
| 外设 ISR 亲和搬到核 1 | ⬜ 刻意不做，见 §3 |
| 推流下 LINK RTT 分布实测 | 🟥 待 HIL |
| 射频缓冲是否退出 PSRAM | 🟨 待实测，见 §3 |
