# 21 详细设计说明书对照与偏差

| 项 | 内容 |
|---|---|
| 代码位置 | `components/s3_camera/camera_stream.c`、`main/{app_main,app_state}.c`、`components/s3_http/http_server.c`、`sdkconfig.defaults` |
| 上游需求 | `doc/SmartCar_S3CAM_OV5640_详细设计说明书_V1.0.md`（下称**说明书**，V1.0，2026-10-02 入库）；引用一律给表号/节号 |
| 状态 | 🟩 **P2 相机面已落地** — 偏差集中在引脚、帧头长度、端口与命名四处，均有意为之；**microSD 存储明确不做**（见 §6） |

本文只做三件事：说明书与当前代码的**逐条落地状态**、**有意的偏差及其理由**、**明确推迟的部分**。
不重述说明书内容。接手 S3 板级/链路时先读这篇，再读 [19-camera.md](19-camera.md) 与 [20-core-assignment.md](20-core-assignment.md)。

## 1. 说明书的三条边界仍然成立

- **ADR-001 / ADR-008**：S3 替换 C6，不动 TC275 的实时闭环与 PWM 权限 → 协议面（proto v2、SF 帧、`/ota/c6`、`C6FW` 标识）零改动，已落实；
- **ADR-002**：视频面与控制面隔离 → 落实为**独立 `esp_http_server` 实例 + 独立端口**，见 §3.2；
- **ADR-004**：`contracts/link` 原样 → 未动一行。

## 2. 引脚：说明书表 11 已被台架方案替代（有意偏差）

| 信号 | 说明书表 11 | 本仓实现 | 出处 |
|---|---|---|---|
| SPI SCLK | 14 | **40** | `CONFIG_S3_LINK_SPI_SCLK_GPIO` |
| SPI MOSI | 21 | **39** | `CONFIG_S3_LINK_SPI_MOSI_GPIO` |
| SPI MISO | 47 | **41** | `CONFIG_S3_LINK_SPI_MISO_GPIO` |
| SPI CS | 42 | 42（一致） | `CONFIG_S3_LINK_SPI_CS_GPIO` |
| SPI IRQ | 1 | **2** | `CONFIG_S3_LINK_SPI_IRQ_GPIO` |
| SD CMD/CLK/D0 | 38 / 39 / 40（4-bit 模组） | **无 —— 存储未启用** | §6 |

不能照抄表 11 的三条硬约束：

1. **不可让的脚**：板载 flash 26..32、八线 PSRAM 33..37（N16R8 封装内连线，占用即启动失败）、
   相机 DVP 4..13 + 15..18、native USB 19/20（**烧录口与台架串口本体**）、CH343 调试串口 43/44、
   板载 WS2812 48、strapping 0/3/45/46（45/46 还兼 `VDD_SPI` 选择，本模块不可动）。
2. **表 11 内部自相冲突**：它给 SPI 的候选（14/21/47/42/1）与给 SD 的候选（38/39/40）互不相让，
   而 39/40/41/42 是本板唯一一段既避开相机、PSRAM、flash，又避开 strapping 的**连续四脚**，
   SPI LINK 拿走它成本最低。
3. **14 是 DVP 区间里唯一空出来的脚**（相机映射等于 `CAMERA_MODEL_ESP32S3_EYE`，用到 13 与 15，跳过 14）——
   它曾被 SD CLK 占用，SD 移除后回到空闲。

**空闲可选脚**（日后加外设从这里取）：1、14、21、38、45/46 除外的表外脚；
其中 47 与 38 因 SD 移除而空出。**注意**：`gpio_num` 1/2 都不是 S3 的 strapping 脚，
说明书对 1 的顾虑与对 2 的排除都无技术依据，只是台架已经按 2 接好 IRQ。

> **复确认（2026-10-02）**：SD 移除后表 11 已无冲突方，两条路都走得通；决定**保持
> SCLK40 / MOSI39 / MISO41 / CS42 / IRQ2**，不回退到说明书表 11 的 14/21/47/42/1。
> 依据：台架杜邦线已按现方案接好、IRQ2 的开漏 + 外部上拉语义在 C6 上验证过，
> 换脚只带来重铺线与重测波形的成本，零收益。真要改只动 `sdkconfig.defaults` 五行。

