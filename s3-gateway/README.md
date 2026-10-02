# s3-gateway — SmartDrive ESP32-S3-CAM 网络协处理器固件

Freenove ESP32-S3-WROOM CAM（N16R8：16 MB flash + 8 MB octal PSRAM）+ OV5640 上跑的
SmartDrive 网络协处理器：softAP + WebSocket + 配对 + 双板 OTA 中继 + MJPEG 摄像头推流
（proto v2、CRC16、安全性裁决全部在 TC275）。

本工程由 [`../esp32c6_car`](../esp32c6_car)（ESP32-C6 版）**全量移植**而来：组件、协议面、
分区表、控制页保持一致，只把 C6 换成本板，并新增摄像头链路。C6 侧文档仍是设计基准
（[21-software-design.md](../tc275_car/doc/20-design/21-software-design.md) 量产 SDD、
[22-link-spi-design.md](../tc275_car/doc/20-design/22-link-spi-design.md) 板间 SPI/SF 帧、
[41-c6-docs-map.md](../tc275_car/doc/40-esp32c6/41-c6-docs-map.md) 跨仓 TODO T1–T6）。
**模块详细设计与完成状态：[`doc/`](doc/00-overview.md)**（每模块一份，随 C6 版一起复制，
其中 `04-link` / `14-sf-link` 的引脚口径以本文件为准）。

控制端两类客户端，协议面完全一致、零区分对待：手机 Web 控制页（`assets_src/`，烧入
assets 分区）与 ESP32-S3 LCD 遥控器（平级仓库 [`../smartcar_remote`](../smartcar_remote)，
proto v2 编解码原样复用本仓 `s3_proto/proto_frames.[ch]`）。

## 目录

```
main/            启动编排 app_main + app_state（组合根，唯一接线处）
components/
  s3_proto/      proto v2 编解码（纯 C 单一实现，TC275 侧同文件复用）
  s3_factory/    NVS 出厂数据（SN/密码/通道/预配对表/会话宽限）
  s3_link/       LINK 传输层：SPI 从机(spi_slave_hd 段模式) + 共享寄存器握手
                 + IRQ 数据就绪线 + 健康监测；v2↔SF 帧映射内置于 link
  s3_sf/         SF 帧编解码（SPI 链路专用容器，纯 C 主机可测，tc275_car doc 22 §5）
  s3_net/        softAP + Captive DNS(UDP53) + mdns_lite + Wi-Fi 事件
  s3_http/       httpd + WS 会话表 + assets 分区流式服务 + REST/OTA 端点
  s3_pair/       配对窗口跟随 + 会话 token（SHA-256 截断存储）+ 30s 宽限
  s3_bridge/     三台泵：命令/遥测广播(50Hz 邮箱+慢客户端降频)/OTA 中继(8×512B 信用窗)
  s3_ota/        自身 A/B：bundle 流式解析 + ed25519 验签(仅验签) + assets 更新 + 回滚
  s3_camera/     OV5640 采集 + MJPEG 推流（独立 httpd:81，见下节）
  s3_maint/      BLE DPT（CONFIG_S3_MAINT_BLE，默认关）
  s3_legacy/     TCP 8080 直通桥（CONFIG_S3_LEGACY_TCP，默认关）
assets_src/      控制页源码（实时画面 + 摇杆 + 50Hz 仪表/车速表 + 配对 + 双板 OTA）
tools/           build_assets.py · sign_bundle.py · ed25519_ref.py · gen_crypto_consts.py
test/host/       主机单测（proto 模糊 10^7 / sha512 / ed25519 RFC8032 / bundle）
```

C6 版里的 `c6_adxl345`（GY-291 台架三轴加速度计，仅 /diag 本地显示）在本工程**不移植**：
S3-CAM 的空闲脚要让给 SPI LINK，IMU 仍留在 C6 治具上。

## 引脚分配（本板唯一真源）

| 用途 | GPIO | 说明 |
|---|---|---|
| 摄像头数据 | 4–18 | XCLK15 · SIOD4 · SIOC5 · Y9..Y2=16,17,18,12,10,8,9,11 · VSYNC6 · HREF7 · PCLK13（等同 `CAMERA_MODEL_ESP32S3_EYE`） |
| USB-Serial-JTAG | 19 / 20 | 烧录与监视走原生 USB |
| SPI flash | 26–32 | 禁占用 |
| octal PSRAM | 33–37 | 禁占用（帧缓冲在此） |
| UART0（文档锚点） | 43 / 44 | 台架一般用不到，控制台默认走 USB |
| TC275 LINK SPI | SCLK40 · MOSI39 · MISO41 · CS42 | `CONFIG_S3_LINK_SPI_*_GPIO` |
| LINK 数据就绪 IRQ | 2 | 开漏 + 外部 10k 上拉到 3V3（22 §3.2） |
| WS2812 状态灯 | 48 | 板载单颗可寻址灯 |

