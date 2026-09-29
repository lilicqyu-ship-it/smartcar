# 04 无线链路（scr_link）

| 项 | 内容 |
|---|---|
| 代码位置 | `main/scr_link.c/.h` |
| 上游需求 | [c6_car doc/05-net](../../esp32c6_car/doc/05-net.md)（AP 口径）、[doc/06-pair](../../esp32c6_car/doc/06-pair.md)（配对时序）、[doc/07-http](../../esp32c6_car/doc/07-http.md)（WS 会话/角色/闸门）、spec §95.5-7（确认协议/Telemetry/不伪造）、§103（控制权丢失） |
| 状态 | 🟩 **代码完成** — 与 C6 真机联调未开始 |

## 1. 职责

S3 ↔ C6 全链路：Wi-Fi STA 接入 softAP → WebSocket `/ws`（proto v2 二进制 +
JSON 文本面）→ 配对 REST → 收发统计 → 静默看门狗。**只使用 C6 现有接口，
不要求 C6 任何改动**（spec §96/97）。

## 2. 结构与任务

```
wifi_init（ STA + WPA2 + WIFI_PS_NONE 驾驶链路省电关闭 + boot_mark_radio ）
   │  事件: STA_START→connect / STA_DISCONNECTED→打印 reason + 重连
   │        （事件驱动，绝不与进行中的尝试叠加） / GOT_IP→wifi_up=true
   ▼
link_monitor_task（250 ms 节拍，prio 4 / 栈 6144）
 ├─ !wifi_up → 等待事件驱动重连（不轮询 connect，防 "sta is connecting" 刷屏）
 ├─ wifi_up && ws==NULL → ws_start()（URI 带 token 时即 CTRL 候选）
 ├─ ws 静默 > CONFIG_SCR_RX_WATCHDOG_MS（10 s）→ ws_restart()
 │    （对齐手机页：iOS 后台化式的半开连接，无 close 事件只能靠静默判定）
 ├─ pair_req 置位 → pair_do()（HTTP，阻塞 ≤6 s，独占 monitor 上下文）
 └─ 每 1 s housekeeping（§5）
```

WS 客户端自任务收发（managed `esp_websocket_client`，auto-reconnect 3 s）。
`esp_websocket_client_send_bin/text` 自带互斥，ctrl 任务 / monitor 任务 /
事件回调三方可并发发送。

## 3. WS 事件处理

| 事件 | 行为 |
|---|---|
| CONNECTED | conn=CONNECTED；**主动发 DRIVE(0,0)**（对齐手机页 onopen，复位 TC275 心跳链）；等 hello 定角色 |
| DISCONNECTED | conn=NONE、ctrl_role=false、tc=false；monitor 依赖客户端 auto-reconnect |
| DATA(op=0x02) | 字节流喂 `proto_parser_feed`；仅消费 `0x41 TELEMETRY`（38 B LE 解码 → `app_state_set_telemetry` + seq 缺口统计） |
| DATA(op=0x01) | JSON：`hello{role,ver,tc}` → 角色/固件版本/车辆链路；`tc{on}`；`pong` → RTT 采样；`err{e=auth}` → ctrl_role=false（spec §103 控制权失效） |

角色映射：`hello role=ctrl` → `owner=S3 MASTER`；`spec` → `owner=NO CONTROL`
（C6 不告知对端角色，WEB MASTER 不伪造，见 [01](01-app-state.md) S-8）。

## 4. 配对（对齐 c6_car doc 06 §3 时序）

```
UI [PAIR] → scr_link_request_pair()（仅置位）
monitor 任务 → POST http://<C6>/api/pair
  ├─ 200 {ok, token} → set_token(NVS) → ws_restart()（带 token 重连即 CTRL）
  ├─ 403 → "No pair window. Press car button 3 s first"   （窗口未开）
  ├─ 409 → "Vehicle busy: another controller paired"      （已 CLAIMED）
  ├─ 504 → "Timeout. Check vehicle, then retry"
  ├─ 503 → "Vehicle link down (TC275 offline)"
  └─ 其余 → "Pair failed. Check C6 and retry"
状态文本 → app_state.pair_status → Pairing 页展示（spec §41：不许只报 Error，
必须给下一步动作）
```

