# 02 proto v2 编解码（main/proto，复用 c6_proto）

| 项 | 内容 |
|---|---|
| 代码位置 | `main/proto/proto_frames.h/.c`（**逐字节拷贝自** `esp32c6_car/components/c6_proto/`，未改动） |
| 上游需求 | [esp32c6_car doc/02-proto.md](../../esp32c6_car/doc/02-proto.md)（唯一权威规格）/ spec §95.5（确认现有 C6 协议）、§97（不得重写 C6） |
| 状态 | ✅ **100%** — 主机自检通过（CRC check 值 + 帧回环） |

## 1. 复用策略（决策）

手机 Web 页、S3 遥控器与 C6 使用**同一套 WS 段协议**；c6_proto 的硬规则
（纯 C99、无 OS/IDF 头、无动态内存、显式小端）使该文件可以原样进 S3 工程。
S3 侧**不改一字**，消除"两端各写一份编码器"的漂移风险；后续协议演进直接
从 esp32c6_car 重新拷贝。

## 2. 帧格式（口径同 esp32c6_car doc 02 §2）

```
| AA | 55 | VER=0x02 | CMD | SEQ | LEN | DATA[LEN] | CRC16 |
CRC = CRC16-CCITT-FALSE（poly 0x1021, init 0xFFFF, MSB first）
check("123456789") == 0x29B1；帧长上限 72 B
```

## 3. S3 实际使用的命令子集

| CMD | 名称 | S3 用法 |
|---|---|---|
| 0x50 | DRIVE | 30 Hz `{i16 v mm/s, i16 ω deg/s}`，兼 TC275 心跳；满行程 v=600 / ω=300（与手机页 app.js 相同）；**另在每个 WS CONNECTED 时发一帧 (0,0) 复位链路** |
| 0x41 | TELEMETRY | 38 B LE 遥测，50 Hz 广播（S3 只收） |
| 0x32 | EMERGENCY_STOP | 急停时随 DRIVE(0,0) 附加发送（legacy 透传，语义在 TC275；DRIVE(0,0) 是保证停车的兜底） |

其余命令（PAIR 0x51 / CFG 0x52 / OTA 0x60-0x65 等）S3 一律不产生、不消费——
配对走 REST `POST /api/pair`（[04](04-link.md)），OTA 属手机 Web 职责（spec §113）。

### 3.1 遥测载荷（0x41，38 B，显式小端）

| 偏移 | 字段 | 类型 | 偏移 | 字段 | 类型 |
|---|---|---|---|---|---|
| 0 | seq | u32 | 19 | battery_mv | u16 |
| 4 | uptime_ms | u32 | 21 | battery_pct | u8 |
| 8 | state | u8 | 22 | odo_session_mm | u32 |
| 9 | fault_code | u16 | 26 | odo_total_mm | u32 |
| 11 | v_target_l/r | i16×2 | 30 | link_rtt_ms | u16 |
| 15 | v_meas_l/r | i16×2 | 32 | link_err_rate | u8 |
| 33 | fw_ver | u32 | 37 | hw_rev | u8 |

`seq` 用于 S3 侧丢包估计（见 [04](04-link.md) §5）；`link_rtt_ms/link_err_rate`
是 C6↔TC275 的链路质量，S3 原样展示于 Radio/Diagnostics 页（spec §58）。

## 4. 主机自检（test/host，对齐 esp32c6_car 主机单测思路）

```
cd test/host && make        # G1 门，无需目标机
  ok  crc16 check value 0x29B1        ok  crc16 NULL guard
  ok  frame length 12 / sync+ver / cmd-seq-len
  ok  parsed to FRAME event + LE payload v=600 w=300
  ok  telemetry encode/decode 回环（u32/u16/i16 负值/u8/u32）
  ok  resync through junk to valid frame
PASS: all cases
```

覆盖：CRC check 值（esp32c6_car G1 门同源断言）+ BUILD→PARSE 回环 + 遥测编解码
回环（含负速度）+ 垃圾字节再同步。完整 10⁷ 模糊测试已在 esp32c6_car 侧完成，
本文件逐字节同源，无需重复。

## 5. 完成状态表

| # | 功能 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| P-1 | 文件同源拷贝（不改动） | ✅ | `cp` 自 esp32c6_car，`diff` 可校验 |
| P-2 | S3 自检脚本级验证 | ✅ | CRC 0x29B1 + 帧回环（本机 cc） |
| P-3 | 与 C6 真机互通 | 🟩 | 待联调（WS 帧流） |
| P-4 | 协议演进同步机制 | 🟡 | 现为人工拷贝；建议后续以 CI 比对两仓文件哈希（esp32c6_car 对 TC275 已有此约定） |
