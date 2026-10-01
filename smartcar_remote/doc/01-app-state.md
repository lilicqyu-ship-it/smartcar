# 01 组合根与 UI 状态中心（app_main / app_state）

| 项 | 内容 |
|---|---|
| 代码位置 | `main/app_main.c`、`main/app_state.c/.h`、`main/Kconfig.projbuild` |
| 上游需求 | spec §59（系统状态模型）、§83（Event History）、§95.8/95.9（UI 与通信/控制解耦）、§98-101（UI State / Single Source of Truth / 过期显示）、§106（页面回读最新状态） |
| 状态 | 🟩 **代码完成** — 见文末完成状态表 |

## 1. 职责

1. **组合根**：启动编排唯一接线点（星型依赖，对齐 esp32c6_car `app_main` 模式）。
2. **UI State**：全系统唯一状态中心；所有页面读同一份快照，禁止各页面私有
   副本（spec §100）。
3. **事件历史**：32 条环形日志，供 Event Log 页与串口镜像（spec §83）。
4. **告警状态**：全屏覆盖层的等级/标题/正文/确认位（spec §20-22）。

## 2. 启动时序（app_main，实现代码顺序）

```
app_main
 ├─ 1 nvs_flash_init()          失败时 erase + 重试（ESP_ERROR_CHECK）
 ├─ 2 app_state_init()          快照互斥 + 日志互斥 + 状态清零
 ├─ 3 scr_settings_init()       NVS 读取（缺省回退 Kconfig 台架值）
 ├─ 4 app_state_set_mode()      持久化模式进状态
 ├─ 5 bsp_display_start()       LCD(ST7262 RGB) + GT1151 触摸 + LVGL 任务
 │      （BSP 全托管，spec §77 不引入第二套 GUI 栈）
 ├─ 6 boot_mark_lcd/touch       P0 页前两格
 ├─ 7 bsp_display_lock(0) + ui_init() + unlock()
 │      UI 对象创建只在主任务完成一次，之后全部 LVGL 访问都在 LVGL 任务
 ├─ 8 scr_link_start()          Wi-Fi + WS + monitor 任务（非阻塞）
 ├─ 9 scr_ctrl_start()          30 Hz 控制任务
 └─ 10 boot_mark_sys + log      P0 页满 4 格 → UI 自动 fade 进主页（600 ms 驻留）
```

启动不阻塞在任何无线结果上：主页状态栏以 `SEARCHING/CONNECTING` 如实反映
链路进度（spec §81 快速可操作、§7 开机页不刷 DEBUG）。

## 3. 快照模型（scr_state_t）

一个结构体承载全部 UI 数据，字段分五组：

| 组 | 字段（摘） | 写入方 |
|---|---|---|
| link | conn / ctrl_role / tc_on / rssi / channel / lat(+min/max) / tx_rate / rx_rate / loss_pct_x10 / tele_seq / quality / c6_fw / link_rtt_ms / link_err_rate / pair_status | scr_link |
| vehicle | speed_mm_s / v_target·meas_l/r / batt_pct / batt_mv / odo / fault_code / veh_state / tc_fw_ver / hw_rev / tele_fresh | scr_link（遥测）+ snapshot 时判定过期 |
| control | owner / mode / joy_v·w / out_v·w / stop_latch / emerg_latch | scr_ctrl（UI 经 scr_ctrl 接口间接写 joy/stop） |
| boot | boot_lcd / boot_touch / boot_radio / boot_sys | app_main / scr_link |
| alert | alert{id, level, title, msg} / alert_ack | scr_ctrl / scr_link / app_alert_* |

**过期判定在快照时点进行**：`tele_fresh = 距上帧遥测 < CONFIG_SCR_TELE_TIMEOUT_MS`
（spec §101——过期显示 `--`，不展示旧值冒充实时）。

### 3.1 速度 EMA（对齐手机控制页 app.js）

```
v = (v_meas_l + v_meas_r) / 2        # 车体速度，mm/s
snap 条件: 首帧 / 距上帧 > 400 ms / |v| < 30 mm/s（停止附近不爬行）
否则:      disp += (v - disp) * (1 - exp(-dt / 150 ms))
```

### 3.2 系统状态推导（spec §59，纯函数 app_state_derive）

```
!boot_sys            → BOOT
emerg_latch          → EMERGENCY
fault_code ≠ 0       → FAULT
alert ∈ {WARN, CRIT} → WARNING
conn ≠ CONNECTED
  或 !tc_on          → CONNECTING
stop_latch           → STOPPED
out_v/w ≠ 0          → CONTROL（DRIVING）
其余                 → READY
```

