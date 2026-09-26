# ESP32-C6 网络协处理器固件详细设计文档（LLDD）

| 项 | 内容 |
|---|---|
| 文档版本 | V1.0 |
| 日期 | 2026-09-26 |
| 上游依据 | [production-software-design.md](production-software-design.md) V1.1（下称 SDD）、[ux-performance-plan.md](ux-performance-plan.md) |
| 固件对象 | ESP32-C6（单 HP RISC-V 核 160 MHz + LP 核 20 MHz，512 KB HP SRAM，8 MB flash），ESP-IDF 5.4（与既有 esp-at 构建工具链同源） |
| 定位 | **网络协处理器**：softAP + Web 服务 + 配对 + 双板 OTA 中继/自身升级；**不解析运动控制语义，安全裁决全部在 TC275**（SDD §3.1） |
| 读者 | C6 固件工程师、TC275 固件工程师（LINK 接口）、硬件（引脚/分区）、产测（DPT 通道） |

---

## 1. 需求提取与追溯

从 SDD 提取的 C6 侧需求（编号供本文档内部追溯，映射表见附录 A）：

| FR | 需求 | SDD 出处 |
|---|---|---|
| FR-1 | softAP「SD-XXXXXX」（SN 派生 SSID，每机唯一密码）+ Captive Portal + mDNS `mycar.local` | §3.4/§8.1/§11.1 |
| FR-2 | WebSocket 服务器：1 控制端 + 2 观赛端；50 Hz 遥测广播；控制消息 <1 ms 进入 LINK | §3.2/§5/§14 |
| FR-3 | LINK 2 Mbps 全双工帧链路（协议 v2），链路健康监测与自动降速握手 | §5.6/§6/§3.7 |
| FR-4 | 配对：车侧按键开窗 → 首个请求客户端成为控制端；会话 token；30 s 断线宽限 | §8.2 |
| FR-5 | 断链即报：客户端全部离线/控制端断开 → LINK_STATE 帧毫秒级通知 TC275 | §4.1/§6.2 |
| FR-6 | 自身 OTA（A/B + 签名 + assets 同步更新 + 回滚）；TC275 OTA 中继泵（窗口流控） | §9 |
| FR-7 | 产测通道：BLE（或 Wi-Fi）DPT，命令 0x70–0x7F 透传 + 治具令牌；自身自检与 SN/密钥写入 | §10 |
| FR-8 | 安全：secure boot v2 + flash 加密 + NVS 加密 + JTAG 熔断（产线一次性）；WS 会话 token | §8 |
| FR-9 | 可靠性：任务看门狗、掉电保护、coredump 落 flash、堆水位遥测上报 | §7/§12 |
| FR-10 | 遗留兼容（可选）：TCP 8080 裸协议直通桥，demo 客户端零感知 | SDD §7 风险表 |

**明确不做（V1.0）**：TLS/HTTPS 服务端（局域网 + token 模型，互联网路线 B 时引入）、IPv6、云端连接、运动语义解析（急停判定、限速全部在 TC275）。

---

## 2. 总体设计

### 2.1 运行环境与内存地图

IDF 5.4 / FreeRTOS（单 HP 核调度）/ mbedTLS / NimBLE（可选编译）。8 MB flash 分区表（在 SDD §4.2 基础上补 NVS 加密所需的 `nvs_keys`）：

| 分区 | 类型/子类型 | 大小 | 用途 |
|---|---|---|---|
| nvs | data/nvs | 32 KB | 运行配置（配网、日志级别、会话 token 宽限） |
| nvs_cert（phy_init 合并） | data/nvs | 16 KB | RF 校准与证书预留 |
| **nvs_keys** | data/nvs_keys | 16 KB | NVS 加密密钥（NVS_ENCRYPTION 必需，SDD 未列，此处补齐） |
| otadata | data/ota | 8 KB | A/B 指针 |
| ota_0 / ota_1 | app/ota_0/1 | 各 3 MB | 固件 A/B（esp_ota_ops 标准 A/B） |
| assets | data/0x40 | 512 KB | gzip 控制页资源（随固件 bundle 更新） |
| coredump | data/coredump | 64 KB | 崩溃转储（网页 `/api/diag` 导出） |
| factory_ota_cache | data/0x40 | 1 MB | 路线 B 服务器下载暂存（V1.1，可缺省） |

