# 19 摄像头采集与视频通道（s3_camera）

| 项 | 内容 |
|---|---|
| 代码位置 | `components/s3_camera/`（`camera_stream.c`、`include/camera_stream.h`、`Kconfig`、`CMakeLists.txt`、`idf_component.yml`）；页面侧 `assets_src/{index.html,app.js,style.css}` |
| 上游需求 | S3-CAM 移植新增，LLDD 无对应章节；`doc/SmartCar_S3CAM_OV5640_详细设计说明书_V1.0.md` 的 P2 相机面（表 12/14/15），偏差记录见 [21-spec-alignment.md](21-spec-alignment.md)；引脚约束见 [README 引脚分配](../README.md)，任务落核见 [20-core-assignment.md](20-core-assignment.md) |
| 状态 | 🟩 **代码完成 + 双仓构建通过**（`:81/stream` 与 `:81/ws/camera` 均已注册；8.2 文本平面 hello/subscribe/pause/profile/ping 已实装，`/diag` 已出相机字段）— 手机侧画面与 Remote 解帧待看 |

## 1. 职责与边界

唯一职责：把 OV5640 直出的 JPEG 送到两类消费者——浏览器的 `<img>`（multipart MJPEG）
与 S3 手持机（WebSocket 二进制帧，帧头 + JPEG）。

- **视频不上板间链路**：帧不进 proto v2 也不进 SF 帧，TC275 对本模块完全无感；
- **不占控制面 httpd**：本模块自带一台 `esp_http_server` 实例（默认 `:81`），
  与 `s3_http` 的 :80 只有"同一台机器、不同端口"的关系，没有共享 URI 表；
  视频的两个端点都在这台 :81 上，因为阻塞式 handler 绝不能挂到 :80（§5）；
- **不是可失败启动项**：无摄像头只降级为"两个端点常 503"，遥控/OTA/LINK 一律照常。

## 2. 硬件与引脚

Freenove ESP32-S3-WROOM CAM 的排针定义与 `CAMERA_MODEL_ESP32S3_EYE` 等价，引脚表真源在
`camera_stream.c` 的 `CAM_PIN_*` 宏：

| 信号 | GPIO | 备注 |
|---|---|---|
| XCLK | 15 | LEDC_TIMER_0 / LEDC_CHANNEL_0，20 MHz |
| SCCB（I²C 语义）SDA/SCL | 4 / 5 | 走新版 I2C 驱动、端口 1（`CONFIG_SCCB_HARDWARE_I2C_DRIVER_NEW` / `..._PORT1`），100 kHz，内部上拉 |
| DVP D0..D7 | 11,9,8,10,12,18,17,16 | 8 位并口（Y2..Y9） |
| VSYNC / HREF / PCLK | 6 / 7 / 13 | |

这条占用直接决定了本板的其它外设去处：**GPIO4..13 + 15..18 归摄像头**（14 是这段里唯一
空出来的脚），原生 USB 19/20、flash 26..32、八线 PSRAM 33..37 均不可动，于是 SPI LINK
挪到 39..42 + IRQ 2、WS2812 留板载 48，14/38/47 回到空闲（SD 存储已按决策移除，doc 21 §6）。

## 3. 采集与推流配置

配置真源是本组件的 `Kconfig`（choice `S3_CAMERA_FRAME_SIZE_ID` 直接选出 `framesize_t` 常量，
代码里不再有数字中转项）：

| 配置 | 默认 | 说明 |
|---|---|---|
| `CONFIG_S3_CAMERA_FRAME_SIZE_ID` | VGA | QVGA / HVGA / **VGA** / SVGA / HD 五档，按名字取 `FRAMESIZE_*` 常量；缩放与 JPEG 编码都在传感器里，档位同时决定链路负载。历史坑：旧版本把档位写成数字（QVGA=5/VGA=8），而本 vendor 树的 `framesize_t` 插入了 `128X128`、`320X320`（QVGA=6/VGA=10），数字会静默配成 CIF 400x296 |
| `CONFIG_S3_CAMERA_JPEG_QUALITY` | 12 | OV5640 gainly 刻度（越小越好），VGA 下 10..15 不致压满 SoftAP；允许 4..40 |
| `CONFIG_S3_CAMERA_STREAM_PORT` | 81 | 控制页里 `<img>` 指向 `http://<同一 host>:81/stream` |
| `CONFIG_CAMERA_TASK_STACK_SIZE` | 4096 | `cam_task`（IDF 组件提供）栈 |
| `CONFIG_CAMERA_CORE1` | y | 采集线程绑核 1，见 doc 20 §1 约束 2 |

