# ios_remote — S3 Remote iOS App（iPhone 遥控器）

用 Swift/SwiftUI 在 iPhone 上复刻 [`smartcar_remote`](../smartcar_remote)
（ESP32-S3 LCD 硬件遥控器）的核心功能：经 C6 softAP + WebSocket 以 proto v2
协议驾驶车辆、显示遥测、配对、诊断与安全告警。**不要求 C6 / TC275 /
s3-gateway 做任何改动**——与 S3 遥控器、手机 Web 页共用同一套车端接口。

```
iPhone (本 App) ──Wi-Fi STA→ C6 softAP ──WebSocket /ws── proto v2 + JSON 文本面
S3 遥控器      ──同上（同构客户端）
C6 ──SPI/SF帧── TC275 ── 车辆
```

## UI（精密遥控台）

暖白背景、深墨色操控台、陶橙色交互重点。驾驶页将实时速度、摇杆和输入反馈
收进一个操控区；驾驶模式用中文命名，停止按钮固定在底部，滚动页面时仍可触达。
连接、配对与异常提示使用中文，未连接时可从驾驶页直达连接设置。
车辆、连接中心、设置使用统一的标题层级与轻量卡片。

- 摇杆：上/下对应前进/后退，左/右对应转向；离开驾驶页或失去控制权时归零。
- 停止：轻点锁存停车，长按 1.2 秒触发急停；支持 VoiceOver 停车和急停操作。
- 车辆：遥测过期后隐藏旧读数；电池告警仍由实际遥测驱动。
- 配对凭证：使用安全输入框，网关地址支持 IP 或主机名。

![新版驾驶页](doc/ui-home-redesign.png)

开发参数：`--tab 0..3` 指定初始页，`--no-alert` 抑制告警覆盖层（用于截图/联调）。
截图为模拟器未连接状态，不代表实车联调结果。

## 功能对照（固件模块 → iOS 实现）

| S3 遥控器（固件） | iOS（本工程） | 说明 |
|---|---|---|
| `proto/proto_frames.[ch]` | `S3Remote/Proto/Proto.swift` | CRC16-CCITT-FALSE、帧编解码、逐字节解析状态机（AA AA 55 容错/VER/FMT/CRC 错误分类），黄金向量与 C 实现字节级对齐 |
| 0x41 遥测 38 B LE | `S3Remote/Proto/Telemetry.swift` | 全字段解析 + `v1.5.0` 固件版本格式化 |
| `scr_link.c`（WS/JSON 面板） | `S3Remote/Link/LinkEngine.swift` + `Link/TextMessage.swift` | `hello{role,ver,tc,pair,ctrl}` 角色判定、`tc{on}`、1 Hz `{"t":"ping"}`→`pong` RTT、`err{auth}` 控制权失效、静默 10 s 看门狗强连、3 s 失败退避 |
| `POST /api/pair` 配对 | `LinkEngine.pair()` | 403/409/504/503 → 可动作的中文提示（doc/04 §4 口径），成功后存 token 并重连 |
| `scr_ctrl.c` 30 Hz 控制 | `S3Remote/Control/DriveController.swift` | 闸门（连接+CTRL）、ECO/NORMAL/SPORT 50/80/100 % 限幅、满行程 v=600 mm/s / ω=300 °/s、零位心跳、STOP 单击锁存、长按 1.2 s 急停 0x32 + 双锁存、RELEASE 后保持停止、控制权丢失冻结 |
| `safety_watch` | `S3Remote/Control/SafetyMonitor.swift` | 失联 1.2 s 去抖全屏告警 + 恢复事件、故障码、低电 20 %/临界 10 % + 5 % 回差 |
| 遥测丢包统计（spec 28） | `S3Remote/Control/TelemetryLossCounter.swift` | seq 缺口 Δ∈(1,1000) 记 Δ−1，loss ‰ |
| 电池显示防抖（C6 cee1189） | `S3Remote/Control/BatteryDisplayFilter.swift` | 电压：5 点中值 + 500 ms EMA + ≤2 Hz 显示闸 + 20 mV 下降迟滞；电量：5 点中值 + 持续 1.5 s 下降确认；会话内只降不升、重连不解除、零电压视为未就绪；告警/颜色仍用真实遥测 |
| 信号强度 | `S3Remote/Control/LinkQuality.swift` + UI `SignalBarsView` | 两级：网关在 hello 带可选 `rssi` 字段或周期发 `{"t":"rssi","dbm":N}` 时显示真实 dBm（分档与 S3 遥控器 Kconfig 同源：−60/−67/−75/−85）；否则用 RTT+遥测丢包合成 4 格预估（UI 标注"预估 · x"）。状态舱与诊断页常驻显示 |
| `app_state.c` | `S3Remote/Model/AppState.swift` | UI 单一事实源、600 ms 遥测过期（"--"）、事件环形日志、快照式发布 |
| LVGL P1/P2/P4/P5/P9 | `S3Remote/UI/*.swift` | Home（摇杆/大速度/STOP）、Vehicle、Diag（统计+配对+事件日志）、Settings、全屏告警覆盖层（急停 RELEASE / 失联自动清除 / 其余 ACK） |