合计 ≈7.8 MB ≤ 8 MB。HP SRAM 预算见 §7。

### 2.2 组件架构与代码树

```
esp32c6-fw/
├── main/
│   ├── app_main.c            # 启动编排（§4.1 状态机驱动）
│   └── app_state.c           # 全局应用状态机 BOOT→ONLINE(+SELF_OTA)
├── components/
│   ├── c6_proto/             # ★ 帧协议编解码 —— 与 TC275 mw/proto 同一源文件
│   │   └── proto_frames.c/.h #   (仓库根 common/proto/ 单一实现，双工具链编译)
│   ├── c6_net/               # softAP+STA · captive DNS · mDNS · 事件处理
│   ├── c6_http/              # httpd：静态 assets 服务 + WS 端点 + 会话表
│   ├── c6_pair/              # 配对窗口跟随 · 角色/会话 token 管理
│   ├── c6_link/              # UART1 驱动封装 · 帧装配 · 健康监测 · 波特率握手
│   ├── c6_bridge/            # 三台泵：命令泵 / 遥测广播器 / OTA 中继泵（核心）
│   ├── c6_ota/               # 自身 A/B + bundle 验签 + assets 更新 + 回滚
│   ├── c6_factory/           # NVS 模式定义 · 产线预配对 · 密钥读取
│   ├── c6_maint/             # BLE DPT 维护服务（C6_MAINT_BLE 编译开关，默认关）
│   └── c6_legacy/            # TCP 8080 直通桥（C6_LEGACY_TCP 开关，默认关）
└── assets_src/               # 控制页源码 → 构建期 gzip → assets.bin
```

**依赖规则**：`c6_proto` 不依赖任何组件（纯 C，无 IDF 头）；`c6_bridge` 是唯一同时接触 c6_http 与 c6_link 的组件（星型解耦）；禁止 c6_net/c6_http 之间直接调用。**帧编解码单一实现**是硬性规则——demo 版 "冒号截断" 这类两侧解析漂移缺陷（evaluation-report P1-2 同源）由共享源码 + 主机端单测结构性杜绝。

### 2.3 任务/上下文模型

| 任务 | 优先级 | 栈 | 职责 | WDT |
|---|---|---|---|---|
| `bridge_task` | 10 | 6 KB | 三台泵的所有状态机（§4.6），唯一写 LINK TX 队列与 WS 广播的上下文 | ✔ 5 s |
| `uart_evt_task`（IDF） | 12 | 4 KB | UART1 事件 → 帧装配入 `q_link_rx` | ✔ |
| `httpd` 任务（IDF） | 5 | 8 KB | HTTP/WS 端点回调；WS 收包 → `q_cmd`（**回调内不做耗时操作**） | — |
| `ota_task`（按需创建） | 8 | 6 KB | 自身 OTA 编排（验签在独立的低优先级片内执行） | ✔ |
| `esp_timer` 任务 | 系统高 | — | 20 ms 节拍：遥测广播 pacing、LINK 健康、PING | — |
| NimBLE host（可选） | 4 | 4 KB | DPT GATT 服务 | ✔ |
| tcpip/event 等系统任务 | 系统默认 | — | — | — |

WDT 订阅通过 `esp_task_wdt_add`；任一订阅任务超时 → panic → coredump → 复位（§4.10）。

### 2.4 队列与数据流规格