TC275 侧信号名不变（P33.11 SCLK / P33.12 MTSR / P33.13 MRST / P23.4 SLSO5 / P23.0 IRQ），
只是接到上表的 S3 脚；改动集中在 `sdkconfig.defaults`，无需改代码。

## 构建（ESP-IDF v6.1）

macOS/Linux 用 EIM 安装的激活脚本（`export.sh` 在无 `python_env` 时会失败）：

```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
idf.py set-target esp32s3     # 首次；sdkconfig.defaults 已写死 target
idf.py build
idf.py -p /dev/cu.usbmodem11201 flash monitor
```

Windows 仍是 `idf.py build` / `idf.py -p COM7 flash`。

- 台架开发默认 `CONFIG_S3_FACTORY_DEV_OVERRIDE=y`：无出厂资料时以 `SD-DEV000` /
  `sddev123456` 启动。**量产必须设为 n**（严格走 FACTORY_WAIT）。
- 台架调试可在 menuconfig 调高日志等级（默认 WARN）；改了 `sdkconfig` 而不改
  `sdkconfig.defaults` 时，该设置只留在本机。

## build 产物与分区（16 MB flash）

| 产物 | 内容 | 去向 |
|---|---|---|
| `bootloader/bootloader.bin` | 二级引导 | `0x0` |
| `partition_table/partition-table.bin` | 分区表 | `0x8000` |
| `ota_data_initial.bin` | OTA 槽位标记初始值（指向 ota_0） | `0x19000` |
| `s3_gateway.bin` | **应用固件**（约 1.06 MB，烧入 ota_0/ota_1 槽） | `0x20000`（ota_0） |
| `assets.bin` | 控制页打包件（`tools/build_assets.py` 生成） | assets 分区 `0x620000` |
| `s3_gateway.elf` / `s3_gateway.map` | 调试符号/链接映射（panic 解栈），**不烧录** | — |

分区布局（真源 `partitions.csv`，LLDD 2.1，与 C6 版逐字节相同）：`ota_0`/`ota_1` 各 3 MB
双槽（A/B 升级 + 回滚），`assets` 512 KB 控制页，`coredump` 64 KB，`factory_ota_cache`
1 MB TC275 固件中继缓存，`nvs/nvs_cert/nvs_keys` 出厂与配对数据。16 MB 板尾部约 8 MB 未分配。

## 烧录方法

本板走板载 USB（USB-Serial-JTAG），**无需按 BOOT 键**，烧完自动复位运行。

> **捷径**：不想激活 IDF 环境时用根目录 `python flash.py`（或 Windows `flash.bat`）：
>
> ```
> python flash.py build         # 编译（含 assets 自动打包）
> python flash.py               # full（默认）：全量四件套
> python flash.py assets        # 仅重打包并烧控制页，固件不动
> python flash.py all -b -m     # 先 assets 后固件，-b 烧前编译，-m 开串口监视
> python flash.py mon           # 仅串口监视
> ```
>
> 均支持 `-p /dev/cu.usbmodem11201`（省略时按 USB VID 自动识别）。

```bash
# ① 全量烧录（新板 / 首次 / 分区表改动后）
idf.py -p PORT erase-flash          # 可选：整片擦除（连带清 NVS 配对/宽限）
idf.py -p PORT flash

# ② 日常开发（改了固件代码）：esptool 只写变化扇区
idf.py -p PORT flash

# ③ 仅更新控制页（改了 assets_src/，固件不动）
parttool.py -p PORT write_partition --partition-name=assets --input build/assets.bin

# ④ OTA 槽位复位（升级卡死/回滚循环时）：重写 otadata 回到 ota_0
parttool.py -p PORT write_partition --partition-name=otadata --input build/ota_data_initial.bin
```

免 IDF 环境（产线治具）：`python -m esptool --chip esp32s3 -p PORT -b 460800 write-flash @build/flash_args`

## 摄像头与 MJPEG 推流（s3_camera）

- 采集：OV5640 直出 JPEG（`PIXFORMAT_JPEG`，XCLK 20 MHz，2 帧缓冲在 PSRAM，
  `CAMERA_GRAB_LATEST`），档位与画质见 `CONFIG_S3_CAMERA_*`（默认 VGA / quality 12）。
- **推流走独立 httpd 实例（默认 `:81`）**：`esp_http_server` 单任务串行处理请求，
  若把阻塞的 MJPEG handler 注册到 80 端口那台，一个卡住的观看者会冻住控制页与
  所有 WebSocket 帧（遥控指令也在其上）。控制页只是 `<img src="http://<host>:81/stream">`。
- 同机不同端口对 `<img>` 无跨域限制；页面本身仍是同源 80。
- 一次只服务一个观看者（两个任务抢同一 2 帧队列只会各自丢帧），第二个请求返回 503；
  页面切后台会主动断开 `src` 让出通道。
