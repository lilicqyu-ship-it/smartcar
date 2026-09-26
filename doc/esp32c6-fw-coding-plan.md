# ESP32-C6 固件编码计划（Coding Plan）

| 项 | 内容 |
|---|---|
| 文档版本 | V1.0 |
| 日期 | 2026-09-26 |
| 上游依据 | [esp32c6-fw-design.md](esp32c6-fw-design.md) V1.0（下称 LLDD）、[production-software-design.md](production-software-design.md) V1.1（下称 SDD） |
| 工程对象 | `AURIX-v1.10.36-workspace/c6_car`（ESP-IDF 工程，目标板 ESP32-C6，8 MB flash） |
| 构建环境 | ESP-IDF v6.1-beta1（`C:\esp\v6.1-beta1\esp-idf`）+ Windows 工具链 `C:\Espressif\tools`（riscv32-esp-elf gcc 15.2 / Ninja） |
| 交付物 | c6_car 全量固件源码 + 分区表 + sdkconfig + assets 构建脚本 + 主机单测 + 本计划 |

---

## 1. 范围与裁剪决策（V1.0 编码基线）

按 LLDD 全范围实现，以下为编码期明确决策（与 LLDD 冲突处均已回写记录）：

| # | 决策 | 理由 / 说明 |
|---|---|---|
| C1 | **D2 单一实现落点**：proto v2 编解码单一源文件为 `c6_car/components/c6_proto/proto_frames.c/.h`（纯 C、零 IDF 头、主机可编译）。myCar 侧升级到 proto v2 时直接采用同一文件（copy + CI 侧 hash 比对），待两仓库合并后改为物理共享 | 物理上是两个独立工程，无法直接 include；先以"同一文件 + 哈希校验"达成单一实现 |
| C2 | **mDNS 自实现**：IDF v6.1 已移除内置 `mdns` 组件（转为 managed component，需构建期联网）。在 `c6_net` 内实现轻量响应器 `mdns_lite`（UDP 5353 组播，`mycar.local` A 记录 + `_http._tcp` PTR/SRV/TXT/A），约 250 行，无外部依赖 | 消除构建期网络依赖；行为覆盖 FR-1 的两条要求 |
| C3 | **OTA 签名算法**：按 LLDD Q2 默认 ed25519，移植"仅验签"公开域风格实现（自研 51-bit limb 域算术 + Edwards 扩展坐标点加，常量由 `tools/gen_crypto_consts.py` 用 Python 大数运算生成、构建前入库）。SHA-512 自实现（常量同法生成），不依赖 mbedTLS，主机/目标共用同一份代码。以 RFC 8032 测试向量 + 32 MB 随机帧模糊测试在主机端验证 | mbedTLS 3.x 无 Ed25519；自实现可被已知向量完全验证，满足"仅验签、可主机单测" |
| C4 | **0x63 语义合并**：LLDD §4.6.3 中 0x63 同时被写为 END 与 STATUS，编码取 **0x63 = OTA_STATUS**（含 state 字段：`RUNNING/DONE/FAILED`），END 语义由 `state==DONE` 承载；0x64 SWAP_REQ、0x65 ABORT 不变 | 消除 LLDD 内部帧号冲突；STATUS 进度帧与 END 帧本就同型 |
| C5 | **CRC16 规格**：CRC16-CCITT-FALSE（poly 0x1021，init 0xFFFF，MSB first，无异或输出，无反射），check 值 `"123456789"→0x29B1`。写死在 `proto_frames.h` 注释与主机单测 | SDD 只写"CRC16-CCITT"，编码期固化唯一规格；TC275 侧接入时以此为准 |
| C6 | **出厂资料缺失的调试行为**：`CONFIG_C6_FACTORY_DEV_OVERRIDE`（默认 y，量产必须关）在 NVS 无 SN/wifi_pass 时以 `SN=DEV000`/固定密码启动并高亮告警，便于台架点灯；关闭后严格按 LLDD 停 `FACTORY_WAIT` | 台架开发可开箱即用，量产语义不妥协 |
| C7 | **c6_maint / c6_legacy**：默认关闭（编译开关 `CONFIG_C6_MAINT_BLE` / `CONFIG_C6_LEGACY_TCP`）。代码完整实现：BLE DPT 走 NimBLE GATT（CMD 写 / IND 通知）；legacy 桥为 TCP 8080 ↔ LINK 的 v2 帧字节直通（仅放行合法 v2 帧） | LLDD FR-7/FR-10，Q3/Q4 未定稿前的安全缺省 |
| C8 | **TX 单上下文的落法**：`link_send()` 内部互斥锁 + 专用 `link_tx_task` 排空 TX 帧队列（深度 8，满返回 BUSY）。bridge / pair / link 健康定时器（PING）都只调用 `link_send()`，物理上仍是单一出线上下文 | LLDD API 注释"入 TX 队列，满返回 BUSY"的字面实现；避免 esp_timer 回调阻塞 |
| C9 | **bridge_task 用 QueueSet 聚合四路输入**：`q_cmd(32)`、`q_link_rx(16)`、`q_pair(8)`、`q_ota_chunk(8)` + `q_events`（LINK 上下行事件、客户端集合变化）+ 20 ms 节拍走 task notification。三台泵全部状态机在 bridge_task 内推进 | FreeRTOS 无多队列 select；QueueSet 是 LLDD §2.3"单上下文"模型的自然实现 |
| C10 | **assets 缺省页**：assets 分区为空/缺文件时，`/` 回退到固件内嵌的极简控制页（非 gzip，功能完整但无样式资源），并在 `/api/diag` 标记 `assets=embedded` | 保证空分区整机仍可配对驾驶；正式页面走 assets.bin 更新 |

