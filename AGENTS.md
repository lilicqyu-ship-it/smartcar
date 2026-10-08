# AGENTS.md — smartcar monorepo

智能车固件 monorepo：五个固件工程 + iOS 遥控器 App + 共享接口契约 + 固件工具链，
一次提交跨工程原子生效。语言/文档惯例为**中文**，代码注释含大量单位域与硬件事实，改动前先读对应真源文档。

## 布局

| 目录 | 内容 |
|---|---|
| `esp32c6_car/` | ESP32-C6 小车主控（ESP-IDF；车端 WebSocket/SPI 桥） |
| `smartcar_remote/` | ESP32-S3 硬件遥控器（LVGL + proto） |
| `s3-gateway/` | ESP32-S3-CAM 车端网关（视觉 + Camera WS） |
| `ios_remote/` | iPhone 遥控器 App S3Remote（SwiftUI，proto v2 over WS；不经 fw.py） |
| `tc275_car/` | TC275 车体控制（TriCore 三核；**有自己的子目录 AGENTS.md，改码前先读**） |
| `tc275_sbl/` | TC275 OTA 二级引导（PFlash 双 bank） |
| `contracts/` | 跨工程接口唯一权威版本（link/proto、ota、camera、crypto、vision） |
| `firmware/` | `fw.py` 五工程统一编译/烧录/归档/OTA 入口 |
| `matlab_motor_model/` | 电机控制律 MATLAB 建模（与 `tc275_car/rt/servo.c` 逐位等价） |
| `doc/00-usage.md` | 总使用手册（命令、场景、CI、FAQ） |

数据路径：iPhone/S3 遥控器 → C6 softAP（WebSocket `/ws`：二进制 = proto v2 帧，
文本 = JSON 控制面）→ SPI/SF 帧 → TC275（CPU0 FreeRTOS 融合/安全，CPU1 电机 1 kHz，
CPU2 链路泵）。相机 MJPEG 走 s3-gateway :81，与控制面独立。

## 常用命令（都在仓库根执行）

```bash
just                    # 列出全部命令
just doctor             # 本机环境体检
just contracts          # 校验各工程接口副本与 contracts/ 逐字节一致（CI 同款门禁）
just fw-build <工程>     # 编译固件（别名 c6/gw-s3/r-s3/app/sbl）
just fw-flash <工程>     # 编译+烧录（ESP 板串口自动识别）
just scons-car          # TC275 固件 TASKING SCons 直编（需本机 TASKING TriCore v6.3r1）
just ios-test           # iOS 单测（模拟器免签名；真机安装 just ios-install）
just tag <别名> <版本>   # 工程版本 tag（前缀 c6/ gw-s3/ r-s3/ app/ sbl/ ios/）
```

主机端单测（无需硬件）：

- TC275：`tc275_car/test/host/` 每个测试文件头部注释里有**精确的 gcc 命令**
  （要点：`-I test/host/stub` 必须在 `-I .` 之前，桩头文件顶替 iLLD 依赖）。
- C6：`make -C esp32c6_car/test/host`。
- iOS：`just ios-test`，或 xcodebuild 指定模拟器 UDID。

## 铁律（违反会被 CI 或台架拒绝）

1. **共享接口只改 `contracts/`**：改权威版本 → `just contracts-apply` → 契约与所有
   工程副本放进**同一个提交**。禁止单方面改任何工程里的副本。
2. **TC275 三核分工固定**：CPU0=FreeRTOS，CPU1/CPU2=裸机（禁用任何 FreeRTOS API）；
   核间只走 `mw/xcore` 共享内存。详见 `tc275_car/AGENTS.md` 与其 33 号 AI 指南。
3. **单位域别串**：`±100`（协议）/ `±1000`（电机·伺服 pct×10）/ `mm/s`（遥测）。
4. **行为变更同批改文档**（TC275 真源索引 `tc275_car/doc/00-index.md §5`）。
5. 接口/协议的线缆格式真源在 `contracts/` 与各 `doc/`，不要从代码反推后当事实写。
6. Windows 上 just 配方必须走 Git Bash（justfile 已写死路径），勿改成裸 bash。

## 各端注意事项

**iOS（ios_remote/）**：Swift 6 + iOS 17+，零第三方依赖（图表全部手绘
SwiftUI Canvas）；Xcode 16+ 文件系统同步组——新 .swift 文件放进 `S3Remote/`
目录即自动入编，无需改 pbxproj。UI 走 `Theme`（暖白纸底/陶橙点缀）、
`Page`/`.panel()`/`PageHeading` 组件族；强制浅色模式。测试在 `S3RemoteTests/`
（纯逻辑 XCTest）。协议解析须与 C6 桥接逐字段对齐（`Link/TextMessage.swift`
注释标注线缆真源）。本机已知的预存测试失败（与改动无关，改前先在干净 HEAD
核实）：`CameraPipelineTests.testInterleavedSmallFeeds…`、`SystemTests` 两例。

**C6 / S3 工程（ESP-IDF）**：构建需 ESP-IDF 环境（本 Mac 的 PATH 里没有
idf.py——承诺构建前先确认）。改 `c6_bridge`/`c6_link` 时对照
`esp32c6_car/doc/`（04-link / 08-bridge / 14-sf-link）。

**TC275**：固件只能 TASKING 编译（SCons 或 AURIX Studio，二者同源）；
主机/CI 编不了固件，只能跑 host 单测。改 SF 帧/遥测/融合层后必须跑对应
host 测试。板间 SPI 从未两板通电联调，`link.c`/`spi_hal_pins.c` 改动需台架验证。

## 其他

- 仓库根有 `.codegraph/` 索引：定位代码优先用 `codegraph explore "<符号/问题>"`
  或 codegraph MCP 工具，其次才是 grep。
- 版本号真源：iOS 在 pbxproj 的 `MARKETING_VERSION`（`just ios-version`），
  TC275 App 在 `mw/app_version.h`，发版一律走 `just tag`。