优先级自上而下；READY 语义 = 连接 + 车辆链路在 + 油门 0（spec §60）。

## 4. 事件日志与告警

- `app_state_log(lvl, fmt, ...)`：环形 32 条 × 48 B，`ts_ms/level/text`；
  `app_state_events_get()` 以**最新在前**拷出；同时按级别镜像到 ESP_LOG
  （CRIT→E / WARN→W / 其余→I）。
- `app_alert_raise(id, lvl, title, fmt, ...)`：**按 id 分槽（5 槽）**，多个告警
  同时存在互不覆盖；快照时合成**最高严重级**展示（同级别取最新）。
  `app_alert_clear(id)` 精确移除自己的槽位——"低电先于失联出现、失联先恢复"
  的序列下，失联恢复不会误清低电（上一版单槽实现的真实缺陷，已修复）。
- `app_alert_ack()`：只 ACK 当前合成展示的那一条。
- 普通信息（RSSI 变差等）**不**进覆盖层，只由状态栏着色（spec §22）。

## 5. 接口（app_state.h 全量）

```c
void      app_state_init(void);
void      app_state_snapshot(scr_state_t *out);
scr_sys_t app_state_derive(const scr_state_t *s);

/* link setters */
app_state_set_conn / set_ctrl_role / set_tc / set_wifi / set_latency /
app_state_set_rates / set_loss / set_quality / set_c6_fw / set_owner /
app_state_set_telemetry / set_pair_status / note_rx

/* control setters */
app_state_set_joy / set_out / set_stop / set_emerg / set_mode

/* boot marks */
app_state_boot_mark_lcd / _touch / _radio / _sys

/* events + alerts */
app_state_log(lvl, fmt, ...) / app_state_events_get(out, cap)
app_alert_raise(id, lvl, title, fmt, ...) / app_alert_clear(id) / app_alert_ack()
```

## 6. 配置项（main/Kconfig.projbuild，本模块相关）

| 项 | 默认 | 说明 |
|---|---|---|
| `SCR_TELE_TIMEOUT_MS` | 600 | 遥测过期阈值（50 Hz 链路名义值） |
| `SCR_BATT_LOW_PCT` / `SCR_BATT_CRIT_PCT` | 20 / 10 | 低电告警阈值（spec §56） |

## 7. 资源

| 项 | 值 |
|---|---|
| 互斥 | 快照 1 + 日志 1（FreeRTOS mutex） |
| 快照成本 | 整结构体拷贝（≈500 B），10 Hz UI / 30 Hz ctrl 均为栈上拷贝 |
| 日志环 | 32 × ~56 B 静态分配 |

## 8. 完成状态表

| # | 功能 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| S-1 | 快照 + 过期判定 | ✅ | `app_state_snapshot`；spec §101/§106 |
| S-2 | 系统状态推导 | ✅ | `app_state_derive`；优先级见 §3.2 |
| S-3 | 速度 EMA | ✅ | 参数与手机页一致；待真机比对手感 |
| S-4 | 事件环形日志 | ✅ | 32 条，newest-first；串口镜像 |
| S-5 | 告警 raise/clear/ack（多槽位仲裁） | ✅ | 同 id 重入刷新文本；槽满丢弃新告警并保持旧告警 |
| S-6 | 启动编排 + P0 四格 | ✅ | `app_main`；无线不阻塞 |
| S-7 | 堆/PSRAM 水位守护 | 🟡 | 只在 Diagnostics 页展示；**无周期守护与告警**（对标 esp32c6_car 10 s 堆守护，列入二阶段） |
| S-8 | WEB MASTER 精确显示 | ⚪ | C6 hello 不告知对端角色，故显示 NO CONTROL（spec §103 允许两者取一） |
| S-9 | `note_rx` 50 Hz 空转移除 | ✅ | WS 新鲜度本就在 scr_link 内计时，删除多余互斥路径 |

## 9. 已知限制与风险

1. 快照是"整段拷贝 + 单互斥"，写侧高频（遥测 50 Hz）与读侧 30 Hz 在双核上
   竞争窗口极小，但未做压力测量；如真机出现锁竞争，按字段拆互斥。
2. `pair_status` 是单一槽位，Pairing 页与 Radio 设置页共用，最后一条反馈覆盖
   前一条（当前 UI 流程下无冲突）。