`scr_link_apply_wifi()`：Radio 设置页 APPLY 后重配 STA 凭据并主动断开，
monitor 以新凭据自动重连。

## 5. 统计（1 s 窗口，spec §24-28）

| 量 | 算法 |
|---|---|
| TX rate | 1 s 内成功 send_bin 帧数（≈30 Hz DRIVE） |
| RX rate | 1 s 内遥测帧数（名义 50 Hz；慢客户端会被 C6 降频） |
| Loss | 遥测 seq 缺口：`Δ∈(1,1000)` 记 `Δ-1` 丢帧；`loss‰ = lost/(lost+rx)`，u16（0.1 % 单位，1000=100 %）；有历史但整秒 0 帧按 100 % |
| Latency | 文本 `{"t":"ping"}` → `pong` 的 RTT，1 Hz；维护 min/max |
| RSSI/信道 | `esp_wifi_sta_get_ap_info` → rssi + primary；RSSI 分档 → quality（Kconfig 阈值，**待实测标定** spec §26） |

## 6. 接口（全量）

```c
void  scr_link_start(void);
bool  scr_link_send_bin(const uint8_t *data, size_t len);   /* 线程安全 */
bool  scr_link_send_text(const char *text);
uint8_t scr_link_next_seq(void);            /* 会话级 SEQ 唯一计数器 */
void  scr_link_request_pair(void);
void  scr_link_apply_wifi(void);
```

SEQ 单点发放：ctrl 任务与 link 的初始帧共享同一计数器，保证会话内
`seq ≠ last_seq` 闸门（c6_car doc 07 §4）永远通过。

## 7. 资源

| 任务/栈 | 值 |
|---|---|
| link_monitor | 6144 B / prio 4 |
| ws client 任务 | 6144 B（组件配置） |
| 发送超时 | 0（try-only）：`is_connected` 快速门控 + 忙则丢帧；DRIVE 33 ms 后重发，**绝不阻塞 ctrl 任务**（阻塞会饿死核 1 的 LVGL 供帧 → 屏幕抖动） |
| TX 锁 | `CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y`：30 Hz 写入不串行化接收循环与 keep-alive PONG（真机日志：分离前 PONG 锁超时刷屏） |

## 8. 完成状态表

| # | 功能 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| L-1 | Wi-Fi STA + 重试 + PS 关闭 | ✅ | `wifi_init` / monitor |
| L-2 | WS + proto v2 收发 | 🟩 | 代码完成；**真机帧流未验** |
| L-3 | JSON 文本面（hello/tc/pong/err） | 🟩 | 代码完成；字段口径以真机 hello 为准 |
| L-4 | 静默看门狗 10 s 重连 | ✅ | 对齐 app.js 3 s/10 s 策略 |
| L-5 | 配对四类失败可读提示 | ✅ | HTTP 状态 → 动作文案 |
| L-6 | 统计 + RSSI 分档 | ✅ | loss u16 ‰ |
| L-7 | 丢包与 RTT 跨重连复位 | 🟡 | `prev_valid` 在 ws_restart 复位，但 lat_min/max 不清零（诊断页累计口径，暂保留） |
| L-8 | 信道固定/白名单 | ⚪ | STA 跟随 AP；AP 侧信道治理属 C6（doc 05） |

## 9. 已知限制与风险

1. 配对请求在 monitor 任务内阻塞执行（≤6 s），期间看门狗检测暂停——配对时
   本就不在驾驶态，可接受；若后续要驾驶中改网，需移到独立任务。
2. RSSI 采样 1 Hz，Radio 页条形图为 1 s 步进（spec §75 的 5 Hz 建议未用，
   低优先级）。
