# 07 验证与测试汇总

| 项 | 内容 |
|---|---|
| 代码位置 | `test/host/`（G1 主机自测）+ 本文档集 |
| 上游需求 | spec §111（AI 验收清单）、§112（完成标准：可理解/可操作/防误触/异常可懂/持续流畅）；方法学对齐 [esp32c6_car doc/13-verification](../../esp32c6_car/doc/13-verification.md) 的 G1-G4 门 |
| 状态 | 🟡 **G1/G2/G3 绿；G4 首轮真机完成（§5.0），HIL 联调项进行中** |

## 1. 门定义

| 门 | 内容 | 状态 |
|---|---|---|
| G1 | 主机侧逻辑自测（无需目标机） | ✅ |
| G2 | 目标编译（ESP-IDF v6.1 / esp32s3） | ✅ |
| G3 | 文档级走查（spec 逐条对照） | ✅ |
| G4 | 真机功能 + HIL 注入 | 🔴 未开始 |

## 2. G1：主机自测（已完成，`cd test/host && make`）

| 用例 | 结果 |
|---|---|
| CRC16-CCITT-FALSE check 值 `0x29B1`（与 esp32c6_car 主机单测同源断言） | ✅ |
| CRC NULL 防护 | ✅ |
| DRIVE 帧构建 → 逐字节解析回环（v=600/w=300/seq 保序/极性/帧长 12） | ✅ |
| 遥测 38 B 编解码回环（u32/u16/负值 i16/u8/u32 全字段） | ✅ |
| 垃圾字节流后再同步到有效帧 | ✅ |

proto 源文件与 esp32c6_car **逐字节同源**，其 10⁷ 模糊测试结论直接继承。

## 3. G2：编译级验证（已完成）

| 项 | 值 |
|---|---|
| 工具链 | ESP-IDF v6.1（EIM，macOS arm64） |
| 目标 | esp32s3，16 MB flash / octal PSRAM |
| 产物 | `smartcar_remote.bin` ≈ 1.51 MB（factory 6 MB，余 75%） |
| 告警 | 本工程代码 0 error / 0 warning（组件 `esp_lcd_gc9503` 的 use-after-free 告警以 `-Wno-error` 降级，原因记录于根 `CMakeLists.txt`；该组件在本硬件 SUB3 上不参与运行） |
| 关键配置核对 | `BSP_LCD_SUB_BOARD_800_480=y`、`BSP_LCD_RGB_BOUNCE_BUFFER_MODE=y`、`SPIRAM_MODE_OCT=y`、`LV_FONT_MONTSERRAT_14/16/20/28/48=y` |
| 任务布局 | LVGL 核 1 / 栈 9216；`scr_ctrl` 核 1 / prio 5；`scr_link` 核 0（网络侧）——见 [06](06-ui.md) §1 |

## 4. G3：spec §111 验收清单走查（结论见 [00-overview](00-overview.md) 系统级视图）

重点核对项与结论：

1. **UI 与通信/控制解耦**（§95.8/95.9）：三任务结构 + 快照通道，静态走查通过。
2. **页面共享同一状态源**（§100）：全工程仅 `app_state` 一处存储，无页面级副本。
3. **数据过期显示**（§101）：`tele_fresh` 统一判定，页面只画快照。
4. **失联后状态**（§102）：告警/事件/停止链路完整，行为待 HIL 确认。
5. **控制权切换无误触**（§104-105）：角色由 C6 hello 驱动，闸门在 ctrl 侧，
   UI 不直接改锁存位；摇杆在无控制权/断链时门控置灰并强制回中（06 §4）。
6. **不伪造数据**（§95.7/§57）：控制器电池/温度无字段即不显示；
   WEB MASTER 无信源即显示 NO CONTROL；SUB3 无背光控制即标注 fixed。

### 4.1 交互稳定性加固走查（2026-09-29 第二轮，全部已入码）

| 问题 | 后果（若不修） | 修复 |
|---|---|---|
| 告警单槽覆盖 | 低电告警被失联覆盖后 `clear` 失效，低电永不清除 | app_state 按 id 分槽 + 最高级合成展示（01 §4） |
| 拖动摇杆中失链 | 手指停在盘上、链路恢复瞬间旧倾斜量复活为行驶指令 | 摇杆 10 Hz 门控 + 门关闭强制回中置零（06 §4） |
| 设置页 10 Hz 重写 textarea | 用户输入逐字符被清空 | 子页打开时一次性填值（06 §1） |
| 软键盘 DEFOCUSED/FOCUSED 竞态 | 两输入框间切换键盘消失 | 50 ms 延时隐藏（06 §1） |
| 慢速倒车显示 `+0.05` | 速度符号错误（截断取整） | 符号取自全值（06 §3） |
| RELEASED 无 PRESSED 配对 | 按压中被覆盖层打断 → 误触发急停 | PRESSED 时间戳清零防护（05 C-2） |
| 无控制权时发 STOP/急停帧 | C6 拒绝 → `err{auth}` 刷屏 | 发送函数内化角色闸门（05 §2） |
| LVGL 任务栈 BSP 默认 7168 | 复杂页回调栈溢出风险 | 提升到 9216 + 绑核 1；ctrl 绑核 1 保 30 Hz 节拍 |

