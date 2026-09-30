# 08 · 架构 v2：双核规划、固件分区、升级 / 标定 / 诊断

> 状态：已实施（2026-09-30）。本文是实现的依据；代码与本文不一致时以本文为准修正代码。
> 接口事实来自 c6_car：`doc/07-http.md`、`doc/08-bridge.md`、`doc/09-ota.md`、`doc/17-calib-ui.md`、
> `components/c6_http/http_server.c`、`components/c6_bridge/bridge.c`。

## 1. 目标与约束

| 目标 | 约束 |
|---|---|
| 驾驶控制 30 Hz 永不抖动；UI 流畅无割裂 | RGB bounce-buffer 送帧对 PSRAM 带宽与 core 1 调度极敏感（R-6/R-10） |
| S3 可给 C6、TC275 做固件升级 | C6 只接受签名 `C6FW` bundle（≤3 MiB）；TC275 raw 镜像（≤3 MiB），均需 CTRL token |
| TC275 标定界面 | 只用已实现的 DPT 0x70–0x74；0x75–0x79 禁止发送 |
| C6 / TC275 故障诊断界面 | C6 走 `GET /api/diag`；TC275 走遥测 0x41 + 事件 |
| 不改变安全语义 | 失联停车、STOP/急停锁存、摇杆 CTRL 门控保持不变 |

## 2. 双核规划

原则：**core 1 = 实时人机核**（只放确定性、短时的任务），**core 0 = 通信 / 服务核**（所有可能阻塞在网络或 flash 上的工作）。
任何会等网络、等 flash、做大块 memcpy 的代码都不允许上 core 1。

| 任务 | 核 | 优先级 | 周期 / 触发 | 说明 |
|---|---|---|---|---|
| `scr_ctrl` 控制 | 1 | 6 | 30 Hz 定时 | DRIVE / 心跳 / 点动 0x71；只做内存运算 + 非阻塞 WS 发送（锁等待 ≤5 ms） |
| `taskLVGL` UI | 1 | 4 | 事件 + 10 Hz 刷新 | 只读状态快照，不做 I/O |
| Wi-Fi 驱动 | 0 | 23 | IDF | `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0` |
| lwIP `tcpip` | 0 | 18 | IDF | **改为绑定 core 0**（原 no-affinity，会漂到 core 1 抢 LVGL） |
| `websocket_task` | 0 | 5 | 事件 | **显式 `task_core_id=0`**（原不绑核） |
| `scr_link` 监控 | 0 | 4 | 50 ms | Wi-Fi/WS 重连、1 Hz ping、配对 |
| `scr_svc` 服务 | 0 | 3 | 队列驱动 | **新增**：OTA 流式上传、`/api/diag` 轮询、标定会话；可长时间阻塞 |
| esp_timer / main | 0 | — | IDF | 默认 |

核 1 上只剩 ctrl（6）> LVGL（4），ctrl 每 33 ms 运行 <1 ms，LVGL 获得其余全部时间 → UI 流畅；
网络抖动、flash 读、HTTP 上传全部在 core 0，不影响送帧与控制节拍。

### 2.1 控制延时路径（对标手机 Web 页）

| 环节 | 旧 | 新 |
|---|---|---|
| 摇杆 → 发送 | 仅 30 Hz 定时，最坏等 33 ms | 摇杆值变化即 `scr_ctrl_kick()` 唤醒 ctrl，立即发送（≥20 ms 合并，≤50 帧/s）；定时 30 Hz 仍作心跳 |
| S3 TCP | Nagle 开：每帧等上一帧 ACK，C6 端 lwIP 延迟 ACK → 100–200 ms | WS 连上即对到 C6 的套接字设 `TCP_NODELAY`（浏览器默认即如此） |
| C6 TCP | Nagle 开：遥测 / pong 回程同样被攒包 | c6_car `ws_tighten_send_timeout()` 同时设 `TCP_NODELAY` |
| WS 发送锁 / 写超时 | 0 超时，冲突即丢帧 | 150 ms：该超时同时用于 socket 写，写不完客户端会**直接断开连接**；5 ms 短于一次 Wi-Fi 重传，实测每几分钟一次 RADIO LOST。发送前再用零等待 `select()` 判可写，不可写就跳过本帧（下一帧 ≤33 ms 带新值） |
| 事件驱动上限 | — | ≥20 ms 合并（≤50 帧/s），避免空口拥塞 |