代码内固定项（不开放为 menuconfig）：`PIXFORMAT_JPEG`（S3 无编码器，不做软件转码）、
`fb_count = 2` + `CAMERA_FB_IN_PSRAM`、`grab_mode = CAMERA_GRAB_LATEST`（丢旧帧保低延迟）、
**画面旋转 180°**（模组在车壳里是倒装的）。旋转走传感器侧 `set_hmirror(1)` + `set_vflip(1)`，
在 `camera_init()` 里 `esp_camera_init()` 成功之后一次性下发：`:81/stream` 与 `/ws/camera`
两条输出同时被纠正，且 OV5640 的 `set_image_options()` 在每次换帧尺寸时都会按
`status.hmirror/vflip` 重画这两位，handset 切 profile 也不会把旋转弄丢。

### 3.1 TCP 发送窗口是推流吞吐的上限

一张 VGA JPEG 20..60 KB，而 lwIP 每条连接最多能同时挂着 `TCP_SND_BUF_DEFAULT` 字节的
未确认数据。默认的 5760 = 4 个 MSS，于是每一帧都要拆成"发 4 段 → 停下等 ACK"的接力，
帧内耗时由 RTT 抖动决定——表现就是忽快忽停的卡顿，而不是稳定低帧率。
`sdkconfig.defaults` 因此显式抬到 14400（10 MSS，一帧的量能一次排进管道）：

```
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=14400
CONFIG_LWIP_TCP_WND_DEFAULT=14400
```

- 上限是 `TCP_SND_BUF / RTT`：VGA(q=12) 一帧 ~20..25 KB，传感器自己 25 fps 就要 0.5..0.6 MB/s。
  5760 窗口在 10 ms RTT 下正好只有 576 KB/s——**没有余量**，RF 一抖动（ACK 合并、
  重传、手机侧省电）就跌破传感器速率，多出来的帧被 `CAMERA_GRAB_LATEST` 丢掉，
  画面表现为"停一下再追一截"；14400 同一 RTT 给到 1.4 MB/s，约 2.5 倍余量，
  且一次有 10 段在飞，单个 ACK 迟到不会让管道空转；
- 排队的是 PSRAM（`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`），不吃内部 DRAM，
  抬窗口不会挤占 `cam_hal` 的 DMA 节点；
- 复验口径：开机日志 `wifi_init: tcp tx win: 14400`（改前是 5760）。

`send_wait_timeout = 2 s`（§5.2）与这个窗口是配套的：窗口够大时正常的帧几十毫秒就写完，
2 s 只可能来自真正拖不动套接字的客户端，踢得干净也不会误伤。

依赖只在 `components/s3_camera/idf_component.yml` 声明一次
（`espressif/esp32-camera ^2.0.0`）；`main/` 下再放一份会报 "Duplicate requirements"。
头文件目录是公开的（`INCLUDE_DIRS include`），`esp32-camera` 则是 `PRIV_REQUIRES`：
`app_main` 只看见本模块的四个接口，不看见传感器类型。

## 4. 启动时序（硬约束，勿重排）

```
app_main
 3b   camera_start()         探测 + 配置传感器      —— 必须在 net_start() 之前
 4    net_start()            softAP + esp_netif/lwIP
 5    http_start()           控制面 httpd :80
 5b   camera_stream_start()  推流 httpd :81         —— 必须在 net_start() 之后
```