## 5. G4：真机 / HIL 清单（🟡 首轮真机已完成，联调项进行中）

### 5.0 首轮真机验证（2026-09-29，S3-LCD-EV-Board-2 + 桌面 C6 网关）✅ 已执行

烧录 `idf.py flash` + 串口日志驱动排错，四轮迭代；发现并修复 4 个只有真机
才能暴露的问题：

| # | 现象（串口日志） | 根因 | 修复 |
|---|---|---|---|
| R-1 | 每 ~2.2 s `LoadProhibited` panic 循环，栈回溯落在 `lv_obj_add_style` | LVGL 内置分配器 64 KB 静态池被 10 个页面数百个样式对象耗尽，`lv_realloc` 返回 NULL（`LV_USE_STDLIB_MALLOC` 是**无 prompt 的派生 int**，defaults 改它无效） | `CONFIG_LV_USE_CLIB_MALLOC=y`（Malloc functions source → CLIB，走系统堆 + PSRAM 兜底）；sdkconfig 必须删除重生成 |
| R-2 | 开机即弹 `RADIO LOST` 全屏告警 | 从未连上过也满足"!link_ok" | ctrl 增加 `link_was_ok` 锁存：只有**已建立过的链路**丢失才告警；未连接状态由状态栏 SEARCHING/CONNECTING 表达 |
| R-3 | `sta is connecting, return error` 每 4 s 刷屏 | monitor 定时重连与进行中的连接尝试打架 | 改事件驱动重连（DISCONNECTED → connect），并打印断开 reason（201=NO_AP_FOUND 等，台架诊断价值高） |
| R-4 | WiFi 关联 + 拿到 IP 后**永久静默**：WS 永不启动 | `s_link.wifi_up` 从未置 true，monitor 永远卡在 `!wifi_up` 分支 | GOT_IP 事件置 `wifi_up=true`。**该 bug 编译期不可见、主机测试不可见，只有真机日志暴露** |
| R-5 | 屏幕抖动（用户实测） | ① WS 重连期间 30 Hz 发送带 50 ms 锁等待阻塞核 1，饿死 LVGL 供帧；② bounce 高度 10 供帧裕量小；③ 10 Hz 对 ~15 个 label 无条件 setText 全量重绘 | ① `esp_websocket_client_is_connected()` 门控 + 发送超时 0（忙则丢帧，33 ms 后重发）；② bounce 高度 10→20（中断率减半）；③ `ui_label_set_text/fmt` 缓存写入，值不变不触碰对象；④ `CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y` 收发分离锁 |
| R-6 | 仍抖 + 字体重叠 → 换防撕裂后"闪得更夸张" | ① 无防撕裂时 LVGL 局部重绘与 RGB 扫描相撞（撕裂=抖动/重影）；② 顶栏/信息卡多个绝对定位 label 落进 `LV_SIZE_CONTENT` 容器（内容高度只算单个子对象）→ 互相叠印 | ① 双帧缓冲防撕裂，**DIRECT_MODE**（FULL_REFRESH 会把每次小失效放大成整屏重绘，直接闪屏——实测否定）；② 显式容器高度 + 行对齐（信息卡两行、顶栏两行） |
| R-7 | 抖动源鉴别 | 无法从日志判断是软件负载还是硬件供电 | `CONFIG_SCR_BENCH_DISP_ONLY=y` 鉴别构建（无 WiFi/控制）：**纯显示稳定** → 锁定软件负载（WiFi/PSRAM 竞争），排除供电/面板时序 |
| R-8 | "闪烁非常严重"（车辆链路接通后） | 日志实锤：TC275 上线、遥测流启动后，**600 ms 遥测瞬时缺口（车辆启动/广播节奏）反复触发全屏 RADIO LOST 覆盖层弹出/消失——90 s 内 31 次全屏红/黑交替**；且覆盖层每 10 Hz 无条件 `move_foreground`（DIRECT 模式 = 整屏失效 ×10/s） | ① 告警去抖 `CONFIG_SCR_ALERT_DEBOUNCE_MS=1200`：持续丢失 1.2 s 才弹全屏，恢复即清；状态栏 STALE/"--"仍 600 ms 即时反应（spec 101/102 分层兑现）；② 覆盖层显隐/z-order 只在状态迁移时执行；③ 顺带修掉开机→主页 FADE 过渡（整屏双层混叠=一次全屏闪）与 ui.c 的 toast 反逻辑判断 |
| R-9 | 摇杆拖动时闪烁 | 摇杆旋钮是全 UI 唯一带 30 px **模糊阴影**的控件：每次移动在 ~200×200 px 区域逐像素 alpha 混合（PSRAM 帧缓冲上最重的渲染路径，触摸上报率高时持续满载）；且 PRESSING 每次输入轮询都触发位置更新（坐标未变也重绘） | ① 阴影改为 3 px 描边（视觉近似、渲染成本骤降）；② 旋钮视觉更新加 2 px 滞回——**控制指令仍全速率**（驾驶平滑度不受影响），只有像素位移 ≥2 px 才重绘 |
| R-10 | 摇杆拖动卡顿 + 屏幕割裂，**开机初始化画面也割裂**（用户实测，R-9 后复现） | **RGB bounce-buffer 供数带宽不足**，两处配置与官方 `esp-dev-kits` `lvgl_demos` 不一致：① `sdkconfig.defaults` 写的是不存在的 `CONFIG_ESP_DEFAULT_CPU_FREQ_240`，被静默忽略，CPU 实跑 **160 MHz**（启动日志 `cpu freq: 160000000`）；② 未开 `SPIRAM_XIP_FROM_PSRAM`，flash 写（NVS / Wi-Fi PHY 校准，开机必发生）期间 cache 关闭，bounce ISR 读不到 PSRAM 帧缓冲 → 面板错位。另 D-cache 32 KB/32 B、`-Og` 调试优化进一步压低 memcpy 吞吐 | 对齐官方：`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240`、`CONFIG_SPIRAM_XIP_FROM_PSRAM`、D-cache 64 KB / 64 B line、`CONFIG_COMPILER_OPTIMIZATION_PERF`、`BSP_LCD_RGB_BUFFER_NUMS=3`；保留 DIRECT_MODE 防撕裂（R-6）。**已真机验证**：日志 `cpu freq: 240000000 Hz`、无 panic/underrun，用户确认割裂消失。**反例（勿再用）**：`CONFIG_LCD_RGB_RESTART_IN_VSYNC=y` 在 bounce 模式下每 VBlank 复位 GDMA → 雪花屏，已撤销 |

