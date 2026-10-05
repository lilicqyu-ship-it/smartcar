# 更新日志

本项目的所有显著变更都将记录在此文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
并且本项目遵循[语义化版本](https://semver.org/lang/zh-CN/)。

## [未发布]

### 变更
- **tc275_car 速度环串口实时整定链路（台架示波器）**：ASCLIN0 控制台新增行命令解析器
  `app/console.c`（`BENCH on|off|?`/`HELP`，精确匹配、超长行整行丢弃），替代回环 echo
  任务（`UART_echoTask`/`vUartEchoTask` 移除——回环检查由任意命令的应答行承担）；CPU0
  经新跨核开关 `XCORE_benchLogSet/Active` 遥控 CPU1 的 `MOTOR_ALGO_diag`：开时以 100 Hz
  推出紧凑 `SRVB` 行（`XCORE_logi` 空格格式：uptime + 左右目标/实测/duty/积分×10，约
  60 B ≈ 6 KB/s @115200，低于线速，uptime 列可发现整行丢弃），无视空闲门以保持连续
  时间轴，关时恢复原 5 s `[SERVO]` 行为（`test_log_policy` 同步扩 bench 断言；新主机
  单测 `test_console`，CI 加跑）。MATLAB 侧 `matlab_motor_model/live_serial_plot.m`
  （`just matlab-live`）实时绘制四格波形（左右速度目标/实测、duty、积分项），关窗自动
  存 `results/live_serial_*.csv` 供离线对照 `tune_pid_grid` 预扫；PID 增益仍为编译期
  常量（`servo.h`），金标流程不变
- **tc275_car 看门狗假死根治（v1.1.2）**：TC27x 的 CPU 看门狗到期**只向本核发 NMI、不会自行复位**
  （TC27x UM §7.44），而 iLLD 默认的 NMI 陷阱钩子是空宏直接返回——v1.1.0 的"喂狗断流 → 复位回
  安全态"这条底线**从未真正闭合**，看门狗到期后核被 NMI 打死、复位永远不来，整车冻结（"假死"，
  2026-10-03 台架复现）。修复：`Configurations/Ifx_Cfg.h` 定义 `IFX_CFG_CPU_TRAP_NMI_HOOK` →
  `IfxCpu_triggerSwReset()`（`SWRSTCON` 无密码寄存器写，NMI 上下文唯一安全的复位原语，钩子不
  返回；禁止在其中打印/走 xcore——会死锁），栈溢出钩子与排障手册的"断喂复位"叙事自此成立。
  同批更正 `bsp/wdg.h` 窗口注释：`REL=0xF800` 按官方锚点 0xE000≈1.3 s 线性外推是 **~1.4 s**，
  旧稿"0.3~0.5 s"与自己的换算锚点算术矛盾（源注释 Cpu0/Cpu1_Main 同步）。真源 21 号升 V1.13、
  31 号 V2.7、32 号 V1.10、33 号 V1.5、00-index 同步；**复位窗口与 NMI 复位行为待台架实测**；
  Release (TASKING) 配置仍因 34 §12 Q4 存量 include 缺口编不出（干净树同样失败，与本修复无关）
- **tc275_car 命令队列抗突发（v1.1.1）**：根治台架"大量 `LINK cmdq full` 后失控"——链路泵
  任何秒级停摆都会让 C6 把整段积压（q_cmd 32 + q_tx 32 + 在途段 ≈ 96 帧）一次性突发灌入，
  8 深 CPU0 命令队列瞬间打满且每条被拒命令打一行日志，而 CPU0 用阻塞 UART 排日志环
  （全环 ≈180 ms @115200）又拖慢同一循环里的出队，洪水自持。修复四件套：队列 8→16 深；
  `SET_SPEED`（30 Hz 摇杆/心跳流）走新 API `XCORE_cmdPushLatest` 新者胜入队（积压塌缩成
  1 条，急停/一次性命令仍严格 FIFO）；`cmdq full` 日志限速 1 行/s 携带抑制计数（计数器
  仍一比一，LINKDBG 新增第 19 字段 `cmdqRej`）；`XCORE_logService` 每调用只排空一行
  （控制周期不再被日志背压绑架）。新增主机单测 `test_xcore`（1095 断言，CI 同步加跑）；
  真源 22 号升 V1.8、31 号 V2.6、33 号 V1.4、00-index 同步
- **五个工程 clangd 全量可解析**（编辑器跳转/补全/诊断）：三个 ESP-IDF 工程沿用各自
  `build/compile_commands.json`（clangd 自动向上发现，无需配置）；TC275 两工程本机无
  SCons + TASKING 工具链，新增 `scripts/gen-tc275-cdb.py`（`just clangd-db` 生成）——
  与 site_scons 同源解析 `.cproject` 的 include/宏/源集，用宿主 clang + `__HIGHTEC__`
  路径出 CDB，缺失的 HighTec 专有头（`machine/cint.h` 等）由 `scripts/clangd-shim/tc275/`
  兜底，iLLD 内建包装头里的 TriCore 汇编诊断经各工程 `.clangd` 的 IgnoreHeader 屏蔽
- **仓库架构 monorepo 化**：四个固件工程（esp32c6_car / smartcar_remote / tc275_car / tc275_sbl）
  以 subtree 保历史并入本仓（各仓原提交号全部保留可追溯），退役 submodule 总控架构——
  一次提交一次推送、跨工程改动原子化，`git status` 不再有 "(new commits)" 指针噪音；
  CI 收编至根目录按 paths 触发（tc275_car 的 C6 交叉校验改用仓内源码）；
  版本 tag 加工程前缀（c6/ r-s3/ app/ sbl/，与 fw.py 别名一致）；
  justfile 退役 init/status/sync/push/fix-head/lock/pin 配方，新增 tag 配方；
  旧工程仓库在 GitHub 归档只读，历史 tag 仍可从旧仓库查阅
- fw.py `flash` / `ota` 默认先增量编译再烧录/打包（一条指令到位，不再烧/推旧构建产物）；`--no-build`（ota 另有 `--file`）跳过编译，flash/ota 内的编译不触发归档
- fw.py 收编第五个工程 **s3-gateway**（别名 `gw-s3`）：build/flash 委托其自带 `flash.py`，产物名按 CMake `project()` 名（`s3_gateway.bin`），版本号读 `CMakeLists.txt` 的 `PROJECT_VER`；OTA 与 esp32c6_car 共用冻结契约（上传 URI `/ota/c6` + bundle magic `C6FW`，台架签包种子各仓一份且字节相同）。justfile 与 `firmware/README.md` 同步（工程清单四→五、别名表加 `gw-s3`、新增「烧录认板」章节）
- 四仓版本真源统一升至 1.1.0，迎接 S3-CAM 替换 C6 的迁移（迁移前状态以整车基线 tag `v1.0.1` 锁定）；s3-gateway 工程纳入本仓版本管理（ESP32-S3 Freenove CAM，对齐 1.1.0）
- **Remote 视频面（scr_cam）+ CAMERA/VISION 页落地**（smartcar_remote，按 Remote 设计文档 §16 文件清单）：Camera WS 客户端（独立 esp_websocket_client 实例、监督者独占拨号、3 s 重拨节拍、Control 联动拆链）、`cam_frame.h` 20 B 头校验、esp_jpeg(TJpgDec) 解码、深度-1 pending + 3 帧槽最新帧环 + 一代延迟释放、10 Hz LVGL 泵；app_state 扩 cam/vision 字段组；host 自测新增 test_cam_frame / test_vision
- **S3-CAM 无 MicroSD**：snapshot/record/SD 容量整体移出 V1.0（Remote R-ADR-09 / 网关 LLDD 1.0 修订），contracts、双端代码与两份设计文档同步收窄
- **Camera WS 8.2 文本平面双端打通**：网关 `/ws/camera` 改双向会话（入站 `subscribe/pause/ping/profile`，出站 hello/pong/1 Hz cam_state，订阅门控 + `set_framesize` 运行态档位）；Remote URI 补 `:81` 端口（`CONFIG_SCR_CAM_STREAM_PORT`，对齐网关流实例，此前按 LLDD 表 12 拨 :80 永远连不上）、暂停期 4 s keepalive ping 配网关 5 s 入站超时、pong 采样本平面 RTT 进 DIAG
- contracts 双仓化：`camera/cam_frame.h`、`vision/vision.h` 副本落位 `s3-gateway/components/s3_proto/`（网关 TX 改用 `cam_frame_build()`，删本地重复定义），契约新增 `VISION_T_ERR`、`CAM_WS_OP_*`/`CAM_WS_PONG` 解析常量，`check-contracts.sh` 校验双端四份副本
- **s3-gateway 的 CI 与 Release workflow 补齐**（`.github/workflows/s3-gateway.yml` + `s3-gateway-release.yml`，此前 `just tag gw-s3` 打了 tag 什么都不触发）：CI 矩阵编台架与生产两种口味（新增 `s3-gateway/sdkconfig.prod` 叠加层关掉免配网 AP 与调试后门，并断言生产那份后门确实关了）、推流预算/核分工 sdkconfig 键断言、大小门禁与 host 单测；Release 走同一份生产配方，附 `PROJECT_VER == tag` 校验与 esp32s3 单文件合并镜像。README 的 CI 清单与 contracts 覆盖表同步（第五工程 + `camera/cam_frame.h`、`vision/vision.h` 双仓副本）

### 修复
- fw.py `flash` 透传参数（`-m`/`-p`/`--id` 等选项）被 argparse 误拒（改 REMAINDER 原样透传）
- **两块 ESP32-S3 烧错板子**：遥控器与 S3-CAM 网关的原生 USB-Serial-JTAG 描述符完全相同
  （303A:1001，连芯片型号都一样），旧的自动选口按"最低 COM 号取第一个 Espressif 口"猜，
  两块同插时可能把网关固件烧进遥控器。现在 fw.py 读 flash 里的 `esp_app_desc_t`
  （分区表 → app 分区 +0x20，magic `0xABCD5432`、`project_name`@+48）认工程，
  只有一块候选也照样认；`s3-gateway/flash.py` 在同 VID 组出现多候选时不再猜而是报错要求 `-p`
- **S3-CAM 视频流卡顿**：lwIP 每条连接的发送缓冲仍是默认 5760 B（4 MSS），一张 VGA JPEG
  要拆成"发 4 段等一轮 ACK"的接力，RF 一抖动就掉到传感器速率以下、多余帧被丢，画面表现为
  忽快忽停；`s3-gateway/sdkconfig.defaults` 把 `LWIP_TCP_SND_BUF_DEFAULT`/`TCP_WND_DEFAULT`
  抬到 14400。配套：控制页 `onerror` 原本只把画面标灰、从不重连（一次 503 就永久黑屏），
  现在指数退避重连并用 `/api/diag` 的 `camera.frames` 检测"连接在但画面停住"；
  `/api/diag` 的 `camera{}` 补 `stall`/`slow` 两项，卡顿变成可核对的计数
- **s3-gateway 的 `/ws/camera` 那版从未编译成功**：ESP-IDF 6.x 把 cJSON 移出 in-tree 组件
  （registry 包 `espressif/cjson`），`s3_camera` 的 `PRIV_REQUIRES cJSON` 让 cmake 配置阶段
  就报 `unknown name`；同版还用了旧帧尺寸枚举名 `QVGA`/`VGA`（现名 `FRAMESIZE_*`）。
  补依赖声明并改枚举后两种口味均通过、台架重烧复验。之所以拖到今天：该工程此前没有 CI，
  "编过了"全靠人肉记 `idf.py` 的历史输出——本轮补的 `s3-gateway.yml` 就是这条的守门人
- **Remote 视频面永远 NO SIGNAL**：`scr_cam` 的 WS 分片重组把 `ev->payload_len`（整帧总长）
  当本片字节数用，而 `esp_websocket_client` 的语义是 `data_len` 才是本事件携带字节数。
  客户端 `buffer_size=4096` < VGA JPEG 20–25 KB ⇒ 每帧拆 5–6 个事件，逐个越读堆约 19 KB、
  后片覆盖前片，EOI 校验必挂 → `CAM_RX_JPEG_ERR` 持续计数。改取 `data_len`（与 `scr_link`
  控制面二进制路径一致）后 `payload_offset + data_len` 才是真正的运行结束偏移，
  `ASM_CAP` 越界检查随之生效；设计文档 §4.1 同步纠正"client 会自动重组分片"的错误前提
- **网关 `/ws/camera` 会话从未建立**（上一条之后视频面仍然无帧的第二道墙）：ESP-IDF 6.x 的
  `esp_http_server` 回完 101 就不再调用 URI handler，会话占位写在 handler 的 `HTTP_GET` 分支里
  等于没写——hello 不发、入站 ops 无人消费、压在 socket 上的帧体被下一次头解析当 opcode 读
  （`WS frame is not properly masked` 反复拆链）。改为挂 `ws_pre_handshake_cb`/`ws_post_handshake_cb`
  （与控制面 `s3_http` 同款）、泵启动即发 hello、跨任务写改用 `httpd_ws_send_frame_async(handle, fd)`
  并统一持锁
- **网关采集档位数字漂移**：`S3_CAMERA_FRAME_SIZE` 用数字中转 `framesize_t`，而本 vendor 树
  插了 `FRAMESIZE_128X128`/`FRAMESIZE_320X320`（VGA 从 8 变 10），菜单选 VGA 实配 CIF 400x296，
  hello 的宽高只能报 `0x0`、profile 退回 `STREAM_PREVIEW`。choice 现直接映射 `FRAMESIZE_*` 常量；
  台架复验：网关 `sensor OV5640 up: frame_size=10 quality=12 640x480`、
  手持机 `camera hello: sensor=OV5640 640x480`
- **Camera WS 文本面互操作闭合（LLDD §8.2 两端对齐）**：网关侧入站 ops（`subscribe/pause/ping/profile`）
  与 hello 落地后，补齐"两个默认互相拧着"那一半——手持机 `cam_subscribe_now()` 改为订阅之后无条件
  补发 `{"op":"profile"}`（旧代码只有配成 WEB 才发，REMOTE 分支只打日志），网关在 `/ws/camera` 会话
  退役时把共用的 `framesize` 寄存器写回自己的 `CONFIG_S3_CAMERA_FRAME_SIZE`；`framesize` 是传感器一组
  全局档位、`/stream` 与 `/ws/camera` 共用，因此规则定为"谁订阅谁声明、退出即恢复"。台架证据
  （不开 CAMERA 页也验到）：网关 `cam: ws/camera op: ping (fd=40)` 每 4 s 一条，
  手持机 `scr_cam: camera pong: rtt=77..527ms`。另更正两处旧前提：网关 `recv_wait_timeout` 只是
  accept 时设的 `SO_RCVTIMEO`、空闲 WS 会话不会被它回收（4 s ping 的价值是 RTT 样本与双向证据，
  不是续命租约）；`/stream` 的 `s_clients` 闸与 `/ws/camera` 的 `s_ws.in_use` 闸互不排斥，
  手持机连接不会让手机页面吃 503，代价是两条泵共用 2 帧队列各拿约一半帧

## [1.0.0] - 2026-10-01

首个整车稳定版 tag。本版锁定：esp32c6_car@v1.0.0、smartcar_remote@v1.0.0、tc275_car@v1.0.0、tc275_sbl@v1.0.0（四仓版本号统一升至 1.0.0）。

### 新增
- About 页固件版本 tap 刷新全链路：S3 → C6（`{"t":"tcver"}` → SPI DIAG 0x53/0x24）→ TC275 即答版本信标；C6 侧走 /api/health
- 四仓版本真源（C6 PROJECT_VER / TC275 APP+SBL app_version.h）统一落位 1.0.0

### 修复
- C6 send_json 状态行补完整状态码（S3 /api/health 超时根因）
- S3 hello.tc 占位串覆盖 tc_on；WS 连续失败强制重连 Wi-Fi 自愈

## [0.1.0] - 2026-10-01

首个整车版本 tag。本仓版本即整车总 tag：一次 lock 组合出的四个子仓版本即为一次整车发布，
子仓明细见各自 CHANGELOG（esp32c6_car / smartcar_remote / tc275_car / tc275_sbl）。

本版锁定：esp32c6_car@v0.1.2+30、smartcar_remote@v0.1.0、tc275_car@v0.2.2、tc275_sbl@v0.1.0。

### 新增
- 多仓库总控：submodule 清单 + contracts 共享接口 + 批量脚本
- contracts：OTA 接口扩至 tc275_car，新增 crypto 三方验签契约与 ota_keys
- firmware/fw.py：四工程固件统一管理（编译/烧录/归档/出厂整包）；dist 归档入库（固件镜像 + manifest 版本管理）
- justfile：fw-* 固件配方 + scons-car/scons-sbl TASKING 直编配方
- 多仓库详细使用指南（概念/场景/命令/脚本/CI/FAQ）
- CI：contracts 门禁双平台（Linux+Windows）；doctor/repos 跨平台脚本，justfile 委托 repos.sh

### 变更
- 四仓命名统一 `<平台>_<功能>`：myCarSbl 仓库改名 tc275_sbl
- TC275 双仓命令行构建统一 SCons（`python -m SCons`，删除 build_sbl.sh）

[未发布]: https://github.com/lilicqyu-ship-it/smartcar/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/lilicqyu-ship-it/smartcar/releases/tag/v1.0.0
[0.1.0]: https://github.com/lilicqyu-ship-it/smartcar/releases/tag/v0.1.0