## 页面

- **驾驶**：状态栏（连接/角色/TC/电量/RTT）、大速度、虚拟摇杆（死区可配、回弹）、
  T/S 与"实际发出"值、模式限幅、STOP（单击停止锁存 / 长按 1.2 s 急停，带进度提示）。
- **车辆**：电池 SOC/电压、左右目标/实测速度、本次/总里程、任务状态、故障码、
  运行时间、C6/TC275 固件版本。
- **诊断**：TX/RX 帧率、遥测丢包 ‰、RTT last/min/max、配对按钮与结果、事件日志。
- **设置**：网关地址（默认 192.168.4.1）、token、摇杆死区、默认模式、应用并重连。

## 构建与运行

要求 Xcode 26+（Swift 6），iOS 17.0+ 真机或模拟器：

```bash
open ios_remote/S3Remote.xcodeproj      # Cmd+R 运行（scheme 由 Xcode 自动创建）
# 命令行
xcodebuild -project ios_remote/S3Remote.xcodeproj -scheme S3Remote \
  -destination 'platform=iOS Simulator,name=iPhone 17 Pro' build
xcodebuild -project ios_remote/S3Remote.xcodeproj -scheme S3Remote \
  -destination 'platform=iOS Simulator,name=iPhone 17 Pro' test   # 60 项单测（含摇杆坐标映射回归）
```

工程无第三方依赖；`project.pbxproj` 为 Xcode 16+ 文件系统同步组格式，新增
Swift 文件放进 `S3Remote/` 或 `S3RemoteTests/` 目录即自动入编。

> 手写 `.xcscheme` 的 BuildableReference 必须带 `BuildableIdentifier="primary"`，
> 缺失会让 Xcode 26 解析时直接崩溃（`IDEScheme schemeFromXMLData` SIGTRAP，
> 本仓库开发时踩过，现已入库修正版共享 scheme `xcshareddata/xcschemes/`，
> CLI `-scheme` 行为确定）。

## 版本管理与 CI

- **版本真源**：`S3Remote.xcodeproj/project.pbxproj` 的 `MARKETING_VERSION`
  （构建号 `CURRENT_PROJECT_VERSION` 随包不随版）。App 关于页读 Bundle 信息，
  升版只有一处改动：

  ```bash
  just ios-version v1.2.0    # 改 MARKETING_VERSION → 同步写 ios_remote/CHANGELOG.md → 提交
  just tag ios v1.2.0        # 建 tag ios/v1.2.0（同固件工程口径）
  git push origin ios/v1.2.0 # 触发 Release workflow
  ```

- **CI 门控**（`.github/workflows/ios-remote.yml`）：push/PR 触及 `ios_remote/**`
  即在 macOS runner 跑全部 Swift 单测；发版不受未测代码污染——
  `ios-remote-release.yml`（tag `ios/v*` 触发）先复用该 CI 再建 GitHub Release。
- **发版产物**：与 tc275 同口径不附二进制（签名证书绑定开发机，runner 产不出
  可安装包），从 tag 自行 `just ios-install`。
- 发版记录见 `CHANGELOG.md`（Keep a Changelog 格式）。

## 联调步骤

1. iPhone 加入 C6 softAP（台架默认 `SD-DEV000` / `sddev123456`），App 首次
   使用需允许"本地网络"权限（`NSLocalNetworkUsageDescription` 已配）。
2. 车侧长按配对键 3 s 开窗 → 诊断页 **PAIR** → token 自动保存并重连取得 CTRL
   （与 S3 遥控器同口径；手机 Web 是备用端，不抢占）。
3. 摇杆驾驶；**STOP 单击**立即停车锁存，**长按 1.2 s** 发 0x32 急停并全屏锁定，
   按 RELEASE 后仍需触摸摇杆才恢复运动（spec 105）。
4. 失联/故障/低电按等级全屏告警；链路断开即停发 DRIVE，TC275 心跳看门狗自行停车。

## 已知限制

- **相机视频面未做**：固件 `scr_cam`（连 s3-gateway 的 JPEG-over-WS）属独立
  视频平面，v1 只做控制面；后续可加 `ASSEMBLE` 帧重组 + MJPEG 式解码渲染。
- **信号强度为预估**：iOS 公开 API 拿不到 Wi-Fi RSSI，App 用 RTT + 遥测丢包
  合成 4 格信号条（标注"预估"）；若 C6 在 hello 中附加 `"rssi":<dBm>` 字段或
  周期广播 `{"t":"rssi","dbm":N}`（softAP 侧 `esp_wifi_ap_get_sta_list()` 一行
  即可取到），App 自动切换为真实 dBm 显示，无需改动 App。
- token 存 UserDefaults（开发口径），上架建议迁 Keychain。
- App 退后台 WebSocket 即断，回前台由看门狗自动重连；驾驶请保持前台。
- 限幅比/死区/电池阈值为台架默认值，与固件同源（Kconfig 默认），**须实车标定**。