- **推流端点必须在 lwIP 之后**：`esp_http_server` 起来需要协议栈，早于 `esp_netif_init()`
  调 `httpd_start()` 会踩 `tcpip_send_msg_wait_sem (Invalid mbox)` 断言并无限复位
  （真机踩过，见 CHANGELOG 修复条目）。
- **传感器探测放在 RF 上电之前**：避开 Wi-Fi 上电电流/中断峰值与 SCCB、DVP 初始化的重叠，
  同时把"无摄像头"的总线扫描证据留在早期日志里，方便区分"排线没插好"和"这块板子根本没接"。

## 5. 视频端点与并发

一台 :81 实例上两个端点，**共用同一套帧计数与查看者闸门**（`viewer_attach()` +
`frame_pump()` 取帧泵；`/ws/camera` 的出站泵是它的任务化变体，§5.2）：

### 5.1 `GET /stream` — 浏览器 `<img>`

- 每帧**两段**写出：`\r\n--<boundary>\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n`
  （合并成一个缓冲，`STREAM_MJPEG_HEAD`）→ 帧体；全部走 `httpd_resp_send_chunk`，响应头设
  `Cache-Control: no-store` 与 `Access-Control-Allow-Origin: *`。
  分成三段时那 ~70 B 的帧头会单独占一个段，多一次写也多一次往返的排队，帧头合并是免费的钱。

### 5.2 `GET /ws/camera`（文本面 + WSBIN）— S3 手持机

**会话模型（2026-10 双向化；握手回调见下）**：IDF 6.x 的 `esp_http_server` **不会**为升级
GET 调用 URI handler（`httpd_uri.c` 里回完 101 直接 `return ESP_OK`，注释
"If the request is websocket handshake, then do not call the uri->handler"），
所以会话生命周期挂在 `ws_pre_handshake_cb` / `ws_post_handshake_cb` 上
（`CONFIG_HTTPD_WS_{PRE,POST}_HANDSHAKE_CB_SUPPORT` 已在 `sdkconfig.defaults` 打开，
控制面 `s3_http` 同款）——把占位写成 handler 里的 `if (req->method == HTTP_GET)`
分支会静默失效：hello 不发、入站帧落进"`s == NULL` 就 `ESP_OK`"的分支，帧体留在 socket 里
被下一次头解析当成 opcode 读，于是刷 `WS frame is not properly masked` 并反复拆链。

- **出站泵 = 每会话一个 `cam_ws_tx` 任务**（核 0、prio 4，`post_handshake` 里创建）：独占该 socket 的**全部写**
  （hello、二进制帧、pong、1 Hz cam_state）。跨任务写只能用
  `httpd_ws_send_frame_async(handle, fd, frame)`——`httpd_req_t` 是每个请求复用的临时对象，
  存下指针就会把 JPEG 字节写进 httpd 当前服务的另一个 socket。该异步发送内部**头与体分两次
  `send()` 且不加锁**，所以本会话所有写统一过 `ws_send_frame()` 拿 `tx_mtx`（`WS_TX_LOCK_MS`）；
  单写者 + 一把锁是刻意的，控制面 `s3_http:ws_tx` 先踩过这个坑；
- **入站 = handler 本体**（只做入站，不写 socket）：客户端每发一帧文本进一次，
  解析 `{"op":...}` 并置事件位（subscribe/pause/ping/profile → `PUMP_*` 位）；
  CLOSE 置 `PUMP_CLOSE`；PING 回 PONG（RFC 6455 5.5.3 要求带原 payload 回显，
  因 `handle_ws_control_frames = true`，控制帧不再由 httpd 任务自动应答）。
  **不属于本会话的 fd、或超长/非文本帧一律返回 `ESP_FAIL` 挂断**——返回 `ESP_OK` 会把帧体
  留在队列里，正是上面那条 desync 的来源；
- 会话对象 `ws_session_t` 是**静态单例**（一次只服务一个手持机，无释放竞争）：槽位由
  **泵自己退役**（退出时清位、`fd=-1`、`in_use=false`）。手持机可能不发 CLOSE 就消失，
  因此 `pre_handshake` 遇到 `in_use` 时用 `httpd_ws_get_fd_info()` 判新旧——只有服务器
  仍认它是活 WS 会话才算被占用，否则等泵收尾（最多 3 s）后接管，日志
  `ws/camera reclaiming dead fd=N`；