## 3. 相机面：落地内容与两处偏差

### 3.1 与说明书一致的部分（表 24 的 P2 主体）

- 采集：OV5640 **直出 JPEG**，`fb_count=2` + `CAMERA_FB_IN_PSRAM` + `CAMERA_GRAB_LATEST`
  —— 即表 15 的"取最新 JPEG、不等待旧帧"，背压由驱动丢旧帧完成，不再有第二条队列；
- 运行态观测：`/api/diag` 新增 `camera{up,sensor,w,h,fps,frames,drop,stall,slow,view}`（18.2），
  `fps` 为 500 ms 滚动窗口、无查看者时报 0，`drop` 是 `esp_camera_fb_get()` 返回空的累计次数；
  在 18.2 的字段之外补了 `frames`（累计推帧，页面判活盯它）、`stall`/`slow`（单帧写 socket
  ≥100 ms 的次数与最慢一次耗时）——目的是把"看着卡不卡"变成可核对的数字；
  同时新增 `psram_free`；`/diag` 页面加了"相机 OV5640"区块；
- 帧头/命令 schema 走独立通道，不进 proto v2（ADR-003）。

### 3.2 表 14 帧头是 20 字节，不是 16（协议级偏差）

表 14 字段累加 `2+1+1+4+4+2+2+4 = **20 B**`，而说明书正文与口头口径写作"16 字节"。
实现**以字段表为准**（`CAM_HDR_LEN 20`，全小端，`put_le16/put_le32`）：

```
off  size  field
0    2     MAGIC 0xCA 0x56
2    1     VERSION 0x01
3    1     FLAGS      bit0 = keyframe
4    4     SEQ        单调帧号（跨查看者累计，复位后从 1）
8    4     TIMESTAMP_MS（开机起 ms）
12   2     WIDTH
14   2     HEIGHT
16   4     JPEG_LEN
20   N     JPEG
```

理由：Remote 解帧读的是**字段**，把长度凑成 16 要么砍掉 `WIDTH/HEIGHT`、要么把 `SEQ` 缩成 2 字节，
两者都是协议变更，必须走 `contracts/` 双边同步，不能各自"兼容"。若后续定稿为 16 B，
本文件的这段就是唯一需要改的落点。

`FLAGS` 的 keyframe 位**每帧都置 1**：MJPEG 式逐帧独立 JPEG 本来就没有帧间依赖，
置 0 反而是错的。事件帧位（表 14 的 event）等 P3 录像/抓拍落地后再用。

> **2026-10-02 更新**：`contracts/camera/cam_frame.h` 已入库（Remote 侧同步按它写 RX），
> 头长定为 **20 B**、字段偏移与上面完全一致，另给出 `CAM_FLAG_EVENT 0x02`、
> `CAM_JPEG_LEN_MAX 65536`、`CAM_SEQ_GAP_MAX 512` 与解析返回值。**本仓已改为直接消费该共享头**：
> 副本落位 `components/s3_proto/cam_frame.h`（与 `proto_frames.h` 同目录同惯例），
> `camera_stream.c` 的 TX 走 `cam_frame_build()`，本地 `CAM_MAGIC_*/put_le*` 重复定义已删除；
> `scripts/check-contracts.sh` 已把两份副本（Remote `main/proto/` + 本仓 `s3_proto/`）纳入逐字节校验。
>
> **2026-10-02 更新（8.2 文本平面）**：`contracts/vision/vision.h` 同样落位 `s3_proto/`，
> 并在契约里补了解析用常量：`CAM_WS_OP_SUBSCRIBE/PAUSE/PING/PONG_NAME`、`VISION_T_ERR`。
> `/ws/camera` 的入站 ops（subscribe/pause/ping/profile）与出站
> hello/pong/cam_state 按此实现，语义见 doc/19 §5.2/§5.4。