**本版明确不做**（与 LLDD §1 一致）：TLS 服务端、IPv6、云端、路线 B 服务器拉取（仅保留 NVS 凭据字段）、LP 核、TCP legacy 的 v1 兼容翻译（Q4 未定稿，仅 v2 直通）。

---

## 2. 文件清单与职责（全部新增文件）

```
c6_car/
├── CMakeLists.txt                  # 不变（IDF 样板）
├── partitions.csv                  # 新增：LLDD §2.1 八分区布局（含 nvs_keys）
├── sdkconfig.defaults              # 新增：LLDD §8 关键项 + 分区表/8MB/WDT/coredump
├── README.md                       # 新增：构建/烧录/assets/测试 指南
├── main/
│   ├── CMakeLists.txt              # 挂全部组件
│   ├── Kconfig.projbuild           # C6_FACTORY_DEV_OVERRIDE 等项目级开关
│   ├── app_main.c                  # 启动编排（LLDD §4.1 状态机驱动）
│   └── app_state.c/.h              # 全局状态机 + 诊断聚合（复位原因/堆水位/自检）
├── components/
│   ├── c6_proto/                   # ★ 单一实现：proto v2 编解码（纯 C）
│   │   ├── proto_frames.h/.c
│   │   └── CMakeLists.txt
│   ├── c6_factory/                 # NVS 出厂数据读写唯一入口（加密命名空间）
│   │   └── factory.h/.c
│   ├── c6_link/                    # UART1 · 帧装配 · 健康监测 · 波特率握手 · TX 队列
│   │   ├── link.h/.c
│   │   └── Kconfig                 # LINK 引脚/波特率（Q1 引脚锁定前可配）
│   ├── c6_net/                     # softAP + captive DNS + mdns_lite + Wi-Fi 事件
│   │   ├── net.h/.c  captive_dns.h/.c  mdns_lite.h/.c
│   ├── c6_http/                    # httpd+WS · 会话表 · assets 分区流式服务 · REST 端点
│   │   ├── http_server.h/.c  ws_sessions.h/.c  assets_store.h/.c
│   ├── c6_pair/                    # 配对窗口跟随 · 会话 token 签发/宽限（§4.4）
│   │   └── pair.h/.c
│   ├── c6_bridge/                  # ★ 三台泵：命令泵/遥测广播器/OTA 中继泵
│   │   ├── bridge.h/.c  ota_relay.h/.c
│   ├── c6_ota/                     # 自身 A/B OTA · bundle 解析 · ed25519 验签 · sha512
│   │   ├── ota_self.h/.c  bundle.h/.c  ed25519v.h/.c  sha512.h/.c  c6_consts.h(生成)
│   ├── c6_maint/                   # BLE DPT（CONFIG_C6_MAINT_BLE，默认关）
│   │   └── maint_ble.h/.c
│   └── c6_legacy/                  # TCP 8080 直通桥（CONFIG_C6_LEGACY_TCP，默认关）
│       └── legacy_tcp.h/.c
├── assets_src/                     # 控制页源码（index.html + app.js，摇杆/仪表/配对/OTA）
├── tools/
│   ├── build_assets.py             # assets_src → build/assets.bin（gzip+目录+CRC32）
│   └── gen_crypto_consts.py        # 生成 c6_consts.h（已入库，构建不需 Python）
└── test/host/                      # 主机单测（MSYS2 gcc / 任意 C99 编译器）
    ├── Makefile  minunit.h
    ├── test_proto.c                # 编解码全分支 + CRC check 值 + 10^7 随机帧模糊
    ├── test_sha512.c               # Python hashlib 生成的多组已知摘要
    ├── test_ed25519.c              # RFC 8032 向量（含篡改必败）
    └── test_bundle.c               # bundle 头解析/防越界
```