- **死链发现靠写失败计数**：`ws_send_frame()` 维护 `tx_fail`，连续 `WS_TX_FAIL_MAX`(=3) 次失败
  即退役会话。未订阅的会话只跑 1 Hz cam_state，二进制发送永远不会失败，没有这个计数就只能
  等下一次重拨来接管（实测每 ~10 s 一次 hello/拆链的日志即由此收口）；
- 帧头编解码改用共享契约 `s3_proto/cam_frame.h`（`cam_frame_build`），与手持机
  `cam_frame_parse` 逐字节同源（与 /stream 一致：SEQ 跨查看者单调、TIMESTAMP_MS 为开机毫秒、
  keyframe 位每帧为 1）；
- 头与图必须**连续内存**才能塞进一条 WS 帧，故每连接持有一块 PSRAM 暂存缓冲
  （`4096 B` 余量，容量不足时才 realloc）；单帧拷贝量 ≈ 一张 VGA JPEG（20 KB 量级），
  在 240 MHz 上远低于发送耗时，不构成额外瓶颈。

### 5.4 文本平面 ops（LLDD 8.2 / 表 17）

| 入站（Remote→gw） | 动作 | 出站（gw→Remote） |
|---|---|---|
| `{"op":"subscribe"}`（及 `{"t":"cam_cmd","op":"start"}`） | 置订阅门 | — |
| `{"op":"pause"}`（及 `op":"stop"`） | 撤订阅门：帧继续抓还（保鲜）但记 `drop` 不发送 | cam_state `subscribed:false` |
| `{"op":"ping"}` | 请求探活 | `{"op":"pong","ts":..}`（RTT 进手持机 DIAG） |
| `{"op":"profile","name":..}`（`cam_cmd` 同形） | `REMOTE_PREVIEW`→QVGA、`WEB_PREVIEW`→VGA，`sensor->set_framesize` 只改寄存器不重启驱动；未知名/失败回 `{"t":"err","e":"bad"}` | 成功回一帧 cam_state（新 w/h） |
| 连接建立（`ws_pre_handshake_cb` 占位、`ws_post_handshake_cb` 起泵） | 泵第一件事即发 hello（手持机的状态机只有收到 hello 才进 SUBSCRIBING） | `{"hello":true,"t":"cam_hello","sensor":..,"profile":..,"w":..,"h":..,"ts":..}` |
| 每 1 s（泵内） | — | `{"t":"cam_state","sensor","profile","w","h","fps","seq","drop","subscribed","ts"}` |

- **订阅门初始为关**：握手即推流会让浏览器侧的 MJPEG 页（`/stream`）和 WS 页抢同一个
  2 帧队列，而手机页面从来不走 `/ws/camera`，所以 `pre_handshake` 一律 `subscribed=false`，
  只有手持机打开 CAMERA 页发 `{"op":"subscribe"}` 才开门。
- **档位只有一个真源**：`framesize` 是传感器的一组全局寄存器，`/stream` 与 `/ws/camera`
  共用，谁订阅谁负责声明。手持机在 subscribe 之后立刻补一条
  `{"op":"profile","name":..}`（档位取自它自己的 `CONFIG_SCR_CAM_PROFILE`），网关会话退役时
  把传感器恢复成自己的 `CONFIG_S3_CAMERA_FRAME_SIZE`（泵退出路径上的一次寄存器写）。
  两端各自留默认值必然拧着——旧行为是手持机 Kconfig 写 REMOTE_PREVIEW、线上却是网关的 VGA，
  而 UI 的 320/640 radio 按说明书 106 只跟随**回传尺寸**，于是 radio 永远指着 640 FULL。
  现在声明走线上、恢复走本地，UI 显示的即线上真实的。
- 未订阅期间泵仍每帧 grab+return——驱动 `CAMERA_GRAB_LATEST` 因此始终持有最新帧，
  re-subscribe 的首帧延迟 ≈ 0；这些帧计入 `cam_state.drop` 与 `/api/diag` 的 `wdrop`。