### 3.3 `/ws/camera` 开在 :81，不开在 :80

表 12 把 Camera WS 与 Control WS 都列在 80 端口。`esp_http_server` 是**单任务**服务：
一个持续 `send` 的 WS handler 会把 80 那台的任务占死，控制页、遥测、所有 WS 帧一起停摆——
这正是当初把 MJPEG 拆到 :81 独立实例的原因（`doc/19` §5 的 `tcpip_send_msg_wait_sem` 断言是同一条约束的另一面）。
因此按"另开一条 WS 端口"落地：

| 端点 | 用途 | 说明 |
|---|---|---|
| `ws://<ap>:81/ws/camera` | Remote 手持机（表 12 要求的 WS 二进制帧通道） | 一帧 = 20 B 头 + JPEG |
| `http://<ap>:81/stream` | 手机浏览器 `<img>`（说明书未覆盖，但网页必需） | multipart MJPEG |

- **一次只服务一个观看者**，但两台闸不同：`/stream` 占 `s_clients`（后到者 503），
  `/ws/camera` 占静态单例 `s_ws`（后到的 WS 升级在 `ws_pre_handshake_cb` 里直接拒绝）；
  同一时刻手持机与手机画面二选一；对端消失却没发 CLOSE 时，用
  `httpd_ws_get_fd_info()` 判旧槽是否还活着，死了就接管（`reclaiming dead fd=N`）；
- `send_wait_timeout = 2 s`（默认 5 s）：拖不动套接字的查看者被踢掉，通道回落空闲——
  手持机中途断电、页面挂后台这类情形的兜底；窗口够大时正常一帧几十毫秒就写完，
  2 s 只可能是真正卡死的客户端；泵的每 socket `SO_SNDTIMEO` 另收紧到 500 ms，
  配合 `tx_fail` 三连败退役会话（见 doc 19 §5.2）；
- ~~`recv_wait_timeout = 5 s` 会回收静默会话~~ **更正**：它只是 accept 时给 socket 设的
  `SO_RCVTIMEO`，httpd 不在入站静默时进 handler，所以空闲 WS 会话不会被它回收。
  手持机每 4 s 的 `{"op":"ping"}` 留着是为了 RTT 采样与双向连通性，不是为了续命；
- 说明书未提的吞吐前提：lwIP 每条连接的发送缓冲默认 5760 B = 4 MSS，是"忽快忽停"的根因，
  `sdkconfig.defaults` 把 `LWIP_TCP_SND_BUF_DEFAULT`/`TCP_WND_DEFAULT` 抬到 14400
  （依据与算式见 `doc/19` §3.1，开机日志 `tcp tx win: 14400` 可核对）；
- 说明书 20.2 的 `S3CAM_CAMERA_WS_MAX_CLIENTS=2` **未采纳**，仍为 1：
  2 帧队列 + 2.4G 带宽下两个查看者只会互相拖慢并放大丢帧；
- 端口 :81 的 httpd 优先级 `tskIDLE+3`，低于 80 那台的 `tskIDLE+5`（视频永不拖控制，doc/20 §2）。

冒烟测法（连上 `SD-DEV000` 后在手机浏览器控制台，或任意 WS 客户端）：

```js
var w = new WebSocket('ws://' + location.hostname + ':81/ws/camera');
w.binaryType = 'arraybuffer';
w.onmessage = e => { var d = new DataView(e.data);
  console.log(d.getUint32(4, true), d.getUint16(12, true) + 'x' + d.getUint16(14, true),
              d.getUint32(16, true), 'B jpeg, total', e.data.byteLength); };
```

文本面（8.2）在同一条连接上验证：`e.data instanceof ArrayBuffer` 为 false 的行就是
hello/cam_state/pong；**订阅门初始是关的**（握手即推流会和 `/stream` 抢同一个 2 帧队列），
所以先 `{"op":"subscribe"}` 才有 binary 帧，`{"op":"pause"}` 看停、再 subscribe 看恢复，
`{"op":"ping"}` 应立刻回 `{"op":"pong","ts":..}`，
`{"op":"profile","name":"REMOTE_PREVIEW"}` 看 cam_state 的 w/h 变小且画面尺寸跟随：