```
命令下行: WS(二进制=proto帧) ─► q_cmd(深度32, 不丢弃, 满则该WS背压)
                                ─bridge命令泵─► 鉴权/SEQ检查 ─► LINK TX(uart_write_bytes)
遥测上行: LINK RX ─► q_link_rx(16) ─bridge─► 遥测邮箱(单槽, 新覆旧)
                                ─20ms节拍─► 逐客户端广播(慢客户端合并降频, §4.3)
OTA 中继: HTTP 上传流 ─► 中继泵(8×512B 信用窗口) ─► LINK ─► TC275 ACK 推进(§4.6.3)
```

C6 内部时延预算：WS 收包 → LINK TX 出线 ≤ 1 ms（队列切换 + 组帧）；LINK RX → WS 广播出线 ≤ 1 ms。端到端承诺仍由 SDD §5 全链路预算兜底（命令 ≤50 ms）。

---

## 3. 对外接口规格

### 3.1 对 TC275（LINK）

帧格式、命令表、遥测表、错误码全部引用 SDD §6，本文档只补充 C6 侧的**使用纪律**：

| 帧 | C6 行为 |
|---|---|
| 0x41 TELEMETRY | 50 Hz 收入遥测邮箱；**解析 linkRtt 字段**（对应 PING 往返）供页面显示；不缓存历史 |
| 0x42 LINK_STATE | 只由 C6 发出：客户端集合变化即发（0=无客户端/1=仅观赛/2=控制端在线），事件驱动、毫秒级 |
| 0x43 PING/PONG | C6 每 100 ms 发 PING；500 ms 无任何下行帧 → LINK DOWN → 状态机复位 + 重连握手循环 |
| 0x44 BAUD_REQ/ACK | 上电 921600 自检 30 s 无误码 → 提议 2 Mbps；连续 10 帧 CRC 错 → 自动回降一档（921600↔2M 两档制） |
| 0x51 PAIR_* | 窗口/授权的执行者是 TC275，C6 只透传并维护会话侧状态（§4.4） |
| 0x60–0x6F OTA | 中继泵的信用窗口由 0x62 ACK/0x63 STATUS 驱动（§4.6.3） |
| 0x70–0x7F DPT | 仅当 DPT 会话持有效治具令牌时透传；C6 自身 DPT 项（版本/自检/NVS 写）本地处理 |

**假设 A1（接受的风险）**：LINK 为机内物理介质，V1.0 不做帧级 HMAC；威胁模型若引入"外壳被打开"场景，在 0x50 系列扩展 MAC（预留，见 SDD §7 备注）。

### 3.2 对手机（HTTP + WebSocket）

| 端点 | 方法 | 说明 |
|---|---|---|
| `/`、`/app.js`、`/…gz` | GET | assets 分区 gzip 资源，`Content-Encoding: gzip` + `Cache-Control: immutable`（URL 带 fwVer） |
| `/ws` | GET 升级 | 升级时校验 `?token=`（控制端恢复）或无 token（观赛端）；升级后二进制帧 = proto v2 帧，文本帧 = 控制面 JSON（HELLO/CLAIM/PAIR 状态） |
| `/api/pair` | POST | 触发 PAIR_REQ（仅窗口内成功） |
| `/api/diag` | GET | JSON 诊断导出：版本、堆水位、LINK 误码率、活动错误、coredump 存在标志 |
| `/ota/c6` | POST | 自身 bundle 上传（控制端 token 必须） |
| `/ota/tc275` | POST | TC275 固件上传 → 启动中继泵 |
| `/api/health` | GET | 存活探针（产测/运维） |

WS 控制面 JSON 仅用于会话协商（~100 B 级、低频）；**一切驾驶数据走二进制 proto 帧**，TC275 永远只见 proto 帧。

### 3.3 内部组件 API（关键签名）