- **尺寸真源**：`esp32-camera` 不把 framesize→宽高表导出，而 hello/cam_state 可能在任何一帧
  被抓取之前就组包，所以 `frame_size_dims()` 显式列出可选档位，采集档位与 profile 切换都同步
  `s_width/s_height`。注意 `framesize_t` 的**编号在不同 vendor 树里会变**（本树插了
  `128X128`、`320X320`，QVGA=6、VGA=10），Kconfig 里用数字中转一次就会"菜单写着 VGA、
  实际配成 CIF 400x296"且毫无报错——现在由 choice 直接选 `FRAMESIZE_*` 常量，不再过数字。
- profile 切换期尺寸变化：在途一帧会被手持机按"尺寸变更重建帧槽"处理（Remote 文档 §5.3），
  网关侧不做静默窗口。

### 5.3 共同的并发与背压约束

- **一次只服务一个观看者**：`s_clients >= 1` 直接回 503 `stream busy - one viewer at a time`。
  两个任务抢同一个 2 帧队列只会各自丢一半帧，且第二个观看者会给第一个加延迟。
  注意这台实例只有一个任务：查看者占着 handler 时，第二个请求其实是在**排队**，
  等 handler 让出才吃到 503（不是立即拒绝）。`/ws/camera` 的闸是 `s_ws_session`
  占位（第二个 WS 升级直接 `ESP_FAIL` 拒绝，不排队），与 `/stream` 的 `s_clients`
  互不重叠：**注意这两把闸不互相排斥**——手持机的 WS 会话不会让手机 `/stream` 吃 503，
  反之亦然，代价是两条泵同时 `esp_camera_fb_get()` 抢那 2 帧队列、各自约拿一半帧
  （`/api/diag` 的 `view` 只统计 `s_clients`，WS 查看者不进这个数）。
- **客户端离场靠写失败发现**（没有独立心跳）：手机锁屏/切页不发 FIN，是下一次
  写返回非 `ESP_OK` 才退出循环并释放 `s_clients`。页面侧配合：切后台主动摘掉
  `img.src` 让出通道，回到前台再恢复。
- **丢到最新**就是驱动的 `CAMERA_GRAB_LATEST`：handler 阻塞在发送里时旧帧被驱动直接丢弃，
  下一次 `esp_camera_fb_get()` 拿到的即最新帧，因此没有第二条视频队列。
- 为什么必须独立实例：`esp_http_server` 是**单任务串行**处理请求的，阻塞的视频 handler
  若注册到 :80，一个卡住的观看者会冻住控制页与所有 WebSocket 帧——遥控指令就在那些帧上。
  这也决定了说明书表 12 的 `/ws/camera`（原列在 80 端口）在这里落在 81。

## 6. 失败模式

