# SmartDrive S3-CAM 网关固件 — 模块设计文档总览与完成状态矩阵

> **移植说明（2026-10-02）**：本目录随 `esp32c6_car` 全量复制而来，正文中的 **C6 口径
> （引脚、8 MB flash、`esp32c6_car.bin`、`c6_adxl345`）仍是设计基准**，与本工程实际差异以
> [`README.md`](../README.md) 的"引脚分配 / 摄像头与 MJPEG 推流 / build 产物与分区"三节为准：
> 目标芯片 ESP32-S3（Freenove ESP32-S3-WROOM CAM，16 MB flash + 8 MB octal PSRAM）、
> LINK 改为 SCLK40/MOSI39/MISO41/CS42/IRQ2、指示灯 GPIO48、新增 `s3_camera`（doc 19 待写）、
> 不移植 `s3_adxl345`（doc 18 已删）。协议面（proto v2 / SF 帧 / `/ota/c6` / `C6FW`）零改动。

| 项 | 内容 |
|---|---|
| 文档版本 | V1.0（C6 基线）+ 2026-10-02 S3-CAM 移植批注 |
| 日期 | 2026-09-26 |
| 上游 | [21-software-design.md](../../tc275_car/doc/20-design/21-software-design.md)（量产 SDD，**设计基准**）→ [22-link-spi-design.md](../../tc275_car/doc/20-design/22-link-spi-design.md)（板间 SPI/SF 帧详细设计）→ [41-c6-docs-map.md](../../tc275_car/doc/40-esp32c6/41-c6-docs-map.md)（两仓库文档分工与跨仓 TODO）→ 本目录（各模块详细设计 + 完成状态） |
| 代码基线 | `s3-gateway` 工作区（`idf.py build` 通过，`build/s3_gateway.bin` ≈ 1.06 MB，target esp32s3） |
| 控制端 | 手机 Web 控制页（`assets_src/`，烧入 assets 分区）＋ ESP32-S3 LCD 遥控器（平级仓库 `../smartcar_remote`，2026-09-29 起：proto v2 编解码原样复用本仓 s3_proto，协议面零改动对接，**双端联调待做**） |

## 状态图例

| 标记 | 含义 |
|---|---|
| ✅ | 完成：已实现 **且** 经主机单测或编译级验证 |
| 🟩 | 完成：已实现，待目标机/HIL 验证 |
| 🟡 | 部分：主体完成，存在明确交付缺口（在文档内逐条列出） |
| 🔴 | 未实现 |
| ⚪ | V1.0 明确裁剪（LLDD"明确不做"或编码计划决策） |

## 模块索引与完成状态矩阵