```c
/* c6_link */
esp_err_t link_send(const proto_frame_t *f);            /* 入 TX 队列, 满返回 BUSY */
void      link_on_frame(link_rx_cb_t cb);               /* 注册到 bridge */
link_health_t link_health(void);                        /* rtt/误码率/波特率/状态 */
esp_err_t link_request_baud(uint32_t baud);             /* 0x44 握手 */

/* c6_http → bridge */
esp_err_t ws_broadcast_binary(const proto_frame_t *f);  /* 遥测: 逐客户端合并降频 */
esp_err_t ws_send_ctl(int sd, const char *json);        /* 控制面文本 */
void      ws_on_binary(ws_bin_cb_t cb);                 /* 命令入口 */
int       ws_controller_sd(void);                       /* -1 = 无控制端 */

/* c6_pair */
pair_state_t pair_state(void);                          /* IDLE/OPEN/CLAIMED */
esp_err_t pair_request(int sd, pair_result_t *out);     /* 0x51 透传 + 会话绑定 */

/* c6_ota */
esp_err_t ota_relay_begin(size_t total, uint32_t crc);  /* → LINK 0x60 */
esp_err_t ota_relay_feed(const uint8_t *chunk, size_t n); /* 信用窗口内 */
ota_self_state_t ota_self_feed(const uint8_t *p, size_t n); /* bundle 流式验签+写分区 */
```

---

## 4. 组件详细设计

### 4.1 app_state —— 应用状态机

```
BOOT ──NVS/分区自检──► PROVISION? ──出厂资料缺失──► FACTORY_WAIT(仅DPT可写SN/密钥)
                         │齐全
                         ▼
                     NET_START ──AP起 + Portal + mDNS──► ONLINE ◄─┐
                         │                │                       │ SELF_OTA(下载/验签/切换)
                         │ LINK DOWN      ▼                       │
                         │            LINK_UP(BAUD握手→UP) ───────┤
                         ▼                │                       ▼
                    (循环重试, 指数退避)   └────────────────► REBOOT_PENDING(自身OTA末段)
```

- **BOOT 自检**：分区表校验、ota_0/1 有效性（`esp_ota_get_state_partition`）、rollback 状态处理（PENDING_VERIFY → 起网络前先跑 §4.10 的最小自检集，通过则 `esp_ota_mark_app_valid_cancel_rollback`）；
- **PROVISION 检查**：c6_factory 读 SN/Wi-Fi 密钥，缺失 → 停在 FACTORY_WAIT：仅 BLE/Wi-Fi DPT 通道开放（产线写入后才进 ONLINE）；
- 与 TC275 的联动：进 ONLINE 后发 LINK_STATE；LINK 恢复握手期间照常起网（Web 可连但页面显示"车端未连接"）。

### 4.2 c6_net —— 接入网

- **softAP**：SSID=`SD-` + SN 后 6 位；密码 = 出厂 12+ 位随机（NVS，标签印刷）；信道 1/6/11 由 NVS 配置（默认 6）；`max_connection=4`（1 控制 + 2 观赛 + 1 产测余量）；
- **Captive Portal**：captive DNS（UDP 53 野答 → 本机 IP）+ HTTP 302（对非本机 Host 头的请求重定向 `/`），触发 iOS/Android 连接弹窗；
- **mDNS**：`mycar.local`（A 记录 + `_http._tcp`）；
- **STA（路线 B 预留）**：NVS 存 uplink 凭据，`esp_wifi_set_mode(WIFI_MODE_APSTA)` 双模；联网成功后开放 OTA 服务器拉取（V1.1，本版本仅保留模式与凭据字段）；
- 事件处理：AP STA 连/断计数 → 通知 bridge 发 LINK_STATE；所有 Wi-Fi 事件回调内**只投递消息，不做业务**（IDF 事件任务约束）。

### 4.3 c6_http —— Web 服务与会话