```js
w.onmessage = e => { if (typeof e.data === 'string') console.log(e.data); };
w.onopen = () => { w.send('{"op":"subscribe"}'); w.send('{"op":"ping"}'); };
```

### 3.4 明确推迟（说明书 P3 及之后）

`/api/camera/snapshot`、`/api/camera/config`、`vision{}`、Assist——
它们都依赖 §9 的 **Frame Hub**（一次采集、多路带租约消费）。当前 2 个 framebuffer 的所有权完全在推流手里，
先做 snapshot 会直接从流里偷帧；录像落盘同理。**下一块该做的是 Frame Hub，不是这几个端点。**
（运行态 profile 切换已随 8.2 文本平面落地，走 `sensor->set_framesize` 寄存器路径，不再等 Frame Hub。）

### 3.5 profile 出厂档偏差

表 7 的 `REMOTE_PREVIEW` 是 320×240@8-10 fps；本仓出厂选 **VGA 640×480**（`FRAMESIZE_VGA`，
本 vendor 树里枚举值 10）+ `quality=12`，已台架确认可稳定出帧。QVGA 通过
`CONFIG_S3_CAMERA_FRAME_SIZE_QVGA` 一档切换，无需改代码。
**与手持机的两个默认已在 2026-10-02 定死规则**：Remote 出厂默认 `SCR_CAM_PROFILE_REMOTE`（320×240）、
网关默认 VGA，而 `framesize` 是共用的一组全局寄存器——曾出现过"Kconfig 写着 REMOTE、线上跑 640、
UI radio 只能跟着回传尺寸显示"的拧巴态。现在的契约是**谁订阅谁声明、退出即恢复**：手持机 subscribe
后立刻补一条 `{"op":"profile"}`，网关在 `/ws/camera` 会话退役时写回自己的 `CONFIG_S3_CAMERA_FRAME_SIZE`
（见 doc/19 §5.4）。

## 4. 核分工：表 19 与 doc/20 的取舍

| 任务 | 说明书表 19 | 本仓实现（doc/20，已台架核对） |
|---|---|---|
| vehicle/link | 核 0，prio 12 | **核 1**，prio 12 |
| bridge | 核 0，prio 10 | **核 1**，prio 10 |
| camera 采集 | 核 1，prio 9 | **核 1**，prio **23**（`esp32-camera` 固定） |
| 视频发送 | 核 0，prio 5 | 核 0，prio 3（:81 httpd） |
| 控制 WS | 核 0，prio 8 | 核 0，prio 5（:80 httpd） |

差异只有一处实质：**SF/SPI 侧放核 0 还是核 1**。

- 表 19 是 C6 单核语义的直译。S3 上若把 link 放核 0，它就要和 `wifi`(prio 23)、`tcpip`(18) 抢同一个核，
  RF 突发期间 SPI 轮询必然被拖——而 SF 的 500 ms 静默看门狗是硬约束；
- doc/20 把 link/bridge 挪到核 1，代价是与 `cam_task`(23) 同核且低于它，已在 doc/20 §3 留了两个旋钮
  （降帧尺寸 / 抬 `LINK_TASK_PRIO` 到 20 以上）。

**结论：保持 doc/20 的分配，不回退。** 判定依据是推流下的 LINK RTT 分布与丢帧率（表 26 的 G3/G5），
不是表 19 的数值。表 19 的优先级数值仍作为参考基线采纳（bridge 10 / link 12 与实现一致）。

## 5. 命名：不执行 `vehicle_*` 改名（有意偏差）

表 23 要求 `c6_link → vehicle_link`、`CONFIG_VEHICLE_LINK_SPI_*`、新工程目录 `esp32s3_cam_car`。
本仓落地为 `components/s3_*` + `CONFIG_S3_*`，工程目录 `s3-gateway/`。