| 现象 | 判据（日志） | 处置 |
|---|---|---|
| 没插摄像头 / 排线方向错 | `esp_camera_init failed` → `SCCB bus has 0 ACKs` | 只告警；两个端点全程 503，其余功能不受影响 |
| 排线半通 / 换成其它传感器 | SCCB 有 ACK 但 `Camera PID` 非 `0x5640` | 同上；ACK 行会打印实际地址（OV5640=0x3c，OV2640=0x30） |
| SCCB 扫描被跳过 | `SCCB scan skipped (GPIO4/5 busy)` | 有别的组件占了 I²C 端口 1，检查引脚冲突 |
| PSRAM 耗尽 / 传感器停摆 | `frame grab failed (psram exhausted or sensor stalled)` | 看 `/diag` 的 `heap_min`、`psram_free` 与 `camera.drop` |
| WS 暂存缓冲申请失败 | `ws frame buffer N B: out of PSRAM` | 该帧作废并结束会话；`/stream` 不受影响（不占这块缓冲） |
| 查看者拖不动套接字（手持机中途断电/挂后台） | `ws/camera closed after N frames (err=...)`、`send stalled N times, last frame M ms` | 2 s 发送超时踢掉，通道回落空闲，下一个请求可占；页面侧 `onerror` 退避重连 |
| WS 会话空闲（手持机停在 DRIVE 页） | **服务端不会因此断开**：`recv_wait_timeout` 只是 accept 时给 socket 设的 `SO_RCVTIMEO`，而 httpd 只在 socket 可读时进 handler——一条静默的 WS 会话谁也不碰它。会话的真正出口是泵写失败计数（`3 writes in a row failed, retiring session`）和对端 FIN/RST（`Failed to read header byte (socket FD invalid)`） | 手持机 4 s 的 `{"op":"ping"}` 因此不是"按住超时"，它的价值是 RTT 采样与双向连通性证据；空闲会话占着唯一的观看者槽位，靠 `httpd_ws_get_fd_info()` 判死后来接管 |
| 文本面来帧过大 / 非 TEXT / JSON 坏 | `ws/camera: malformed text op dropped`（坏 JSON 只丢该帧）；>128 B 或非 TEXT 直接拆会话 |  oversized 无法只丢一帧还保持流同步（body 还压在 socket 上），拆链重连是唯一诚实处置 |
| 第二个 WS 客户端 | `ws/camera busy - one handset at a time` | 直接拒绝升级（不排队）；手机要画面就先让手持机断开或走 `:81/stream` |
| 传感器中途停摆（WS 会话期间） | `ws/camera: sensor silent 5 s, dropping session` | 结束会话释放占位；`/stream` 侧同样靠 grab 失败退出 |
| 传感器吐出非 JPEG | `unexpected non-JPEG frame format` | 理论不发生（`PIXFORMAT_JPEG` 固定）；出现即查配置回退 |

## 7. 观测

- 每 30 帧一行：`N frames, X.X fps, YY.Y KB/frame`；断开时 `mjpeg|ws/camera closed after N frames (err=...)`；
  写塞住时每 16 次一行 `send stalled N times, last frame M ms (worst K ms)`；
  文本面：`ws/camera op: <name> (fd=N)`（每个入站 op 一行——手持机暂停期每 4 s 的 ping 因此可见，
  不开 CAMERA 页也能证明入站路径真的在跑）、`ws/camera hello sent (OV5640 WEB_PREVIEW)`、
  `ws/camera profile -> REMOTE_PREVIEW`、`ws/camera: 3 writes in a row failed, retiring session`、
  `ws/camera reclaiming dead fd=N`、`ws/camera busy - one handset at a time`、
  `ws/camera: unknown op "..."`；
- 启动确认：`Detected OV5640 camera` → `cam config ok` →
  `sensor OV5640 up: frame_size=10 quality=12 640x480`（这里的数字是本 vendor 树里
  `FRAMESIZE_VGA` 的实际枚举值，不是 Kconfig 的中转数字——数字中转项已删）→
  `MJPEG stream on http://<ap>:81/stream, camera ws on ws://<ap>:81/ws/camera`；
  画面方向：台架上正放的物体在两个入口里都应显示为正（倒装已被 §3 的 180° 旋转抵消）；
- `/api/diag` 的 `camera{up,sensor,w,h,fps,frames,drop,stall,slow,view,wdrop}`（说明书 18.2）：
  `fps` 是 500 ms 滚动窗口且**无查看者时报 0**，`frames` 是累计推出去的帧数（页面判活就盯它），
  `drop` 是 `esp_camera_fb_get()` 返回空的累计次数，`stall`/`slow` 是 §3.1 的写卡顿计数与
  最慢一次发送耗时，`view` 即 `s_clients`，`wdrop` 是 `/ws/camera` 未订阅/坏帧而丢弃的
  在途帧数（即 cam_state 里那个 `drop`）；`/diag` 页面有"相机 OV5640"区块；
- 页面侧的自救（`assets_src/app.js`）：`<img>` 断流只报一次 `onerror`，不重连就永远黑屏，
  故 0.8 s 起指数退避重连（封顶 4 s）；连接没断、画面停住时 `<img>` 不报错，
  由每秒一次的 `/api/diag` 轮询看 `camera.frames` 是否推进，3 s 不动主动重开
  （控制面是另一台 httpd 实例、更高优先级，推流再忙也答得上这个请求）。