- **httpd 配置**：`CONFIG_HTTPD_WS_SUPPORT=y`；max open sockets 8；URI 匹配表（§3.2）；静态资源 handler 直接从 assets 分区 `esp_partition_read` 流式发送（512 KB 不占堆，按 4 KB 块读发）；
- **WS 会话表**（最多 4 条）：`{sd, role: CTRL|SPECTATOR|UNAUTH, token_hash, last_seq, slow_count}`；
  - 角色：升级时带有效 token → CTRL（同时只允许 1 个，重复带 token 的旧连接宽限 §4.4）；无 token → SPECTATOR（二进制命令帧被拒，返回错误 JSON）；
  - **遥测广播 pacing**：20 ms 节拍从邮箱取最新帧，逐客户端 `httpd_ws_send_frame_async`（配合 `httpd_sess_update_lru_sender`）；发送失败/EWOULDBLOCK → `slow_count++`，≥3 则该客户端降为每 4 拍一帧（12.5 Hz），仍失败 → 仅保活不推遥测。**观测端变慢不影响控制端**；
  - 命令入口：二进制帧 → 校验 `role==CTRL` 且 `seq` 单调（防重放，C6 侧第一道；最终裁决在 TC275 E2E）→ `q_cmd`；
- 长任务约束：OTA 上传 POST handler 允许流式接收（分块回调），但验签挪到 `ota_task`。

### 4.4 c6_pair —— 配对与角色（与 TC275 职责切分）

**职责切分**：TC275 = 授权权威（按键开窗、窗口计时、PAIR_CONFIRM 裁决）；C6 = 会话执行者（socket↔角色绑定、token 签发与宽限）。

```
手机                C6(c6_pair)                    TC275
 │ 升级WS(无token)     │                             │
 │◄─{role:spectator}  │                             │
 │                    │◄────0x51 PAIR_NOTIFY{窗口60s}──│ 用户按车键3s
 │ POST /api/pair     │                             │
 │──────►│ 0x51 PAIR_REQ{sd} ──────────────────────►│ 窗口内→CONFIRM{ok}
 │◄─{role:ctrl,token}  │ 签发sessionToken(32B随机)    │
 │       │ NVS存{token_hash, 过期=now+30s宽限}        │
```

规则：窗口外 PAIR_REQ → TC275 拒绝，C6 透传拒绝原因；已有 CTRL 时新 PAIR_REQ → 除非旧 CTRL 断开超 30 s（宽限过期），否则拒绝；**token 只在内存/NVS 存 hash**；控制端断线 → 立即发 LINK_STATE=0（而非等 30 s）——停车语义归 TC275 心跳/断链逻辑，C6 的 30 s 宽限只影响"免按键重连"，二者解耦。

### 4.5 c6_link —— LINK 链路

- **UART1**：候选引脚 GPIO10(TX)/GPIO11(RX)（避开 strap GPIO4/5/8/9/15 与 USB GPIO12/13；EE 引脚矩阵评审锁定）；RX 环 4 KB + 事件队列；TX 环 2 KB；波特率状态机：`921600 → (30 s 零误码) → 提议 2M(0x44) → 确认切换`；误码率滑窗 >1‰ → 回降 921600 并遥测上报；
- **帧装配**：调用共享 `c6_proto` 的逐字节状态机（AA55/VER/CMD/SEQ/LEN/CRC16），畸形帧计数进 `link_health`；
- **健康监测**：20 ms 节拍：发 PING；任何下行帧刷新时间戳；500 ms 静默 → DOWN 事件 → bridge 发 LINK_STATE 前先本地标记"车端未连接"（页面状态灯）并循环重握手（100 ms→2 s 指数退避）；
- **TX 满策略**：命令帧 BUSY → 向源 WS 回错误（不静默丢弃）；遥测类可丢（TC275 20 ms 后自然重发）。

### 4.6 c6_bridge —— 三台泵（核心组件）

#### 4.6.1 命令泵
`q_cmd` → 逐帧：role/seq 校验 → `link_send` → 失败回 WS 错误 JSON。单帧处理 ≤50 µs（无阻塞调用）。