**修复后 3 分钟 soak（真机 + 真实 C6）**：0 panic；WiFi→WS→hello 全链路
`WS connected` / `Control owner: S3`（C6 台架 bench 模式）/ `Vehicle link down`
（TC275 未接，判定正确）；130 s 处一次服务端侧 TCP 掉线 → **3 s 自动重连 +
角色/链路状态自动重新同步**（spec §82 恢复语义自证）；锁超时由 2.3/s 降至
0.6/s（偶发丢 pong，无功能影响）。

### 5.1 单板 bring-up（✅ 已完成，见 §5.0；遗留触摸手感实测）

1. ~~烧录 + P0 四格全绿自动进主页~~ ✅
2. 触摸：主页/设置页逐页点检（GT1158 IC 已识别，触摸行为需人工点检）
3. 显示：bounce-buffer 模式无花屏/漂移 ✅（bounce 高度 20；抖动由 R-5 修复，待用户视觉复核）
4. ~~LVGL 任务栈水位~~ 栈已提升 9216（BSP cfg），真机 UI 全页面创建稳定

### 5.2 与 esp32c6_car 联调（C6 台架：`SD-DEV000`/`sddev123456`）

1. STA 入网 + WS 建立 + `hello` 角色判定（无 token = NO CONTROL；
   `CONFIG_C6_BENCH_CTRL=y` 时全角色 CTRL，注意口径）。
2. 遥测 50 Hz 到达率、RSSI/RTT/丢包数字合理性。
3. 配对全流程：车侧开窗 → 200/token → 重连即 CTRL；403/409/504 文案核对。
4. 驾驶：摇杆 → DRIVE 0x50 @30 Hz（TC275 侧抓帧核对 v/ω 极性与满量程）；
   模式切换限幅比例；STOP 单击/长按/RELEASE 链路。
5. 失联注入：关 C6 → RADIO LOST 覆盖层 + 事件 + TC275 心跳停车；恢复 C6 →
   自动清除 + "Radio recovered" 事件。
6. 控制权注入：另一台手机带 token 接管 → S3 显示 NO CONTROL、发送停止；
   释放后 S3 重新申请。

### 5.3 实车标定（spec §26/§33，出产品标准前必做）

- RSSI 分档阈值（10/50/100/200/500 m 距离测试，S3 即链路测试仪表的雏形）。
- ECO/NORMAL/SPORT 限幅与死区默认值（驾驶手感）。
- 遥测超时 600 ms / 看门狗 10 s 的现场复核。

## 6. 缺口汇总（跨文档索引）

| 缺口 | 文档 | 阶段 |
|---|---|---|
| 全部 G4 真机项 | 本文档 §5 | 下一里程碑 |
| Quick Panel / 滑动切页 / 校准页 / Radio Test | [06](06-ui.md) U-9/U-10、P7/P10 | spec §92 Phase 4-5 |
| 正式视觉资产（SVG 图标/开机页） | [06](06-ui.md) §9.3 | spec §92 Phase 6 |
| 堆水位周期守护 | [01](01-app-state.md) S-7 | 二阶段 |