- 2026-10-02 实测：VGA 帧缓冲 2 × 23680 B 落 PSRAM，task map 里
  `cam_task core=1 prio=23 hwm=3340B`、推流 `httpd core=0 prio=3 hwm=3560B`。

## 8. 资源

| 项 | 值 |
|---|---|
| `cam_task` | 4096 B 栈，prio 23，**核 1**（IDF 组件创建） |
| 推流 httpd 任务 | 4096 B 栈（`HTTPD_DEFAULT_CONFIG`），prio 3，**核 0**，`ctrl_port = 端口+1000`（两台实例的 ctrl 端口必须不同，默认 32768 会撞） |
| 帧缓冲 | 2 × 23 680 B，PSRAM |
| WS 单帧暂存 | ≈ 帧长 + 4 KB，PSRAM，仅 `/ws/camera` 会话期间持有 |
| `cam_ws_tx` 出站泵任务 | 3072 B 栈，prio 4，**核 0**，每 `/ws/camera` 会话一个，会话结束自删 |
| WS 会话对象 | ≈ 64 B，内部 RAM（calloc），事件位组一份 |
| 最大 URI 数 | 2（`/stream` + `/ws/camera`，再加端点要同步抬 `max_uri_handlers`） |

## 9. 完成状态

| 项 | 状态 |
|---|---|
| OV5640 采集 + :81 MJPEG 推流 | 🟩 真机验证（探测、配置、服务起、无复位） |
| `:81/ws/camera` 二进制帧通道 | 🟩 代码完成（帧头改用共享契约 `s3_proto/cam_frame.h`）；🟥 握手与解帧未真机验证（冒烟片段见 doc 21 §3.3） |
| 8.2 文本平面（hello/subscribe/pause/profile/ping→pong/cam_state） | 🟩 代码完成 + 双仓构建通过；🟥 真机时序未测（hello→首帧、pause 让位、profile 切换重建帧槽） |
| 单查看者 503 / 切后台让出通道 | 🟩 代码完成；页面行为待手机复看 |
| 推流不卡顿（吞吐 + 自救 + 可观测） | 🟩 代码完成 + 台架复烧（开机日志 `tcp tx win: 14400`）；🟥 真机 fps/`stall` 曲线待手机连上 `SD-DEV000` 读数 |
| 推流预算项不被"顺手改回来" | 🟩 断言已接：`tools/check_streaming_budget.sh` 逐条比对生成的 sdkconfig（`TCP_SND_BUF/WND=14400`、`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`、`TCPIP_TASK_AFFINITY_CPU0`、`CAMERA_CORE1` 等 11 项），CI 与 Release 两个 workflow 都调它，本地发版前自查同一条命令。要重新调优：先改本文 §3.1 的算术，再改脚本那张表 |
| 推流下的遥控手感与 RTT | 🟨 待手机实测（绑核依据见 doc 20 §3） |
| `/diag` 摄像头状态字段 | 🟩 已接（`camera{}` 含 `frames/stall/slow` + `psram_free`，`/diag` 页面区块同步） |
| 多查看者 | ⚪ 明确不做（单通道低延迟优先，说明书 20.2 的 `MAX_CLIENTS=2` 未采纳） |
| profile 运行态切换（REMOTE_PREVIEW/WEB_PREVIEW） | 🟩 代码完成：`sensor->set_framesize` 寄存器级切换，不重启驱动；🟩 入站 ops 已台架证实可达（串口 `ws/camera op: ping (fd=N)` 每 4 s 一条 + 手持机 `camera pong: rtt=..ms`）；🟥 `subscribe`/`profile`/`pause` 三条只有开了 CAMERA 页才会发出，切换后的首帧、手持机帧槽重建、以及"手持机在场时手机 `/stream` 跟着降到 320×240"（Remote 默认档位声明所致，退出后恢复）均待用户点页验证。快照/录像仍随 SD 裁剪取消（§1.0 修订） |