| 文档 | 模块 | 代码位置 | 需求追溯 | 完成度 | 验证状态 |
|---|---|---|---|---|---|
| [01](01-app-state.md) | 应用状态机与启动编排 | `main/app_main.c` `main/app_state.c` | LLDD §4.1/§4.10 | 🟡 90% | 🟩 编译通过；link_task TWDT 已补 |
| [02](02-proto.md) | proto v2 编解码（手机/WS 侧，S3 遥控器同源复用） | `components/s3_proto/` | SDD V1.2 §6.1b / LLDD §3.1 | ✅ 100% | ✅ 主机单测 12 项 + 10⁷ 模糊 |
| [03](03-factory.md) | 出厂数据（NVS） | `components/s3_factory/` | LLDD §4.8 | 🟡 90% | 🟩 编译通过；写入入口（DPT）未接 |
| [04](04-link.md) | LINK 链路（**SPI 从机**） | `components/s3_link/` `components/s3_sf/` | tc275_car doc 22 / SDD V1.2 §6.1a / FR-3 | 🟩 代码完成 | 🟩 G1/G2 通过；波形兼容与台架门禁待测 |
| [05](05-net.md) | 接入网 | `components/s3_net/` | LLDD §4.2 / FR-1 | 🟡 95% | 🟩 编译通过；Portal 弹窗真机验证（09-26）；mDNS 解析待手机实测 |
| [06](06-pair.md) | 配对与会话 | `components/s3_pair/` | LLDD §4.4 / FR-4 | 🟩 100% | 🟩 编译通过，流程未联调 |
| [07](07-http.md) | Web 服务 | `components/s3_http/` | LLDD §4.3 / §3.2 / FR-2 | 🟡 92% | 🟩 编译通过；HELLO/abort 缺陷已修待回归 |
| [08](08-bridge.md) | 三台泵 | `components/s3_bridge/` | LLDD §4.6 / §2.4 | 🟩 97% | 🟩 编译通过；信用窗时序待 HIL |
| [09](09-ota.md) | 自身升级与验签 | `components/s3_ota/` | LLDD §4.7 / FR-6 | 🟡 92% | ✅ 验签/解析主机单测；OTA 流程待目标验证 |
| [10](10-maint.md) | BLE DPT 通道 | `components/s3_maint/` | LLDD §4.9 / FR-7 | 🔴 55% 骨架 | 🔴 未编译（默认关）、C6 本地 DPT 项未实现 |
| [11](11-legacy.md) | TCP 8080 直通桥 | `components/s3_legacy/` | LLDD FR-10 | 🟡 代码完成 | 🔴 未编译（默认关）、未测试 |
| [12](12-assets-tools.md) | 控制页与工具链 | `assets_src/` `tools/` | SDD §11 / LLDD §4.7 | 🟡 95% | ✅ 工具实测；assets 分区+Portal+WS 观察态真机验证（09-26）；配对/驾驶待 TC275 |
| [13](13-verification.md) | 验证与测试汇总 | `test/host/` | LLDD §9 + doc 22 §8 | 🟡 G1 绿 / G2 绿 | G3 走查完毕；G4 HIL 未开始 |
| [14](14-sf-link.md) | **SF 链路详设（SPI 落地）** | `components/s3_sf/` `components/s3_link/` | tc275_car doc 22 §4–§5 | 🟩 代码完成 | ✅ test_sf 7 项；波形兼容待台架 |
| [15](15-led.md) | WS2812 状态指示灯 | `components/s3_led/` | bring-up 运维需求 | 🟩 代码完成 | 🟩 真机验证（绿心跳=正常） |
| 19（待写） | OV5640 采集 + MJPEG 推流 | `components/s3_camera/` | S3-CAM 移植新增需求 | 🟩 代码完成 | 🟩 真机启动验证（OV5640 探测 + :81 推流起）；手机侧画面待看 |
| [20](20-core-assignment.md) | **双核功能分配**（核0=RF/IP 面，核1=板级面） | 各组件任务创建点 + `sdkconfig.defaults` | S3 移植新增（LLDD §2.3/§2.4 的单核基线） | 🟩 代码完成 | 🟩 开机 `task map` 核对；推流下 LINK RTT 待 HIL |

## 系统级完成视图

```
需求侧（LLDD §1）                        实现落点                 状态
FR-1  softAP/Portal/mDNS                 05-net                   🟡 (mDNS 简化实现，待真机)
FR-2  WS 服务器 / 50Hz 遥测 / <1ms        07-http + 08-bridge      🟡 (已实现，端到端待联调)
FR-3  LINK（SPI 5M / SF 帧 / 健康）       04-link + 14-sf-link     🟩 (代码完成；22 §8 台架门禁待测)
FR-4  配对                               06-pair                  🟩
FR-5  断链即报 LINK_STATE                 08-bridge + 04-link      🟩
FR-6  自身 OTA + TC275 中继               09-ota + 08-bridge       🟡 (全流程待目标验证)
FR-7  产测通道                           10-maint                 🔴 (骨架)
FR-8  安全(token/防重放/验签)             06/07/09                 🟡 (secure boot/熔断属产线)
FR-9  可靠性(WDT/coredump/堆守护)         01-app-state             🟡 (link 任务未订阅 TWDT)
FR-10 遗留桥                             11-legacy                🟡 (代码完成未验证)
```

## 一致性约束（所有模块文档的公共口径）

1. 帧格式 `AA 55 VER(0x02) CMD SEQ LEN DATA[≤64] CRC16-CCITT-FALSE`，多字节载荷**显式小端**。
2. **唯一 LINK TX 编排上下文 = bridge_task**；pair/link 健康定时器只通过 `link_send()`（内部互斥 + TX 队列）。
3. **唯一 WS 广播上下文 = bridge_task**（20 ms 节拍）；httpd 任务内不做耗时操作。
4. 组合根 = `app_main`：所有跨组件回调/sink 注册只发生在这里（星型依赖规则，LLDD §2.2）。
5. 状态标记口径：文档中的"完成"以**本文档集书写时点的代码**为准（含 HELLO/abort 两处修复后的回归编译）。