理由：改名对行为零收益，却会把刚在台架核对过的绑核点、引脚默认值、日志锚点全部重排一遍；
`s3_` 前缀已经表达了"芯片位"这一层语义（说明书的 `vehicle_` 表达的是"职责位"，两者不冲突，
下表给出映射即可）。接手 Remote / TC275 侧时按它找文件：

| 说明书名 | 本仓实际 |
|---|---|
| `components/vehicle_link` | `components/s3_link` + `components/s3_sf` |
| `components/vehicle_bridge` | `components/s3_bridge` |
| `components/vehicle_net` / `_pair` / `_ota` | `s3_net` / `s3_pair` / `s3_ota` |
| `components/camera` | `components/s3_camera` |
| `components/camera_ws` | `s3_camera/camera_stream.c` 里的 `/ws/camera` handler（未独立成组件） |
| `components/camera_record` | **未创建**（依赖 Frame Hub，且落盘目标 SD 已取消，见 §6） |
| `components/vision` | **未创建**（P4/P5） |
| `components/http` | `components/s3_http` |
| `CONFIG_VEHICLE_LINK_SPI_*` | `CONFIG_S3_LINK_SPI_*` |
| `esp32s3_cam_car/` | `s3-gateway/`（monorepo 子目录，与 `esp32c6_car/` 并存） |
| 表 23 里 `contracts/camera`、`contracts/vision` | `contracts/camera/cam_frame.h` **已入库**（帧头 schema，见 §3.2 更新块）；`contracts/vision` 未创建 |

## 6. microSD 存储：明确不做（2026-10-02 决策，实现已回退）

说明书把 SD 定为"录像、抓拍、事件缓存"的落点（表 5、§12、表 24 的 P3 前置）。本轮按 **SD-MMC 1-bit**
落地过一版物理层（CMD38 / CLK14 / D0 47、FATFS 挂 `/sdcard`、开机 mount 在相机之后 `net_start()` 之前），
真机验证过无卡路径：挂载 ~7 ms 超时退出、一行 WARN、网络与相机不受影响。随后按你的口径**整块移除**
（`components/s3_sd/` 已删，`app_main` 步骤 3c、`/api/diag` 的 `sd{}`、诊断页"microSD 存储"区块、
`sdkconfig.defaults` 的 `CONFIG_S3_SD_*` 全部退回，binary 从 1.07 MB 回到 0x103530 ≈ 1.01 MB，ota_0 余量 66%）。
它从未入库，移除前的副本暂存 `/tmp/s3_sd_removed_20261002/`（重启即失，需要时按上表重写更可靠）。

保留下来的技术结论（真要重启存储时不必再查一遍）：

| 项 | 结论 |
|---|---|
| 总线选型 | S3 的 SDMMC 支持 GPIO 矩阵（`SOC_SDMMC_USE_GPIO_MATRIX=1`、`SOC_SDMMC_NUM_SLOTS=2`），**任意脚都能做 1-bit SD-MMC**；不必退到 SDSPI，更不该占用 SPI2（LINK 在用） |
| 配置 | `host.flags = SDMMC_HOST_FLAG_1BIT` + `slot.width = 1`（`SDMMC_HOST_DEFAULT()` 默认按 8/4/1 + DDR 全开）；d1..d3 置 `GPIO_NUM_NC`；`SDMMC_SLOT_FLAG_INTERNAL_PULLUP` 在杜邦线台架上是必要的 |
| 引脚冲突 | 说明书给 SD 的 38/39/40 与给 SPI 的 14/21/47/42/1 **互斥**（39/40 撞 MOSI/SCLK）；本板无空闲 4-bit 组，1-bit 是唯一可行形态 |
| 挂载语义 | ESP-IDF 无"盘符"概念，VFS 挂载点即根目录（`/sdcard`）；`esp_vfs_fat_sdmmc_mount` 的 `format_if_mount_failed` **必须为 false**——读不了的卡上可能有用户录像，绝不自动格盘 |
| 容量读数 | `esp_vfs_fat_info()` 的 FAT 空闲扫描在 1-bit 卡上可能是百毫秒级，**绝不能在请求路径里调用**，需缓存 + 由落盘任务按 TTL 刷新 |
| API 坑 | v6.1 无 `sdmmc_card_get_name()`；legacy `driver/sdmmc_host.h` 是组件 `esp_driver_sdmmc` 的公开 include 目录，REQUIRES 需写 `esp_driver_sdmmc fatfs sdmmc esp_timer` |
| 时机 | 插卡电流峰与 RF 电流峰不叠加——放在 `net_start()` 之前，与 doc/19 §3 同一判据 |