代码量预估 ≈ 6.5k 行（含测试与页面）。实现顺序 = 依赖序：proto → factory → link → net → http → pair → bridge → ota → maint/legacy → main → 配置/资产/测试。

---

## 3. 关键设计落点（编码级）

### 3.1 proto v2 帧与常量（c6_proto，C1/C3/C4/C5）

- 帧格式：`AA 55 VER CMD SEQ LEN DATA[LEN] CRC16`，`VER=0x02`，`LEN ≤ 64`，CRC16-CCITT-FALSE。
- 解码器为逐字节状态机（`proto_parser_feed`），事件：`NONE/FRAME/CRC_ERR/FMT_ERR/VER_ERR/OVERFLOW`；**无任何动态内存、无未校验循环边界**（模糊测试对象）。
- CMD 常量分层：`0x01–0x32` 遗留直通（语义归 TC275）、`0x41 TELEMETRY / 0x42 LINK_STATE / 0x43 PING|PONG / 0x44 BAUD`、`0x50 DRIVE / 0x51 PAIR / 0x52 CFG / 0x53 DIAG`、`0x60–0x6F OTA`、`0x70–0x7F DPT`。
- 0x41 遥测载荷按 SDD §6.3 定长 38 B 结构（packed struct + encode/decode 帮助函数）；0x42 载荷 1 B（0 无客户端/1 仅观赛/2 控制端在线）；0x43 载荷 1 B 类型 + PONG 回显 SEQ；0x44 载荷 `{u32 baud, u8 op(REQ/ACK/NAK)}`；0x51 载荷 `{op(REQ/CONFIRM/NOTIFY/REJECT), …}`；0x60–0x65 按 C4。

### 3.2 任务 / 队列 / 上下文（LLDD §2.3 映射）

| 任务 | 优先级 | 栈 | 实现落点 |
|---|---|---|---|
| bridge_task | 10 | 6 KB | c6_bridge，QueueSet 聚合（C9），唯一广播/唯一出线编排 |
| uart_evt_task | 12 | 4 KB | c6_link（IDF UART 事件任务）→ 帧装配 → `q_link_rx` |
| link_tx_task | 11 | 3 KB | c6_link（排空 TX 帧队列，C8） |
| httpd 任务 | 5 | 8 KB | c6_http；回调内仅入队/查表，无耗时操作 |
| ota_task | 8 | 6 KB | c6_ota（按需创建，验签+写分区） |
| dns/mdns/legacy 任务 | 4 | 3 KB | c6_net / c6_legacy（lwip socket 阻塞循环） |
| esp_timer(20 ms) | 系统 | — | c6_link 健康+PING（每 5 拍）、c6_bridge 节拍通知 |
| esp_timer(10 s) | 系统 | — | app_state 堆守护采样 |

WDT：`esp_task_wdt_init({5s, idle=0})`（IDF 6.1 新签名）+ bridge/uart_evt/ota 订阅，循环内 `esp_task_wdt_reset()`。

