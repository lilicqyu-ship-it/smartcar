# 05 控制与安全（scr_ctrl）

| 项 | 内容 |
|---|---|
| 代码位置 | `main/scr_ctrl.c/.h` |
| 上游需求 | spec §18-19（STOP/急停）、§22（告警不打扰）、§33-34（模式）、§59（状态模型）、§62（FAULT 展示）、§102（失联即停 + 明示）、§104-105（控制权/恢复语义） |
| 状态 | 🟩 **代码完成** — STOP/急停语义待 HIL 注入验证 |

## 1. 职责

UI 是 View，本模块是 Controller（spec §73/74）：30 Hz 消费摇杆输入 → 模式限幅
→ DRIVE 帧；持有 STOP/急停锁存；监视链路与车辆健康并触发告警。页面切换、
设置变更不影响本任务的节拍。

## 2. 30 Hz 控制循环

```
vTaskDelayUntil(1000 / CONFIG_SCR_CTRL_RATE_HZ)
 ├─ snapshot → safety_watch(§4)
 ├─ gate = conn==CONNECTED && ctrl_role && !stop_latch && !emerg_latch
 ├─ gate && (joy≠0)：
 │     v = joy_v * mode_pct / 100     # mode: ECO 50 / NORMAL 80 / SPORT 100
 │     w = joy_w * mode_pct / 100     # Kconfig 可调；实车标定前为台架初值
 │     send DRIVE(v, w)
 ├─ (conn && ctrl_role) 其余情况：send DRIVE(0,0)
 │     # 锁存停止 / 摇杆零位都持续喂心跳，TC275 心跳看门狗不误停
 └─ app_state_set_out(v, w)           # UI 显示"实际发出的"油门/转向
```

**发送闸门与手机页一致**：无 CTRL 角色时完全不发（C6 会以 `err{auth}` 拒绝，
重发只制造错误刷屏）。门控内化在 `send_drive/send_emergency_stop` 里——
STOP/急停的立即帧同样过闸，状态机任何路径都发不出未授权的命令。

## 3. STOP / 急停语义（spec §19/§63/§105）

| 动作 | 行为 | 解除 |
|---|---|---|
| STOP 单击 | 立即发 DRIVE(0,0)（不等下个 tick）+ `stop_latch=true` | 触摸摇杆（`joystick_touch`），清锁存 |
| STOP 长按 ≥1.2 s | `emerg_latch=true` + 发 `0x32 EMERGENCY_STOP` + DRIVE(0,0) + 全屏覆盖层 | 覆盖层 [RELEASE]（`emergency_release`）——**明确动作** |
| RELEASE 后 | emerg 清除；**stop_latch 保持** → 车辆保持停止，重新触摸摇杆才恢复 | —（spec §105：不自动恢复运动） |

双层停车语义：`0x32` 是 legacy 透传（语义在 TC275，尽力而为），
**DRIVE(0,0) 才是保证停车**；两者同发。

## 4. 安全监视（safety_watch，随 30 Hz 循环）

| 条件 | 动作 | 解除 |
|---|---|---|
| `!(conn==CONNECTED && tele_fresh)` **持续 ≥1.2 s**（`SCR_ALERT_DEBOUNCE_MS`） | 覆盖层 `RADIO LOST / VEHICLE STOP`（CRITICAL）+ 事件 CRIT | 链路恢复且遥测新鲜 → 清覆盖层 + 事件 INFO "Radio recovered"（spec §82/102） |

去抖只作用于**全屏覆盖层**：遥测过期的状态栏反应（STALE/"--"）仍是 600 ms
即时生效（spec §101/§102 分层兑现）。车辆端启动 / C6 广播节奏造成的
数百毫秒遥测缺口不再把整屏告警变成频闪（真机 R-8：90 s 内 31 次 → 0 次）。
| `fault_code ≠ 0` | 覆盖层 `VEHICLE FAULT 0x####`（WARNING）+ 事件 | fault 归零 |
| `batt ≤ CRIT` | 覆盖层 `CRITICAL BATTERY`（CRITICAL） | 电量回升至 LOW+5 |
| `LOW < batt ≤ 20 %` | 覆盖层 `LOW BATTERY`（WARNING） | 同上（回差防抖动） |

RSSI 变差**只**影响状态栏着色，不弹覆盖层（spec §22：普通信息不全屏）。

端到端停车兜底：链路断开后 DRIVE 心跳自然停发 → TC275 心跳看门狗自行停车
（c6_car 侧语义），与 S3 侧的 UI/告警互相独立——**S3 死机时车也会停**。

## 5. 接口（全量，UI 唯一控制入口）

```c
void scr_ctrl_start(void);
void scr_ctrl_stop_button(void);        /* STOP 单击          */
void scr_ctrl_emergency(void);          /* STOP 长按           */
void scr_ctrl_emergency_release(void);  /* 覆盖层 RELEASE      */
void scr_ctrl_joystick_touch(void);     /* 摇杆按下=重新接管   */
void scr_ctrl_control_lost(void);       /* 预留：控制权切换钩子 */
```

UI 不直接写 `stop_latch/emerg_latch` 状态位——全部经本模块接口，保证"每次
锁存变化都有对应的一帧零速输出"。

## 6. 资源

| 项 | 值 |
|---|---|
| 任务 | `scr_ctrl` 4096 B / prio 5 / **绑核 1**（Wi-Fi/lwip 独占核 0；与 LVGL 同核但优先级更高，33 ms 突发为微秒级，30 Hz 节拍不受 UI 负载影响） |
| 节拍 | 33.3 ms（vTaskDelayUntil，无漂移） |
| 帧构造 | 栈上 12 B frame，无堆 |

## 7. 完成状态表

| # | 功能 | 状态 | 证据 / 缺口 |
|---|---|---|---|
| C-1 | 30 Hz DRIVE + 模式限幅 | ✅ | 限幅比待实车标定（spec §33） |
| C-2 | STOP 单击/长按分流 | ✅ | 计时在 UI 侧（Home 页按下时长）；**RELEASED 无 PRESSED 配对时按单击处理**（PRESSED 时间戳清零防护），杜绝误触发急停 |
| C-3 | 急停 0x32 + 锁存 + RELEASE | 🟩 | 语义实现完整；**HIL 注入验证未做** |
| C-4 | 失联告警/恢复 + 事件 | ✅ | 与 spec §82/102 一一对应 |
| C-5 | 故障/低电告警（回差） | ✅ | 阈值 Kconfig |
| C-6 | 心跳维持策略 | ✅ | 闸门内零位也发 0,0 |
| C-7 | SEQ 单调来源 | ✅ | `scr_link_next_seq` 唯一计数器 |

## 8. 已知限制与风险

1. 模式切换只改 S3 侧限幅，不通知车辆（无 CFG 帧语义约定）——ECO 的"平缓
   加速"由限幅近似，真正的加速度快慢属 TC275 标定范畴。
2. `scr_ctrl_control_lost()` 目前是预留钩子（未被调用）；手机抢控的真实表现
   是 `err{auth}` → ctrl_role=false → 闸门关闭 → 停止发送，等效生效。
