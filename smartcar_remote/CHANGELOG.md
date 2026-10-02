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
- **视频档位改为遥控器固定 320×240 + FULLSCREEN ×2 放大，删除 640×480 选项**：手持机不再向网关要 VGA（订阅时仍声明 REMOTE_PREVIEW，清晰度留给手机网页 /stream），侧栏两个 profile 按钮换成一个 FULLSCREEN 切换——LVGL `lv_image_set_scale` 2x 最近邻放大（关抗锯齿）绕图心铺满 640×480 列（条带隐藏、TAB 常驻覆盖底部），同帧率同空口，画质略软是 QVGA 全速率换来的已接受取舍；1x 时绕回窗口态。全屏是本地缩放，不设链路门控；VISION 页保持 1:1（结果叠加层按帧像素坐标映射，缩放会错位）
- **视频面解码提速 + 显示延迟减半**：esp_jpeg 弃用 ROM 固化的 TJpgDec（它对 RGB565 输出走逐像素 3→2 字节转换、哈夫曼无查表加速），改编译新版解码器——`JD_FASTDECODE=2` 表驱动哈夫曼（63 KB 工作区由 `scr_cam_start` 锁进内部 RAM，PSRAM 回退兜底，启动日志标注落位）+ `JD_FORMAT_RGB565` 原生 2 字节直写。CAMERA/VISION 页另立 33 ms 视频泵（`ui_camera_pump_video` / `ui_vision_pump_video`，帧槽 seq 门控让无新帧的 tick 零开销）：新帧平均 ~17 ms 上面板，替代原先最长 100 ms 的 UI tick 等待；NO SIGNAL 遮罩只在可见性边沿搬前景，30 Hz 空转不再搅动绘制顺序。`SCR_CAM_MAX_FPS` 显示上限 10→12，对齐传感器 QVGA 实际 ~11.5 fps
- **（台架决策，详见 sdkconfig.defaults 注释）PSRAM/flash 120 MHz 实验否决**：本批模块的 flash 颗粒无 HPM 支持（bootloader 启动即警告），120 MHz 下 WiFi/TCP 立即劣化（控制帧成片跳发、WS ping ~360 ms）——已回退 80 MHz；视频面的收益全部来自上面的解码器与泵配置，不在存储时钟
- **订阅即声明档位**：`cam_subscribe_now()` 在 `{"op":"subscribe"}` 之后无条件补发
  `{"op":"profile","name":..}`（原先只有配成 WEB 才发，REMOTE 走 `#else` 分支只打日志）。
  `framesize` 是网关传感器的一组全局寄存器、与 `/stream` 共用，静默 subscribe 等于接受网关自己的
  VGA，而 CAMERA 页的 320/640 radio 按说明书 106 只跟随回传尺寸——结果手持机 Kconfig 默认
  `SCR_CAM_PROFILE_REMOTE`（320×240）与网关默认 VGA（640×480）互相拧着，radio 永远指着 640 FULL。

### 修复
- **CAMERA→VISION 直接切页后 TAB 栏整条消失**：`ui_nav_open` 把新页面对象提到最前，而 `tabs_set_visible(true)` 只在"隐藏→可见"边沿重新置顶——HOME→CAMERA 会经历隐藏（置顶 ✓），CAMERA→VISION 时 TAB 本来就可见、边沿不触发，不透明页面就盖住了它（此前 640 档的 immersive 每次切页先藏后显，恰好每次都触发置顶，锁 320 后路径变直才暴露）。改为每次 on 都幂等置顶（已在顶层则跳过 move，10Hz 刷新零开销）
- **"置灰 + toast" 的 toast 是死代码**：LVGL 对 `LV_STATE_DISABLED` 对象根本不派发 `CLICKED`（`lv_indev.c` 的 `is_enabled` 前置判断，见 `indev_proc_release`），所以 §8.1 三 TAB、§8.2 profile、§8.3 VISION mode/drive 按钮一旦变灰，tap 进不到回调，写在回调里的原因 toast 永不触发——说明书要求的"置灰 + toast"实际只剩置灰。新增主题原语 `ui_set_blocked()` 承载**信息型门控**：保持可点、用递归 `OPA` 淡出（子 label 随父一起变暗，`lv_obj_get_style_opa_recursive` 沿父链相乘）、取消 PRESSED 高亮让手感仍是"按不动"；原因串改为单一真源函数（`cam_block_why()` / `vis_gate_why()` / `vis_drive_why()`），tap 出的 toast 与侧栏文案同源不再各说各话。VISION 的 AUTO 占位与 STALE  ASSIST 此前同样点不动，现在会解释"为什么不能用"。**标定/OTA/清故障（08 §5）不动**：那是安全型硬禁，必须继续用真 `LV_STATE_DISABLED`
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