#### 4.6.2 遥测广播器
`q_link_rx` 中 0x41 → 写遥测邮箱（覆盖式）+ 刷新 LINK 健康；20 ms 节拍按 §4.3 pacing 广播；同时把 state/faultCode 抽出维护页面状态缓存（供新连接的 WS 首屏）。

#### 4.6.3 OTA 中继泵（TC275 固件）

**信用窗口流控**——上传速率受 TC275 PFlash 擦写限制，必须显式背压，防止 512 KB 堆被上传流吃光：

```
POST /ota/tc275 ─► 0x60 BEGIN{total, crc} ──► TC275 擦 PFlash1 扇区(耗时主源)
手机流 ──► 窗口(8×512B 在途) ── 0x61 CHUNK{k} ──► TC275 编程+回读校验
                 ▲                                │
                 └──── 0x62 ACK{k}(信用+1) ◄──────┘  窗口耗尽→暂停读HTTP
全部收完+ACK ─► 0x63 END ─► TC275 整槽CRC+签名 ─► 0x64 SWAP_REQ → TC275 复位切槽
全程: 0x63 STATUS 进度帧 ─► C6 广播给手机(进度条), 并写入遥测扩展字段
```

超时与断点：任一 ACK 2 s 未到 → 重发该 CHUNK 1 次 → 仍失败 → 中止并上报 `ERR_OTA_RELAY`；手机断开 → 中继泵终止 + 0x65 ABORT → TC275 丢弃半槽（SBL 回滚机制兜底）。总时长目标与 SDD §2.2 一致（≤30 s 量级，擦写实测校准；页面进度条不设硬超时）。

### 4.7 c6_ota —— 自身升级

- **Bundle 格式**（`POST /ota/c6`，multipart 流式接收）：

```
magic "C6FW" | version | c6.bin(len,sha256) | assets.bin(len,sha256) | ed25519(sig over 前述摘要)
```

- **流程**：流式解析 → ed25519 验签（移植公开域参考实现，仅验签，~100–300 ms，在 ota_task 执行）→ `esp_ota_begin/write/end` 写非活动 app 槽 → assets.bin 整体写 assets 分区（带本地 CRC）→ `esp_ota_set_boot_partition` → 回复手机 → 5 s 后自重启；
- **回滚**：`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`：新镜像首启为 PENDING_VERIFY → §4.10 最小自检集 + LINK 握手成功 → `mark_app_valid`；否则下次复位引导器自动回滚旧槽，页面经 `/api/health` 可见当前槽与版本；
- TC275 侧签名公钥与 C6 侧同源管理（`common/keys/pub_ed25519.bin` 构建期嵌入），见开放问题 Q2。

### 4.8 c6_factory —— 出厂数据（NVS）

| 键 | 类型 | 写入者 |
|---|---|---|
| `sn` | str(16) | 产线 DPT |
| `wifi_pass` | str(≥12 随机) | 产线 DPT（与标签一致） |
| `pair_salt` | bytes(16) | 产线 DPT |
| `uplink_ssid/pass` | str | 用户（路线 B 预留） |
| `ch` / `log_level` | u8 | 运维 |

读写仅经 `c6_factory` 封装（nvs 加密命名空间）；产线批量预配对：DPT 写入"预授权 token 表"（治具生成的首批控制端凭据），用户开箱免按键。

### 4.9 c6_maint —— BLE DPT 通道（编译开关 `C6_MAINT_BLE`，默认关）

NimBLE 自定义 GATT：Service `0xFDxx`（产测专用 UUID），Char：`CMD`（写，20–512 B JSON/proto 帧）、`IND`（通知，响应/日志）。进入 DPT 需治具令牌（写 CMD 首帧校验，令牌由 MES 下发）。命令集映射 SDD §10.2：版本/自检/SN 写/预配对/老化启停；运动类 0x7x 直接透传 LINK。无 BLE 构建（消费版）时 DPT 走 Wi-Fi 专用 SSID `SD-xxxx-DPT` + 令牌。

