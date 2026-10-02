# 更新日志

本项目的所有显著变更都将记录在此文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
并且本项目遵循[语义化版本](https://semver.org/lang/zh-CN/)。

## [未发布]

### 新增
- **Remote 视频面（scr_cam）+ CAMERA/VISION 页落地**（按 Remote 设计文档 §16 文件清单）：Camera WS 客户端（独立 `esp_websocket_client` 实例、`cam_monitor_task` 独占拨号、3 s 重拨节拍、Control 断链联动拆链）、`cam_frame.h` 20 B 头校验、esp_jpeg(TJpgDec) RGB565 解码、深度-1 pending + 3 帧槽最新帧环 + 一代延迟释放、10 Hz LVGL 泵；app_state 扩 cam/vision 字段组与告警槽；host 自测新增 `test_cam_frame` / `test_vision`
- Camera WS URI 带端口 `CONFIG_SCR_CAM_STREAM_PORT`（默认 81，对齐网关 `s3_camera` 流实例；此前按 LLDD 表 12 拨 :80 永远连不上）
- 暂停期 4 s keepalive ping：用 pong 采样本平面（Camera WS 自己那条连接）的 RTT 进 DIAG 页，并在串口打一行 `camera pong: rtt=..ms`，不开 CAMERA 页也能台架证明文本面往返通。**注意它不是续命租约**：`esp_http_server` 只在 socket 可读时进 handler，`recv_wait_timeout` 只是 accept 时设的 `SO_RCVTIMEO`，网关不会因入站静默回收空闲 WS 会话（此前 changelog 写的"守住网关入站超时否则单观众位被回收"是错的，已更正）

### 变更
- PROJECT_VER 升至 1.1.0（整车 1.1 基线，迎接 S3-CAM 替换 C6）
- **订阅即声明档位**：`cam_subscribe_now()` 在 `{"op":"subscribe"}` 之后无条件补发
  `{"op":"profile","name":..}`（原先只有配成 WEB 才发，REMOTE 走 `#else` 分支只打日志）。
  `framesize` 是网关传感器的一组全局寄存器、与 `/stream` 共用，静默 subscribe 等于接受网关自己的
  VGA，而 CAMERA 页的 320/640 radio 按说明书 106 只跟随回传尺寸——结果手持机 Kconfig 默认
  `SCR_CAM_PROFILE_REMOTE`（320×240）与网关默认 VGA（640×480）互相拧着，radio 永远指着 640 FULL。

### 修复
- **视频面永远 NO SIGNAL（blocker）**：WS 分片重组把 `ev->payload_len` 当本事件拷贝长度使用，而它是**整帧**总长（`data_len` 才是本片字节数）。`WS_RX_BUF=4096` < VGA JPEG 20–25 KB ⇒ 每帧拆 5–6 个事件，逐个越读堆约 19 KB 并让后片覆盖前片，SOI 侥幸可过、EOI 必挂 → `CAM_RX_JPEG_ERR` 持续计数。改取 `data_len` 后 `total = payload_offset + data_len` 成为真正的运行结束偏移，`total > ASM_CAP` 越界检查也随之成立（与 `scr_link.c` 控制面二进制路径一致）
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
