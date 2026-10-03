# 更新日志

本项目的所有显著变更都将记录在此文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
并且本项目遵循[语义化版本](https://semver.org/lang/zh-CN/)。

## [未发布]

### 新增
- **整车拓扑页**：iPhone → Wi-Fi/WS proto v2 → C6 → SPI/SF 帧 → TC275 节点链，
  链路状态实时点亮（WS/TV 通道、两级 RTT、SPI 误码），TC275 外设芯片
  （xcore 命令队列/DFlash 标定/电机编码器）与 s3-gateway 视觉网关虚线占位
- **芯片版本清单**：本 App（Bundle）/ C6 固件（hello ver）/ TC275 App（tcver
  信标，遥测 fw_ver 兜底）/ TC275 SBL（tcver）/ 硬件 rev——零固件改动；
  AppState 新增 tcver 存储并随断链清零

### 修复
- App 版本号链路：Info.plist 的 CFBundleShortVersionString 改用
  `$(MARKETING_VERSION)` 占位（此前硬编码 1.0.0，`just ios-version` 升版到不了包内）

## [1.1.0] - 2026-10-03

### 新增
- **座舱 HUD UI 大改版**：弧线速度表盘（渐变弧/刻度/红区/状态章）、遥测呼吸环摇杆
  （外环按链路健康脉动、死区环可视化）、T/S/v/ω 芯片行、图标化模式选择器、
  危险斜纹 STOP（长按进度横扫）、径向 SOC 仪表、RTT 走势图、呼吸辉光告警覆盖层；
  全局触感反馈（STOP 按压升级/模式选择/急停 error 震动）
- **电池显示防抖**（对齐 C6 cee1189 策略）：电压 5 点中值 + 500 ms EMA + ≤2 Hz
  显示闸 + 20 mV 下降迟滞；电量 5 点中值 + 持续 1.5 s 下降确认；会话内只降不升、
  重连不解除、零电压视为未就绪；告警与颜色仍用真实遥测
- **信号强度显示**（两级）：网关提供 hello `rssi` 字段或 `{"t":"rssi","dbm":N}`
  时显示真实 dBm（分档 −60/−67/−75/−85 与 S3 遥控器 Kconfig 同源）；否则按
  RTT + 遥测丢包合成 4 格预估（UI 标注"预估"）
- **App 图标**（CoreGraphics 脚本绘制：发光摇杆 + 底座 LED + 信号弧），
  `ios_remote/tools/gen_icon.swift` 可复现，`just ios-icon` 一键重生成

### 变更
- 版本管理接入 monorepo 口径：版本真源 pbxproj `MARKETING_VERSION`（`just ios-version`
  升版），关于页读 Bundle；tag 前缀 `ios/vX.Y.Z`，Release workflow 复用 CI 门禁

## [1.0.0] - 2026-10-03

### 新增
- 首版：S3 遥控器（smartcar_remote）核心功能的 iOS 复刻——proto v2 帧编解码
  （CRC16 黄金向量与 C 实现字节级对齐）、WS 链路（hello/tc/pong/err 文本面、
  10 s 静默看门狗、3 s 退避）、配对 REST（403/409/504/503 → 动作提示）、
  30 Hz DRIVE 心跳与 ECO/NORMAL/SPORT 限幅、STOP/急停双锁存语义（对齐
  scr_ctrl.c）、失联 1.2 s 去抖 + 电池 20/10 % 回差告警、遥测丢包统计；
  驾驶/车辆/诊断/设置四页 + 全屏告警覆盖层；60 项主机单测