- 启动顺序有硬约束：**探测/初始化在 `net_start()` 之前**（避免与 RF 上电峰值重叠，
  也把无摄像头时的 SCCB 扫描证据留在早期日志），**推流 httpd 在 `net_start()` 之后**
  （lwIP 起来之前 `httpd_start` 会踩 `tcpip_send_msg_wait_sem` 断言并反复复位）。
- 摄像头缺失不影响其余功能：`camera_start()` 只告警，`/stream` 全程 503。

## 双核分工（ESP32-S3）

S3 是双核，C6 版（单核）里"任务随便摆"的前提不再成立，本工程按
**核 0 = 面向 socket 的一切，核 1 = 面向引脚的一切** 显式绑核，理由与代价见
[doc/20-core-assignment.md](doc/20-core-assignment.md)：

| 核 | 承载 |
|---|---|
| 0（PRO） | WiFi 驱动(23) + esp_timer(22) + lwIP tcpip(18，已从浮动改为绑核) + 控制 httpd(5) + 推流 httpd(3) + DNS/mDNS/legacy TCP(4)；外设 ISR 也在此核 |
| 1（APP） | 相机采集 `cam_task`(23，`CONFIG_CAMERA_CORE1`) + SPI LINK(12) + 三台泵 bridge(10) + OTA 写盘(8) + 回滚确认(5) + WS2812(3) |

台架构建（`CONFIG_S3_BENCH_CTRL` + `CONFIG_FREERTOS_USE_TRACE_FACILITY`）开机末尾会打
`task map`，逐行给出 `任务名/core/prio/state/hwm`，用于核对上表。

## 固件签名（/ota/c6）

沿用 C6 版的线上标识：上传 URI 仍是 `/ota/c6`，bundle 魔数仍是 `C6FW`（改动需与
`tools/sign_bundle.py`、控制页同步，属于协议面）。验签公钥内嵌于
`components/s3_ota/keys/pub_ed25519_dev.bin`（私钥种子 `tools/keys/ed25519_dev.seed`，
仅台架用）。**量产前必须换产线密钥对**：

```bash
python tools/sign_bundle.py --c6 build/s3_gateway.bin --out build/c6fw.bundle
curl -F file=@build/c6fw.bundle "http://192.168.4.1/ota/c6?token=<控制端token>"
```

TC275 固件经 `POST /ota/tc275` 由 s3_bridge 信用窗口中继（0x60–0x65，LLDD §4.6.3）。

## 主机单测（G1 门）

```bash
cd test/host && make check
```

覆盖：proto 全分支 + CRC check 值(0x29B1) + 10^7 随机帧模糊；SHA-512 已知向量；
ed25519 RFC 8032 正/反向量 + dev 密钥端到端；bundle 签名/哈希/越界/截断。
（`test_sf` 覆盖 SF 帧；`s3_camera` 依赖目标机外设，不入主机测试。）

## 关键口径（与 LLDD 的差异见编码计划 C1–C10）

- 手机/WS 侧帧：`AA 55 VER(02) CMD SEQ LEN DATA CRC16-CCITT-FALSE`，LEN ≤ 64；
  板间 SPI 链路用 SF 帧（`5A 01 TYPE SEQ FLAGS LEN CID DATA CRC16`，LEN ≤ 248，
  4B 段对齐），映射表见 `doc/14-sf-link.md`
- 载荷一律显式小端（TriCore 大端 ↔ ESP32-S3 小端，禁止结构体直转）
- `0x63` = OTA_STATUS（`state==DONE` 即 END），`0x64` SWAP、`0x65` ABORT；
  UART 时代的 0x43 PING / 0x44 BAUD 已删除（doc 22 T2）
- 会话 token 32B 随机，仅存 SHA-256 前 16B 哈希（内存 + NVS 30s 宽限）
- LINK 接线以本文件"引脚分配"表为准（SCLK40 / MOSI39 / MISO41 / CS42 / IRQ2），
  量产时钟 5 MHz（22 §8 G5）；TC275 仍是 SPI 主控

## 台架验证状态（2026-10-02）

- ✅ `idf.py build`（v6.1，target esp32s3）产出 `s3_gateway.bin` 1.06 MB，ota_0 余量 66%
- ✅ 真机连续冷启动一致：`octal_psram 8MB / 80MHz` → `Detected OV5640 camera (0x3c)`
  → `cam config ok` → AP `SD-DEV000` + DHCP `192.168.4.1` + `mycar.local` →
  `s3_http: httpd up (v1.1.0)` → `cam: MJPEG stream on :81/stream` → `state=online`
- ✅ 双核绑核已由开机 `task map` 核对（`doc/20-core-assignment.md` §4 记录实测值）
- ✅ WS2812（GPIO48）驱动起、`s3_led` 心跳正常
- 🟩 未验证：手机侧画面/遥控（需要连上 AP 看）、TC275 联机（本板 SPI 脚已改，
  需按上表重新接线后测波形与 RTT）
- 🔴 未移植：`s3_adxl345`（见"目录"说明）
