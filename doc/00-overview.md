# smartcar_remote 固件 — 模块设计文档总览与完成状态矩阵

| 项 | 内容 |
|---|---|
| 文档版本 | V1.0 |
| 日期 | 2026-09-29 |
| 上游 | 本仓库 [doc/ESP32-S3-LCD-EV-Board v1.5 远距离智能遥控器——LCD UI-UX 产品级需求规格书](../doc/ESP32-S3-LCD-EV-Board%20v1.5%20远距离智能遥控器——LCD%20UI-UX%20产品级需求规格书.md)（下称**规格书**，引用写作 spec §N，**设计基准**）→ [`esp32c6_car/doc`](../../esp32c6_car/doc/00-overview.md) 的 [02-proto](../../esp32c6_car/doc/02-proto.md) / [05-net](../../esp32c6_car/doc/05-net.md) / [06-pair](../../esp32c6_car/doc/06-pair.md) / [07-http](../../esp32c6_car/doc/07-http.md)（**协议与链路口径基准**）→ 本目录（各模块详细设计 + 完成状态） |
| 代码基线 | `smartcar_remote` 工作区（ESP-IDF v6.1 / esp32s3，`idf.py build` 通过，`build/smartcar_remote.bin` ≈ 1.51 MB，factory 6 MB 分区余 75%） |
| 硬件 | ESP32-S3-LCD-EV-Board-2（主板 MB v1.5，N16R16V）+ SUB3 子板 4.3" 800×480 RGB（ST7262E43）+ GT1151 触摸 |

## 状态图例

| 标记 | 含义 |
|---|---|
| ✅ | 完成：已实现 **且** 经主机自测或编译级验证 |
| 🟩 | 完成：已实现，待真机/HIL 验证 |
| 🟡 | 部分：主体完成，存在明确交付缺口（在文档内逐条列出） |
| 🔴 | 未实现 |
| ⚪ | V1.0 明确裁剪（规格书分阶段要求或硬件不支持） |

## 模块索引与完成状态矩阵

| 文档 | 模块 | 代码位置 | 需求追溯 | 完成度 | 验证状态 |
|---|---|---|---|---|---|
| [01](01-app-state.md) | 组合根与 UI 状态中心 | `main/app_main.c` `main/app_state.c/.h` | spec §59/§83/§95.8-9/§98-101 | 🟩 代码完成 | ✅ 编译级；EMA/状态推导经走查 |
| [02](02-proto.md) | proto v2 编解码（复用） | `main/proto/proto_frames.[ch]` | c6_car doc 02 / spec §95.5 | ✅ 100% | ✅ 主机自检（CRC 0x29B1 + 帧回环） |
| [03](03-settings.md) | 用户设置（NVS） | `main/scr_settings.c/.h` | spec §31-33/§35/§15 | 🟩 代码完成 | ✅ 编译级；NVS 持久化待真机 |
| [04](04-link.md) | 无线链路（Wi-Fi/WS） | `main/scr_link.c/.h` | c6_car doc 05/06/07 / spec §95.5-7 | 🟩 代码完成 | 🟩 编译级；与 C6 真机联调未开始 |
| [05](05-ctrl.md) | 控制与安全 | `main/scr_ctrl.c/.h` | spec §18-22/§59/§102-105 | 🟩 代码完成 | 🟩 编译级；STOP/急停语义待 HIL |
| [06](06-ui.md) | LCD UI（LVGL 9） | `main/ui/` | spec §4-§71（UI 全节） | 🟩 代码完成 | 🟩 编译级；触摸/显示待真机 |
| [07](07-verification.md) | 验证与测试汇总 | `test/host/` + 真机日志 | spec §111 验收清单 | 🟡 G1-G3 绿，G4 首轮真机 ✅ | 真机排错 R1-R5 见 07 §5.0；HIL 联调进行中 |

## 系统级完成视图（spec §111 验收清单映射）

```
验收项                                        实现落点            状态
800×480 布局（硬件实测 480×480 结构亦成立）    06-ui               🟩 (待真机)
Touch / Home 驾驶界面 / 虚拟摇杆 / STOP        06-ui + 05-ctrl     🟩 (待真机)
Emergency Stop（长按 + RELEASE）               05-ctrl + 06-ui     🟩 (待 HIL)
Connection / Control Owner / Vehicle State     04-link + 06-ui     🟩
Speed / Battery / RSSI / Latency / Packet Loss 04-link + 01-state  🟩 (阈值待实测标定)
Radio / Vehicle / Diagnostics / Settings 页    06-ui               🟩
Pairing 页（车侧窗口 + POST /api/pair）        04-link + 06-ui     🟩
Alert Overlay（等级配色 + ACK/RELEASE）        01-state + 06-ui    🟩
Event Log                                      01-state + 06-ui    🟩
Dark Theme                                     06-ui               ✅
Telemetry 不阻塞 UI / Radio 不阻塞 UI          任务边界见 07 §3    ✅ (架构保证)
失联后状态 / 手机接管后状态 / 控制权无误触      05-ctrl + 06-ui     🟩 (待 HIL)
屏幕校准页 / Quick Panel / Radio Test 工程模式  —                   ⚪ (二阶段)
遥控器自身电池 / 温度显示                       —                   ⚪ (无硬件, spec §57)
```

## 一致性约束（所有模块文档的公共口径）

1. 帧格式 `AA 55 VER(0x02) CMD SEQ LEN DATA[≤64] CRC16-CCITT-FALSE`，多字节载荷
   **显式小端**；编解码源文件与 `esp32c6_car` **逐字节同源**（见 [02](02-proto.md)）。
2. **组合根 = `app_main`**：所有跨模块初始化顺序固定（NVS → 状态 → 显示 → UI →
   链路 → 控制），模块间不互相 include 对方私有头。
3. **任务边界 = 解耦规则**（spec §95.8/95.9/§74）：UI 只在 LVGL 任务运行；
   `scr_link`/`scr_ctrl` 不持有任何 LVGL 对象；所有跨任务数据只经
   `app_state` 快照（互斥拷贝）交换，页面间共享同一份状态（spec §100）。
4. **控制权裁决链**：S3 只表达与转发（DRIVE 0x50 + 0x32），最终安全裁决在
   TC275；S3 侧 STOP/急停是"本端立即 + 对端尽力"双层语义（见 [05](05-ctrl.md) §3）。
5. **阈值口径**：RSSI 分档、ECO/NORMAL/SPORT 限幅、死区默认值均为台架初值
   （`main/Kconfig.projbuild`），**未经实车距离/驾驶测试不得视为产品标准**
   （spec §26/§33）。