### 2.2 内存放置

LVGL 堆改为 **PSRAM 优先**（`main/lv_mem_psram.c`，`LV_USE_CUSTOM_MALLOC`）。C 库分配器会让每个控件先占内部 RAM，
十余个页面把内部 RAM 耗到 0，`esp_wifi_init()` 报 `ESP_ERR_NO_MEM` → abort → 重启循环（表现为不断闪屏）。
内部 RAM 专留给 Wi-Fi / lwIP / DMA / 任务栈；启动日志打印 `heap after init`，内部可用需 ≥ 96 KB。
初始化顺序：显示 → 通信 / 控制 / 服务任务 → UI 构建。

## 3. Flash 分区（16 MB）

| 名称 | 类型 / 子类型 | 偏移 | 大小 | 用途 |
|---|---|---|---|---|
| nvs | data/nvs | 0x9000 | 24 K | 设置、token |
| phy_init | data/phy | 0xF000 | 4 K | |
| factory | app/factory | 0x10000 | 6 M | S3 应用（不变） |
| coredump | data/coredump | 0x610000 | 64 K | 不变 |
| **fw_c6** | data/0x40 | 0x620000 | 3136 K (0x310000) | C6 固件暂存区：4 K 头 + ≤3 MiB `C6FW` bundle |
| **fw_tc** | data/0x41 | 0x930000 | 3136 K (0x310000) | TC275 固件暂存区：4 K 头 + ≤3 MiB raw 镜像 |
| 预留 | — | 0xC40000 | 3.75 M | 后续资源 / 日志 |

现有分区偏移不动 → 已写入的 NVS / 应用不受影响（需重刷分区表一次）。

### 3.1 暂存区头格式（sector 0，4096 B，LE）

| 偏移 | 字段 | 说明 |
|---|---|---|
| 0 | `magic[4]` = `"SCFW"` | |
| 4 | `hdr_ver` u16 = 1 | |
| 6 | `target` u8 | 1 = C6，2 = TC275（必须与分区匹配） |
| 7 | 保留 u8 | |
| 8 | `size` u32 | 载荷字节数（1 … 3 MiB） |
| 12 | `crc32` u32 | 载荷 CRC32（IEEE，`esp_rom_crc32_le(0, …)`） |
| 16 | `version[32]` | 显示用版本串，NUL 结尾 |
| 48 | `built[32]` | 暂存时间串 |
| 4096 | 载荷 | 原样 `C6FW` bundle / TC275 镜像 |

写入工具：`tools/stage_fw.py --target c6|tc275 --image <bin> --version <str> --port <S3 串口>`，
生成头 + 载荷后用 `esptool write-flash <分区偏移>` 写入。S3 升级前**完整重算 CRC32**，不匹配拒绝发送。

## 4. 固件升级流程（S3 → C6 / TC275）

1. 固件升级页读取两个暂存区头，显示版本 / 大小 / 校验状态。
2. 前置条件（S3 自己强制，C6 不检查）：WS 已连接且为 CTRL、有配对 token、车速 0、非急停锁存；TC275 还要求 `tc` 在线。
3. 用户**长按 2 s** 「START UPDATE」确认（防误触）。
4. `scr_svc`（core 0）：STOP 锁存（DRIVE 持续发 0/0 心跳）→ 校验 CRC →
   `esp_http_client` `POST http://<C6>/ota/c6|/ota/tc275`，`X-Session-Token: <token>`，`Content-Length: size`，
   从分区 4 KB 分块流式读写；显示发送进度。
5. 结果：
   - C6：200 `{"ok":true,"reboot_s":5}` → 提示 C6 5 s 后重启，WS 会断开并自动重连，重连后 hello `ver` 为新版本。
   - TC275：200 `{"ok":true,"acked":N,...}`；过程中 WS `{"t":"otastatus","state","pct"}` 显示 TC275 写入进度，`otaswap` 表示切换，`otaerror` 表示失败。
   - 401 未配对、413 过大、503 链路未就绪、502 / 400 传输失败 → 明文显示原因与处理建议。