连带影响：**P3 的录像/抓拍/gallery 失去落盘目标**，snapshot 若要留图只能走"内存环形缓冲 + WS 单帧"这一类
不落盘方案；`/api/diag` 不再有 `sd{}`。表 24 的 P3 阶段因此整体停在"未开始"。

## 7. 落地状态（对齐表 24 的阶段）

| 阶段 | 项 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| P0 | OV5640 点亮、引脚确认 | 🟩 | 台架 `cam: sensor OV5640 up: frame_size=8 quality=12` |
| P0 | SD 物理层 | ⬜ 明确不做 | 2026-10-02 决策移除，见 §6 |
| P1 | C6 等价替换（SoftAP/`/ws`/配对/SPI/遥测/驾驶） | 🟩 | doc 04/05/06/07/08 |
| P2 | 相机面：MJPEG + WS 二进制帧 + **8.2 文本平面** + diag 字段 | 🟩 代码完成 | `:81/stream` 与 `:81/ws/camera` 注册日志；🟥 手机画面、Remote 解帧、hello→首帧/pause/profile 时序、`drop` 计数待测 |
| P3 | snapshot / 录像 / gallery | ⬜ 未开始 | 前置：Frame Hub（说明书 §9）＋ 存储目标（已取消，见 §6） |
| P4 | line / color / QR + `vision` JSON | ⬜ | 同上；`vision_task` 未创建；**落位政策：核 1**（[doc/22](22-vision-resource-policy.md)） |
| P5 | Assist（`omega = clamp(omega_remote + Kx·e_x + Kθ·e_θ)`） | ⬜ | 依赖 P4；权限模型已符合 ADR-008（视觉只出高层指令）；**经 bridge/SF 下行**（doc/22） |
| P6 | ESP-DL 目标检测 | ⬜ | PSRAM 预算未评估；**先过 doc/22 §4 预算清单再开** |
| P7 | OTA / 故障 / 老化 | 🟨 | 双分区自更新与 TC275 relay OTA 是 C6 基线既有能力；G6 老化未做 |
| — | 表 21 故障矩阵 | 🟡 | 相机侧已具备判据（`camera.up`、`drop`、`view`），存储项随 SD 取消，控制面断链策略沿用 C6 基线未改 |

## 8. 下一步（按依赖排序，不是按说明书顺序）

1. ~~**Remote 侧接 `:81/ws/camera`**~~ → 两端已齐（Remote 按 20 B 头解帧 + 订阅/pause/ping，
   网关按 8.2 回 hello/cam_state/pong），**只剩真机对拉验证**；
2. **手机侧复看**：`/stream` 画面与切后台让出通道、`/ws/camera` 握手解帧与文本面 ops（§3.3 两段冒烟片段）；
3. **Frame Hub**（说明书 §9）：一次采集、带租约多播给 JPEG/vision——它是 P3/P4 的共同前置，
   存储取消后落盘一路不再需要它，但 snapshot 与 vision 仍要从同一帧缓冲取数；
4. **G3/G5 HIL**：推流下测 LINK RTT 分布与丢帧率，再决定 doc/20 §3 的两个旋钮是否要动；
5. ~~表 14 帧头定稿（16 还是 20）~~ → **已定 20 B**（`contracts/camera/cam_frame.h`）；
   剩下的动作是本仓改成消费该共享头，而不是继续维护本地重复定义。
