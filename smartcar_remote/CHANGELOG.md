# 更新日志

本项目的所有显著变更都将记录在此文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
并且本项目遵循[语义化版本](https://semver.org/lang/zh-CN/)。

## [未发布]

### 变更
- PROJECT_VER 升至 1.1.0（整车 1.1 基线，迎接 S3-CAM 替换 C6）

### 修复
- C6 OTA 期间不再弹全屏 RADIO LOST 告警：上传饿死遥测与"接受后重启等待重连"两个窗口内静默（scr_svc 新增 `scr_svc_ota_quiet_c6()`，90 s 重连宽限；上传失败/宽限超时后告警照常）
- TC275 断电（WS 仍连接）不再触发 RADIO LOST 全屏告警：告警只跟踪 S3↔C6 无线链路本身，车端离线由 SYSTEM 页 OFFLINE/STALE 与遥测 "--" 呈现（需配合 C6 侧遥测停播修复）

## [1.0.0] - 2026-10-01

首个稳定版，对齐 `CMakeLists.txt` PROJECT_VER 1.0.0。

### 新增
- About 页节点卡 tap 刷新固件版本：S3 本地 / C6 走 /api/health / TC275 走 `{"t":"tcver"}` 请求链（含超时与结果横幅）
- TC275 卡双行显示 APP + SBL 版本；信标缺失时 APP 回退遥测 fw_ver
- 版本号统一 v 前缀显示

### 修复
- hello.tc 占位字符串不再覆盖 tc_on（真实在线信标先于 hello 广播；遥测帧自证链路在线）
- WS 连续 3 次失败强制重连 Wi-Fi（应对 C6 重启后 STA 假在线）

## [0.1.0] - 2026-10-01

首个版本 tag，对齐 `CMakeLists.txt` PROJECT_VER 0.1.0（proto v2 对接 esp32c6_car + tcver 版本显示）。

### 新增
- 架构 v2 服务层基座：OTA 暂存分区 + core-0 服务任务；双核绑定与控制延迟路径
- LVGL 堆迁入 PSRAM + FIRMWARE/CALIBRATE/DIAGNOSE 三个服务页面
- HUD 风格整体改版：主页面板化 + 设置页指挥台；配对 UI 与设置功能增强
- 显示 TC275 APP+SBL 版本：tcver JSON 接入 + Diag 卡 SBL 行
- 触摸输入去抖与 UI 元素位置优化
- CI：ESP-IDF 构建门禁 + 固件体积/资源余量报告

### 变更
- WS 发送超时改为 150 ms（soak 09-30）
- 关闭 Wi-Fi NVS 持久化，避免连接期 flash 写干扰 RGB 供帧
- 真 240 MHz + XIP-from-PSRAM（RGB bounce 路径）
- 架构 v2 说明与中文用户手册

### 修复
- WS 发送超时 0 改为 5/20 ms，消除 DRIVE 掉帧与 ping 丢失
- HUD 改版后真机上的三处标签布局溢出
- Settings 页补返回 Home 的页头

[未发布]: https://github.com/lilicqyu-ship-it/smartcar_remote/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/lilicqyu-ship-it/smartcar_remote/releases/tag/v1.0.0
[0.1.0]: https://github.com/lilicqyu-ship-it/smartcar_remote/releases/tag/v0.1.0