### 4.10 可靠性与自检

- **最小自检集**（rollback 判定与 BOOT 复用）：分区表/NVS 可读、LINK 三次握手、heap 水位 >64 KB、TWDT 全订阅；
- **TWDT** 5 s；**中断 WDT** 默认开；panic 策略 `PRINT_REBOOT` + coredump 落 flash；
- **brownout**：默认阈值；复位原因经 `esp_reset_reason` 读取，写入 `/api/diag` 并随遥测扩展字段上送 TC275 黑匣子（SDD §8.4 联动）；
- **堆守护**：`heap_caps_get_minimum_free` 每 10 s 采样，<64 KB → 主动重启前广播诊断（防慢性泄漏演变为硬故障）；
- LP 核 V1.0 不使用（预留：深度睡眠期间 UART 唤醒/看门狗，见开放问题 Q5）。

---

## 5. 关键时序（汇总）

§4.4 配对时序之外，其余核心时序已在正文给出：命令路径（§2.4/§4.6.1）、遥测广播（§4.3/§4.6.2）、OTA 中继窗口流控（§4.6.3）、自身 OTA（§4.7）。全部时序的端到端指标以 SDD §5 预算表为准，C6 内部承诺 ≤1 ms/跳。

---

## 6. 资源预算

| 资源 | 预算 | 备注 |
|---|---|---|
| HP SRAM | Wi-Fi ~70 KB + httpd/WS 4 客户端 ~45 KB + 任务栈 ~40 KB + 队列/缓冲 ~15 KB + NimBLE(可选) ~35 KB ≈ **205 KB**，余量 ≥200 KB | 每 10 s 水位采样（§4.10） |
| CPU（稳态） | Wi-Fi 底噪 ~5% + bridge/广播 50 Hz <5% ≈ **<10%** | |
| CPU（OTA 峰值） | 上传流 + 2 Mbps UART 泵 + 广播 ≈ **35–40%**， <50% 预算线 | TC275 擦写是瓶颈方 |
| 时延 | WS→LINK ≤1 ms；LINK→WS ≤1 ms | 兜底于 SDD 全链路 ≤50 ms |
| flash | app ~1.2 MB ×2 + assets 0.5 MB + coredump 0.06 MB，8 MB 余量 ~3.5 MB | |

## 7. 安全实现（SDD §8 的 C6 侧落地）

| 项 | 实现 | 时机 |
|---|---|---|
| secure boot v2 | RSA-PSS 镜像签名，公钥摘要烧 efuse | 产线一次性 |
| flash 加密 | AES-XTS，Release 模式（密钥不可回读） | 产线一次性 |
| NVS 加密 | `nvs_keys` 分区 + ENCRYPTION=y | 随 flash 加密 |
| JTAG 熔断 | eFuse DIS_USB_JTAG / DIS_LEGACY_SPI_BOOT 等 | 产线一次性 |
| 会话 token | 32 B 随机，仅存 hash；WS 升级时校验 | 运行期 |
| 防重放 | 控制帧 seq 单调（C6 第一道）+ TC275 E2E（最终裁决） | 运行期 |
| 调试口 | UART0 日志保留（运维），运行期日志级别默认 WARN | NVS 可配 |

产线一次性步骤（provisioning）编入 DPT 序列：烧 efuse 之前完成 SN/密钥写入与预配对表——**顺序不可颠倒**（efuse 熔断后 NVS 加密命名空间只可通过运行固件写入）。

## 8. 构建与版本