### 3.3 时延与背压落点

- 命令 ≤1 ms：WS 回调（httpd 任务）只做 role/SEQ 校验 + `xQueueSend(q_cmd, 0)`；失败立即回错误 JSON（不静默丢）。bridge 收到即 `link_send()`（拷入 TX 队列，无阻塞调用）。
- 遥测 ≤1 ms：UART 事件任务装配后入 `q_link_rx`；bridge 写**单槽邮箱**（新覆旧，临界区 <1 µs）；20 ms 节拍逐客户端 `httpd_ws_send_data_async`（慢客户端 `slow_count≥3` 降为每 4 拍，再失败只保活——观测端不影响控制端）。
- OTA 中继背压：8×512 B 在途信用窗口；HTTP handler 阻塞在"信用信号量"（2 s 超时→重发该块一次→放弃→0x65 ABORT），堆不被上传流吃穿。

### 3.4 安全落点

- 会话 token 32 B 随机（`esp_fill_random`），**只存 SHA-256 截断 hash**（内存 + NVS `sess_tok`，含 `now+30 s` 过期）。
- 防重放第一道：C6 校验 CTRL 二进制帧 SEQ 单调（乱序/回退 → 拒 + 错误 JSON，计数进 diag）；最终裁决在 TC275 E2E。
- /ota/* 端点强制 CTRL token；DPT 命令需治具令牌才透传 0x70–0x7F。
- ed25519 验签：RFC 8032 流程（canonical s < L 校验、常量由脚本生成），bundle 双段 sha256 + 整体签名；验签失败/缺公钥 → 拒绝写入。

### 3.5 状态机与可靠性

- `app_state`：BOOT（分区/NVS 自检 + rollback PENDING_VERIFY 处置）→ FACTORY_WAIT（无出厂资料且未开 DEV_OVERRIDE）/ NET_START → ONLINE（LINK 握手期间 Web 可连，页面置"车端未连接"）⇄ SELF_OTA → REBOOT_PENDING。
- 自检集（rollback 判定复用）：分区表/NVS 可读、heap ≥64 KB、TWDT 全订阅、LINK 握手成功（30 s 超窗则仍放行网络但记 `LINK_FAIL`，由 LLDD"照常起网"条款兜底）。
- 堆守护：10 s 采样 `heap_caps_get_minimum_free`，<64 KB → `/api/diag` 置警 + 遥测错误注入（V1.0 不自动重启，留人工窗口，LLDD §4.10 的"重启前广播诊断"由 diag 端点承接）。

---

## 4. 对外接口冻结（以代码头文件为准）

LLDD §3.3 签名全部按原文实现（`link_send/link_on_frame/link_health/link_request_baud`、`ws_broadcast_binary/ws_send_ctl/ws_on_binary/ws_controller_sd`、`pair_state/pair_request`、`ota_relay_begin/ota_relay_feed/ota_self_feed`），编码期补齐的配套接口：

```c
/* c6_proto */  int proto_encode(const proto_frame_t*, uint8_t*, size_t);
                proto_rx_ev_t proto_parser_feed(proto_parser_t*, uint8_t, proto_frame_t*);
/* c6_link */   QueueSetMemberHandle_t link_rx_queue(void);        /* bridge QueueSet 用 */
                link_event_t link_pop_event(link_event_t*);        /* UP/DOWN/BAUD 变化 */
/* c6_net */    esp_err_t net_start(const net_cfg_t*);             /* AP+DNS+mdns 一键起 */
                void net_on_ap_clients(net_clients_cb_t cb);       /* 客户端增减→LINK_STATE */
/* c6_http */   void http_on_session_change(http_sess_cb_t cb);    /* WS 开/关→LINK_STATE */
                int  http_post_cmd(const proto_frame_t*, int sd);  /* 入 q_cmd, 满回 BUSY */
/* c6_pair */   void pair_set_output(void (*out)(const proto_frame_t*)); /* bridge 注入 */
                void pair_on_frame(const proto_frame_t*);          /* PAIR_CONFIRM 路由 */
/* c6_bridge */ esp_err_t bridge_start(void);
                const proto_frame_t* bridge_telemetry_snapshot(void); /* 首屏/健康用 */