6. 升级中禁止再次触发；离开页面不中断（任务在后台完成，结果写入事件日志）。

## 5. TC275 标定页

入口：设置 → 07 CALIBRATE。所有命令经 WS 二进制 v2 帧，需 CTRL。

| 功能 | 命令 | 交互 |
|---|---|---|
| 读取标定记录 | 0x72 REC_GET → `{"t":"rec"}` | 进入页面自动读取；显示 src / pos / invert / fullScale / wheelDia / crcOk |
| 编码器判向 | 0x70 CAL_DIR → `{"t":"cal"}` | 需勾选「四轮离地」+ 长按 1.5 s；3 s 超时提示查看串口 `ENCCAL=`；显示 4 轮 invert / delta / saved |
| 电机点动 | 0x71 `{motor, duty}` | 4 个电机 × 正/反，按住 30 Hz 由 ctrl 任务发送 ±300（30%），松开发 duty 0；TC275 自带 300 ms 自停 |
| 写入参数 | 0x73 REC_SET（12 B） | fullScale 100–5000、wheelDia 30–200 步进调整后写入；回执 `rec` |
| 恢复默认 | 0x74 REC_CLEAR | 长按确认 |

安全：页面打开期间摇杆不可见 → DRIVE 恒 0/0；未勾选离地、非 CTRL、`tc` 离线时所有动作按钮禁用并显示原因。

## 6. 故障诊断页（C6 + TC275）

入口：设置 → 08 DIAGNOSE。页面可见时 `scr_svc` 每 2 s `GET /api/diag`（超时 1.5 s）；TC275 数据来自 20 ms 遥测。

**健康结论区**：自动推导的故障条目（严重度颜色 + 文字 + 建议）：

| 条件 | 等级 | 条目 |
|---|---|---|
| `/api/diag` 取不到 | 严重 | C6 无响应（检查供电 / Wi-Fi） |
| C6 `reset` ∈ panic / wdt / brownout | 警告 | C6 上次异常复位：<原因> |
| C6 `coredump` = true | 警告 | C6 存有崩溃转储 |
| `link.up` = false | 严重 | C6↔TC275 SPI 链路断开 |
| `crc_err` / `fmt_err` 2 s 内增长 | 警告 | SPI 链路误码增长 |
| `heap_min` < 64 KB | 警告 | C6 内存余量低 |
| 遥测失效 | 严重 | TC275 无遥测 |
| `fault_code` ≠ 0 | 严重 | TC275 故障码 0x…（码表在 myCar 仓库） |
| 电池 ≤ 低阈值 | 警告 | 电池电量低 |
| 目标 / 实测速度偏差持续 > 200 mm/s | 警告 | 左 / 右侧速度跟踪偏差大（检查电机 / 编码器，可去标定页） |
| 无条目 | 正常 | ALL SYSTEMS NOMINAL |

**明细区**：C6（版本、状态、分区、复位原因、自检、heap_min、uptime、客户端数、IMU）；
链路（up、RTT、CRC / FMT 错误、rx / tx、busy）；TC275（状态码、故障码、固件 / 硬件版本、电压 / 电量、左右目标 / 实测速度、里程、链路 RTT / 误码率）。

**操作**：REFRESH（立即轮询）、CLEAR FAULT（发送 0x31，TC275 可能忽略，页面提示以遥测为准）。

## 7. UI 区域划分

设置主页（COMMAND DECK）模块：01 CONTROL · 02 RADIO · 03 DISPLAY · 04 PAIRING · 05 ABOUT ·
**06 FIRMWARE · 07 CALIBRATE · 08 DIAGNOSE**（2 列 × 4 行）；工程模式追加 DIAG / EVENTS（可滚动）。
三个新页面均为独立页（`UI_PAGE_FW / UI_PAGE_CALIB / UI_PAGE_FDIAG`），HUD 标题栏返回设置。

## 8. 验证

- 构建通过；双板串口同时抓日志：WS 稳定、无 panic。
- 任务分布：`vTaskGetInfo` 不可用时以启动日志核号为准（ws / svc 绑 core 0）。
- 固件升级：无暂存镜像 → 页面显示 EMPTY 且按钮禁用；暂存后 CRC 校验通过才允许发送。
- 诊断页：拔 TC275 / 断 C6 时对应条目出现。