- `sdkconfig.defaults` 关键项：`HTTPD_WS_SUPPORT=y`、`BOOTLOADER_APP_ROLLBACK_ENABLE=y`、`NVS_ENCRYPTION=y`、`ESP_COREDUMP_ENABLE_TO_FLASH=y`、`LWIP_MAX_ACTIVE_SOCKETS=16`、`FREERTOS_HZ=1000`、`ESP_TASK_WDT_TIMEOUT_S=5`、BT 仅随 `C6_MAINT_BLE`；
- 版本号 = SDD §9.1 规范；`fwVer` 经遥测上报、页面展示，assets URL 带 fwVer 防陈旧缓存；
- CI 产物：`c6-fw-{ver}.tar.gz`（c6.bin + assets.bin + manifest + sig）+ 主机单测报告；与 TC275 固件包组合为整机发布包（SDD §9.3 路线 A）。

## 9. 测试设计

| 层 | 内容 | 通过标准 |
|---|---|---|
| 主机单测 | `c6_proto`（与 TC275 共用同一份测试集，模糊测试 10⁷ 随机帧）、bundle 解析器、会话表状态机 | 行覆盖 ≥90%（proto 100% 分支） |
| 目标单测/集成 | c6_link 帧装配与健康监测、c6_pair 窗口/宽限、遥测降频策略 | IDF pytest-embedded 用例集 |
| HIL 联调 | 与 TC275 台架：LINK 波特率握手/降速、断链 LINK_STATE 时序（拔线 ≤10 ms 内发出）、OTA 中继全流程 + 每阶段断电注入 | SDD §8 验收用例 |
| 压力 | 4 客户端并发 + 慢客户端 + WS 洪泛 + 上传中反复断连 | 控制端时延不劣化；无堆耗尽 |
| 老化 | 72 h 连续遥测 + 周期配对/断开 + 每 30 min 一次 OTA 中继 | 0 泄漏（水位不降）、0 复位 |

## 10. 开放问题与决策记录

| # | 事项 | 状态/倾向 |
|---|---|---|
| Q1 | LINK 引脚锁定（候选 GPIO10/11） | 待 EE 引脚矩阵评审 |
| Q2 | OTA 签名算法：SDD 定 ed25519；IDF mbedTLS 无内建支持，采用移植参考实现（仅验签）。若产线工具链更适配 ECDSA P-256，需与 TC275 侧同步改并升版 SDD | 待定，默认 ed25519 |
| Q3 | 消费版是否带 BLE（DPT 走 Wi-Fi 专用 SSID 可省 NimBLE 35 KB） | 待产品定义 |
| Q4 | TCP 8080 遗留桥保留几个版本 | 建议仅量产首批固件开启 |
| Q5 | LP 核启用（深睡唤醒/低功耗待机） | V1.1 起评估，涉及整机电源架构 |
| D1 | 遥测广播采用"最新值邮箱 + 慢客户端降频"而非逐客户端队列——观测端慢不得拖累控制端 | 已决策 |
| D2 | 帧编解码单一共享源码（common/proto/，双工具链编译 + 共享单测） | 已决策，硬性规则 |
| D3 | 配对授权在 TC275、会话执行在 C6 的职责切分 | 已决策 |

## 附录 A：需求追溯矩阵（摘要）

| FR（§1） | 落点 |
|---|---|
| FR-1 接入网 | §4.2 |
| FR-2 WS/遥测 | §3.2/§4.3/§4.6.2 |
| FR-3 LINK | §3.1/§4.5 |
| FR-4 配对 | §4.4 |
| FR-5 断链即报 | §4.2 事件/§4.4/§4.5 |
| FR-6 OTA | §4.6.3/§4.7 |
| FR-7 产测 | §3.2/§4.8/§4.9 |
| FR-8 安全 | §3.2/§4.4/§7 |
| FR-9 可靠性 | §2.3/§4.10 |
| FR-10 遗留桥 | c6_legacy（开关，规格随 Q4 定稿） |

> 本文档为 C6 固件的实现基线；接口签名以代码头文件为准，架构级变更需回写 SDD 并同步升版。