/* c6_ota */    ota_self_state_t ota_self_feed(const uint8_t*, size_t);
                size_t ota_self_result(char* json, size_t cap);
```

---

## 5. 构建与验证门（gate）

| Gate | 内容 | 通过标准 |
|---|---|---|
| G1 主机单测 | `make -C test/host`：proto 全分支（乱序字节注入、边界 LEN、CRC 篡改、VER 错、溢出恢复）+ 10⁷ 随机帧模糊（不死机不越界）；sha512 已知向量；ed25519 RFC 8032 正/负向量；bundle 越界防护 | 全绿，proto 100% 分支 |
| G2 目标编译 | `idf.py build`（IDF v6.1-beta1，C6 目标，自定义分区表 + sdkconfig.defaults） | 0 error / 0 新增 warning |
| G3 静态自检 | 代码走查：无动态内存（httpd 回调除外，仅栈上）、所有 `memcpy` 带边界、ISR 禁锁 | 走查单 |
| G4 台架联调（后续） | LLDD §9：LINK 握手/降速、断链 LINK_STATE ≤10 ms、OTA 中继断电注入 | 另行排期，不在本次编码范围 |

## 6. 风险与回写项

| 风险 | 缓解 |
|---|---|
| IDF v6.1-beta1 与 LLDD 假定的 5.4 存在 API 差异（task_wdt 新签名、mdns 移除、httpd LRU API 改名） | 以 6.1 实际头文件为准编码（已核实）；差异记录于本文件 C2/C8，待正式版复核 |
| ed25519 自实现正确性 | C3：Python 生成常量 + RFC 8032 正/反向量 + 模糊测试，主机端全验证后才进目标码 |
| LINK 引脚未锁定（LLDD Q1） | Kconfig 可配（默认 GPIO10/11），EE 评审后仅改配置 |
| myCar 侧仍为 proto v1 | C1：c6_proto 单文件可直拷；TC275 升 v2 时零改动接入（另立任务） |

---

## 7. 编码落地差异记录（相对本计划，实现时确认的事实）

| # | 差异 | 说明 |
|---|---|---|
| R1 | IDF v6.1-beta1 实际 API 与编码期核对的补充 | `esp_reset_reason()` 并入 `esp_system.h`；`IP_EVENT_AP_GOT_IP` 事件已移除（AP 地址为静态默认 192.168.4.1，c6_net 内置常量）；`esp_app_get_description()`（非 get_version）；`heap_caps_get_minimum_free_size()`；`esp_core_dump_image_get(addr, size)` 双参数 |
| R2 | NVS 加密方案 | IDF 6.1 默认走 HMAC-eFuse 派生；按 LLDD `nvs_keys` 分区方案改选 `CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH=y`，`CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID=0` 仅满足编译期范围检查 |
| R3 | SHA-512 自实现（C3 落地） | 未用 mbedTLS：`c6_ota/sha512.c` + `ed25519v.c` 全部主机可编译；payload 摘要固定为 **SHA-512 截断 32 字节**（bundle 头 `c6_sha/assets_sha` 字段），`tools/sign_bundle.py` 与固件两侧一致 |
| R4 | ed25519 倍点公式 | RFC 8032 专用 dbl 公式在单位元上不闭合，会破坏从 identity 出发的 double-and-add；已改为统一加法公式做倍点（`ge_double == ge_add(P,P)`），RFC 8032 正/反向量验证通过 |
| R5 | optional 组件空注册 | `c6_maint`/`c6_legacy` 关闭时以空 `idf_component_register()` 占位，避免默认组件解析强编 NimBLE/lwip 路径 |
| R6 | 构建产物 | `build/c6_car.bin` ≈ 1.05 MB（3 MB 槽位余量充足）；assets/签名工具链实测可用（`build/assets.bin` 4.6 KB、签名 bundle ≈1.05 MB） |

**G1–G3 状态：G1 全绿（proto 12 项含 10^7 模糊 / sha512 4 项 / ed25519 4 项 / bundle 6 项）；G2 `idf.py build` 0 error，自研组件 0 warning；G3 走查随编码完成。G4（HIL）按计划另行排期。**
