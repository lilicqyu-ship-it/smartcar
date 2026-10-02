# 更新日志

本项目的所有显著变更都将记录在此文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
并且本项目遵循[语义化版本](https://semver.org/lang/zh-CN/)。

## [未发布]

### 变更
- 本工程由 `esp32c6_car` 全量移植：目标芯片 ESP32-C6 → **ESP32-S3**（Freenove ESP32-S3-WROOM CAM，16 MB flash + 8 MB octal PSRAM），组件目录/符号统一 `c6_` → `s3_`，协议面（proto v2、SF 帧、`/ota/c6` 与 `C6FW` 标识）**零改动**
- SPI LINK 引脚改为空闲脚方案：SCLK40 / MOSI39 / MISO41 / CS42 / IRQ2，状态灯 WS2812 → GPIO48
- 不移植 `c6_adxl345`（GY-291 台架 IMU）：`/diag` 去掉 `"imu"` 字段与诊断页对应区块
- **microSD 存储不做**（2026-10-02 决策）：按说明书实现过一版 1-bit SD-MMC 物理层（`components/s3_sd`，CMD38/CLK14/D0 47 + FATFS 挂 `/sdcard`，无卡路径已真机验证 ~7 ms 超时退出），随后整块移除；`app_main` 步骤 3c、`/api/diag` 的 `sd{}`、诊断页存储区块与 `CONFIG_S3_SD_*` 全部退回，省回 ~70 KB（FATFS + SDMMC）。保留的技术结论记在 `doc/21-spec-alignment.md` §6，录像/抓拍/gallery（P3）随之停在未开始
- 任务摆放从"调度器自由浮动"改为显式绑核（单核 C6 的前提在双核 S3 不再成立）：**核 0 = 面向 socket 的一切**（WiFi/esp_timer 由 IDF 钉住，故 lwIP `tcpip` 改为 `AFFINITY_CPU0`，两台 httpd 与 DNS/mDNS/legacy 一律 `core_id=0`），**核 1 = 面向引脚的一切**（`cam_task` 经 `CONFIG_CAMERA_CORE1` 挪出核 0，link/bridge/ota/rb_chk/led 绑核 1）；分配依据、接缝代价与实测 task map 见 [doc/20-core-assignment.md](doc/20-core-assignment.md)
- 纳入 smartcar monorepo 版本管理；PROJECT_VER 升至 1.1.0 对齐整车基线；`.gitignore` 公钥例外路径随组件更名修正（`c6_ota` → `s3_ota`），修复 `pub_ed25519_dev.bin` 被静默忽略
- **`framesize` 是网关与手持机共用的一个全局寄存器组，谁订阅谁声明、退出即恢复**：`/ws/camera` 会话退役（泵退出路径）时把传感器写回本机 `CONFIG_S3_CAMERA_FRAME_SIZE`。此前手持机默认不声明档位，网关就留在被上一条 bug 配错的尺寸上，手机 `/stream` 会跟着长期降级；现在两端各自的默认只在"没人声明"时生效（Remote 侧对应改动：订阅后立刻发 `{"op":"profile"}`）
- 入站 ops 打一行 `ws/camera op: <name> (fd=..)`：手持机暂停期每 4 s 的 `{"op":"ping"}` 因此在串口上可见，不开 CAMERA 页也能证明文本面双向已落地
- **视频画面旋转 180°**：模组在车壳里是倒装的，`camera_init()` 在 `esp_camera_init()` 成功后一次性下发传感器侧 `set_hmirror(1)` + `set_vflip(1)`（两者同时开即 180°，非镜像）。修在传感器而不是某一条输出，故 `:81/stream` 与 `/ws/camera` 一起转正；OV5640 换帧尺寸会按 `status.hmirror/vflip` 重画这两位，handset 切 profile 不会丢旋转

### 新增
- `s3_camera`：OV5640 直出 JPEG + MJPEG 推流，**独立 esp_http_server 实例（默认 :81）**——阻塞的视频 handler 不能挂到 80 端口那台，否则会冻住控制页与所有 WS 帧；单查看者通道，第二个请求 503
- 控制页"实时画面"卡片：按需开关 `http://<host>:81/stream`，页面切后台自动让出通道
- 台架开机 `task map` 日志（`CONFIG_S3_BENCH_CTRL` + `CONFIG_FREERTOS_USE_TRACE_FACILITY`）：逐行打印 `任务/core/prio/剩余栈`，用于核对双核分工表
- `s3_camera` 视频第二条通道 `ws://<host>:81/ws/camera`：WS 二进制帧 = 20 字节小端帧头（MAGIC `CA 56` / VERSION / FLAGS·bit0 keyframe / SEQ / TIMESTAMP_MS / WIDTH / HEIGHT / JPEG_LEN）+ JPEG 体，与 `/stream` 共用同一条取帧循环和同一道单查看者门禁；新增 `camera_stats()` 供 `/api/diag` 消费
- `ws://<host>:81/ws/camera` **8.2 文本平面落地**（LLDD §8.2 / 表 17）：会话改为双向——握手 handler 转做入站（`subscribe/pause/ping/profile`，同时接受 `cam_cmd` 同形），每会话一个 `cam_ws_tx` 出站泵任务独占全部 socket 写（hello、二进制帧、`{"op":"pong"}`、1 Hz `cam_state`）；订阅门控推流（暂停期抓了还、计入 `drop`，re-subscribe 首帧≈0）；profile 运行态切换走 `sensor->set_framesize` 寄存器路径（REMOTE_PREVIEW→QVGA、WEB_PREVIEW→VGA）；会话槽位由泵自己退役，对端失联未发 CLOSE 时用 `httpd_ws_get_fd_info()` 判死后接管。详见 doc/19 §5.2/§5.4
- 共享契约落位本仓：`contracts/camera/cam_frame.h`、`contracts/vision/vision.h` 副本进 `components/s3_proto/`，`camera_stream.c` 帧头改用 `cam_frame_build()`（本地重复定义删除），`scripts/check-contracts.sh` 纳入双仓逐字节校验；契约同步新增 `VISION_T_ERR`、`CAM_WS_OP_SUBSCRIBE/PAUSE/PING/PONG_NAME` 解析常量
- `/api/diag` `camera{}` 新增 `wdrop`（`/ws/camera` 未订阅/坏帧丢弃数）
- `/api/diag` 新增 `psram_free` 与 `camera{up,sensor,w,h,fps,drop,view}` 字段，S3 诊断页对应新增"相机 OV5640"区块（说明书 18.2）
- 说明书对齐记录 [doc/21-spec-alignment.md](doc/21-spec-alignment.md)：逐项登记与《SmartCar_S3CAM_OV5640_详细设计说明书_V1.0》的一致处、偏差处与延后项（帧头 20 B 而非文中说的 16 B、WS 落在 :81 而非 :80、不做 `vehicle_*` 更名等，附理由）
- **CI 与 Release workflow 补齐**（monorepo `.github/workflows/s3-gateway.yml`、`s3-gateway-release.yml`）：入库的 `sdkconfig.defaults` 是台架口味，新增 `sdkconfig.prod` 叠加层显式关掉 `S3_FACTORY_DEV_OVERRIDE` / `S3_BENCH_CTRL`。CI 矩阵把**两种口味都编**（只编一种必漏一种：只编台架 → 发布物带后门；只编生产 → 台架专用 `#if CONFIG_S3_BENCH_CTRL` 分支悄悄编不过），生产那份断言后门确实关着，两份都断言推流预算与核分工的 sdkconfig 键（`TCP_SND_BUF=14400`、`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`、`TCPIP_TASK_AFFINITY_CPU0`、`CAMERA_CORE1` 等，清单在 `tools/check_streaming_budget.sh`，Release 也调同一条——tag 可以打在 CI 未跑绿的提交上），再走 `tools/ci_size_report.py` 大小门禁 + host 单测 + 控制页 JS 语法。Release 由 tag `gw-s3/vX.Y.Z`（`just tag gw-s3`）触发，发版前置 host 门禁、并校验镜像 `esp_app_desc_t` 里的 `PROJECT_VER` == tag（本工程版本号是写死的 `set(PROJECT_VER ...)`，不是 git describe）；产物 `s3_gateway.bin` / `assets.bin` / 合并单文件镜像（合并件只到 app 末尾，不含 `0x620000` 的 assets 分区，否则得到 ~6.4 MB 稀疏文件）

### 修复
- **`/ws/camera` 的会话从未建立**（手持机视频面拿到 hello 之前的最后一道墙）：ESP-IDF 6.x 的
  `esp_http_server` 在 `httpd_uri.c` 里回完 101 就 `return ESP_OK`，**不再调用 URI handler**
  （源码注释 "If the request is websocket handshake, then do not call the uri->handler"），
  所以把会话占位写在 `handler` 的 `if (req->method == HTTP_GET)` 分支等于没写：hello 永远不发，
  入站帧走进"无会话即 `ESP_OK`"的分支、帧体压在 socket 上，下一次头解析把负载字节当 opcode 读，
  表现为 `httpd_ws: WS frame is not properly masked` 反复刷 + 会话 1.6~4.7 s 一拆。
  生命周期改挂 `ws_pre_handshake_cb`（占位/拒升级）与 `ws_post_handshake_cb`（101 已上线才起泵、
  设 `SO_SNDTIMEO`/`TCP_NODELAY`），与已验证的控制面 `s3_http` 同款；handler 只做入站，
  非本会话 fd / 超长 / 非 TEXT 一律 `ESP_FAIL` 挂断而不是静默 `ESP_OK`（后者正是 desync 的来源）。
  两个回调开关 `CONFIG_HTTPD_WS_{PRE,POST}_HANDSHAKE_CB_SUPPORT` 早已在 `sdkconfig.defaults` 里
- **`ws_send_hello()` 是死代码**：函数写好了却没有任何调用点（`-Wunused-function` 才暴露），
  手持机的状态机只有落 hello 才进 SUBSCRIBING，于是永远停在等待态。现在泵启动第一件事即发 hello
- **跨任务写 WS 用了失效的 `httpd_req_t`**：泵任务存下握手那次请求的 `req` 指针再调
  `httpd_ws_send_frame(req,…)`，而 `req` 是每请求复用的临时壳（一台实例只有一个 `hd_req`），
  结果是往 httpd 当前服务的**另一个 socket** 写 JPEG 字节。改为
  `httpd_ws_send_frame_async(handle, fd, frame)`，并把该 socket 的全部写（hello/二进制/pong/cam_state/PONG）
  收进 `ws_send_frame()` 统一持锁——异步发送内部头与体是两次 `send()` 且不加锁，两个写者按字节交错
  即对端眼里的"坏帧"
- **采集档位被数字中转吃掉，菜单 VGA 实配 CIF 400x296**：`Kconfig` 的
  `S3_CAMERA_FRAME_SIZE` 隐藏 int 项（QVGA=5/HVGA=7/VGA=8/SVGA=9/HD=11）是 C6 时代对着旧
  `framesize_t` 写的，本 vendor 树插入了 `FRAMESIZE_128X128` 与 `FRAMESIZE_320X320`
  （现 QVGA=6、VGA=10），而驱动对越界数字不作任何校验——于是"选了 VGA"实际跑 CIF，
  hello/cam_state 的宽高查不到只能报 `0x0`、profile 名退回 `STREAM_PREVIEW`。
  删掉数字中转项，choice 直接映射 `FRAMESIZE_*` 常量（`CAM_FRAME_SIZE`），
  `frame_size_dims()` 同步 profile 切换；真机核对：`sensor OV5640 up: frame_size=10 quality=12 640x480`，
  手持机侧 `camera hello: sensor=OV5640 640x480`
- **失联手持机会把会话槽位焊死**：对端掉电/切页不发 CLOSE 时，未订阅的泵只跑 1 Hz cam_state，
  原先只有二进制写失败才退泵——它永远等不到。现在 `ws_send_frame()` 统计连续失败，
  达 `WS_TX_FAIL_MAX`(3) 即退役会话，日志 `3 writes in a row failed, retiring session`；
  下次拨号另有 `httpd_ws_get_fd_info()` 判死接管兜底
- 相机推流 httpd 必须在 `net_start()` 之后启动：早于 lwIP 起来会踩 `tcpip_send_msg_wait_sem (Invalid mbox)` 断言并反复复位
- **视频流卡顿/黑屏**（保证"稳定不卡顿"的一揽子改动，依据与口径见 [doc/19 §3.1、§5、§7](doc/19-camera.md)）：
  - 根因是每条连接的 TCP 发送缓冲只有默认 5760 B（= 4 MSS）——一张 VGA JPEG 20..25 KB 必须拆成"发 4 段等一轮 ACK"的接力，RF 一抖动就跌破传感器速率、多余帧被 `CAMERA_GRAB_LATEST` 丢掉，表现为"停一下再追一截"。`sdkconfig.defaults` 与现网 `sdkconfig` 把 `LWIP_TCP_SND_BUF_DEFAULT`/`TCP_WND_DEFAULT` 抬到 14400，开机日志 `tcp tx win: 14400` 可核对
  - 控制页 `onerror` 只把画面标灰、从不重连：一次 503、一次板子复位或 Wi-Fi 抖动就把"实时画面"永久留在黑屏上，这是最难看的"卡死"。现在 0.8 s 起指数退避重连（封顶 4 s），并在连接未断而画面停住时（`<img>` 不会报错）用 `/api/diag` 的 `camera.frames` 判活，3 s 无推进主动重开
  - 推流 handler 在 `esp_camera_fb_return()` 之后仍读 `fb->len` 累计字节数：缓冲描述符已交回驱动、可能正被采集任务改写，改为返回前先取长度
  - `send_wait_timeout` 3 s → 2 s：拖不动套接字的查看者占着这台实例唯一任务的窗口减半，正常帧的写在调窗后是几十毫秒量级，不会误伤
- `flash.py` 自动选串口不再猜：遥控器同为 ESP32-S3，两块板的原生 USB-Serial-JTAG 描述符完全相同（303A:1001），旧逻辑按最低 COM 号取第一个 Espressif 口，两块同插时可能把网关固件烧进遥控器。现在同 VID 组出现多候选直接报错要求 `-p`；要一条指令走对板用 monorepo 认板入口 `python ../firmware/fw.py flash gw-s3`（读 flash 里的 `esp_app_desc_t` 认工程）
- **`/ws/camera` 那一版其实从未编译成功**：ESP-IDF 6.x 把 cJSON 移出 in-tree 组件（改由 registry
  的 `espressif/cjson` 提供），`s3_camera` 里 `PRIV_REQUIRES cJSON` 让 cmake 配置阶段就
  `Failed to resolve component 'cJSON': unknown name` 整场构建失败；同一版新增的
  `ws_apply_profile()` 还用了旧帧尺寸枚举名 `QVGA`/`VGA`（现名 `FRAMESIZE_QVGA`/`FRAMESIZE_VGA`）。
  补 `components/s3_camera/idf_component.yml` 的 `espressif/cjson` 依赖（解析结果锁进
  `dependencies.lock`）、`PRIV_REQUIRES` 改 `espressif__cjson` 并修正枚举名；台架与生产两种口味
  均构建通过（1.02 MB / ota_0 余量 66%），台架已重烧当前树并冷启动复验
- TC275 断电后遥测缓存仍以 50 Hz 永久重播（`mbox_fresh` 从不清除）——客户端新鲜度看门狗被假帧一直喂着，网页与 S3 SYSTEM 页永远显示车端在线；改为消费制：每帧 SPI 遥测只转发一次，链路掉线即停（S3 的丢帧率/接收速率统计也不再计入重播帧）

## [1.0.0] - 2026-10-01

首个稳定版，对齐 `CMakeLists.txt` PROJECT_VER 1.0.0。

### 新增
- 按需版本查询：WS `{"t":"tcver"}` → SPI DIAG 0x53/0x24 中继至 TC275（200 ms 限流，配合 S3 About 页 tap 刷新）
- TC275 版本信标转发：EVT 0x24/0x25 经 DIAG 隧道广播 `{"t":"tcver"}`（配合 tc275_car 版本上链路）
- ADXL345 三轴加速度计（位拍 SPI），集成进 /diag 诊断页
- 台架标定页 /calib.html 与 EVT 0x22/0x23 解码；标定流程重排为四步 + jog 故障门禁
- 控制页体验：深色座舱主题、iOS 手势加固、车速表与 EMA 平滑、角色可见/回切即连/心跳看门狗
- /diag HTML 诊断页 + socket 表自愈（僵尸回收器）
- 可选 captive portal（`CONFIG_C6_CAPTIVE_PORTAL`，默认关弹窗改手动输网址）
- flash 脚本按 USB VID 自动识别板载串口，串口被占用时早报错

### 变更
- 工程名 c6_car → esp32c6_car，统一仓库名/工程名/产物名
- 统一 LF 换行（.gitattributes），保证 contracts 跨平台逐字节校验一致
- flash.py 跨平台 EIM 环境发现；lwIP 槽位提升到 32
- 启动时先打印上次复位原因
- smartcar_remote（S3 遥控器）登记为第二控制端
- CI：固件资源统计门禁（对齐 smartcar_remote）

### 修复
- WS 发送互斥 + PING/PONG 应答，对接 S3 遥控器
- 控制命令处理防陈旧帧；DRIVE 队满时丢弃新帧，规避 queue-set 幻影项断言重启
- 禁用 Nagle 算法，控制/遥测帧即时发送
- WS CLOSE 帧显式回收会话
- send_json 状态行补完整状态码（仅传原因短语产生 "HTTP/1.1 OK" 非法行，esp_http_client 拒绝解析，S3 /api/health 超时）
- .vscode compile-commands-dir 指向当前检出路径

## [0.1.2] - 2026-09-27

### 修复
- WS 重连退避与 err 提示自动恢复；lwIP TIME_WAIT 由 2 min 缩短到 20 s——修复手机端网页刷新后打不开、控制误报 busy

## [0.1.1] - 2026-09-27

### 新增
- CI：GitHub Actions（主机测试 + 固件编译 + tag 自动发版）

### 修复
- 链路交互简化（SPI 断连类故障根因修复）：SEQ 窗口自愈、TX 卡死重排与 RX 重排重试
- 静默断连的快速回收与 WS 发送反压

## [0.1.0] - 2026-09-27

首个版本化基线（台架联调）。

### 新增
- 板间链路换向 UART → SPI 从机；SPIDBG 链路诊断与复位后首帧序号误判修复
- Web 控制页、captive portal 302 兜底、WS2812 状态指示灯
- 弱电源台架缓解三件套 + 串口抓取工具（真机 bring-up）
- flash.bat / flash.py 一键烧录（full/assets/all 模式）
- 主机单测套件（G1 门 26/26 绿）与 14 份模块设计文档

[未发布]: https://github.com/lilicqyu-ship-it/esp32c6_car/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/lilicqyu-ship-it/esp32c6_car/compare/v0.1.2...v1.0.0
[0.1.2]: https://github.com/lilicqyu-ship-it/esp32c6_car/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/lilicqyu-ship-it/esp32c6_car/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/lilicqyu-ship-it/esp32c6_car/releases/tag/v0.1.0
