# 51 · 编码器健康监测 P0 缺陷修复记录（单侧失效被合并 alive 掩蔽）

| 项 | 内容 |
|---|---|
| 文档编号 | 51 |
| 域 | 生产/质量（5x） |
| 版本 | V1.0（2026-10-04 首版，随 tc275_car v1.3.1） |
| 缺陷编号 | BUG-ENC-1（P0） |
| 状态 | **代码已修复并过 TASKING 构建 + 主机单测；单侧拔线注入等台架项未做** |
| 上级索引 | [00-index.md](00-index.md) |
| 关联真源 | [37 · 传感器融合](30-tc275/37-sensor-fusion.md) V1.3、[21 §5.1/§5.2](20-design/21-software-design.md) V1.16、[31](30-tc275/31-firmware-architecture.md) V2.9、[33](30-tc275/33-ai-codebase-guide.md) V1.7、[38](30-tc275/38-developer-logging.md) V1.1 |

> **一句话**：4 个轮编码器的健康被合并成一个 `alive` 布尔，而融合保护把"1 ms 发布序号在变"当成"编码器有效"——序号由发布器无条件自增，编码器全死它也照变；单侧失效时对侧一动 `alive` 恒真，死侧以假 0 测量参与闭环与融合。修复 = 发布**按侧边沿龄**原语，融合与伺服按"被驱侧必须持续出边沿（500 ms 起步宽限）"判健康。

---

## 1. 缺陷记录

| 项 | 内容 |
|---|---|
| 发现方式 | 代码审查（针对"单侧传感器失效可能未被及时发现"的专项核查），非现场故障 |
| 影响等级 | P0：前进保护的 `FUSION_ENCODER_LOST` 硬停止对**传感器本身失效**完全失效；单侧失效引发死侧 PI 积分 windup 与融合量污染 |
| 修复版本 | tc275_car v1.3.1 |

### 1.1 根因一：健康被合并成单一 `alive`（rt/encoder.c）

```c
dl = d[ENCODER_SIDE_LEFT_0] + d[ENCODER_SIDE_LEFT_1];   /* 每侧两轮求和 */
dr = d[ENCODER_SIDE_RIGHT_0] + d[ENCODER_SIDE_RIGHT_1];
if ((dl != 0) || (dr != 0)) lastMoveMs = now;           /* 任一侧动过即续命 */
g_alive = (uint32)(now - lastMoveMs) < ENC_ALIVE_WINDOW_MS;   /* 500 ms */
```

- **单侧失效检不出**：左侧两通道全坏、右侧在转 → `dl==0, dr!=0` → `alive` 恒 TRUE。
- **它其实是运动检测器**：静止超过 500 ms 时健康编码器也报 FALSE（状态回显退回指令回显、闭环退开环），语义与"健康"不符。
- 发布快照只有一个 `enc.alive`，全系统（`motor_algo` measOk、Cpu0 状态回显、Cpu2 `[WHEELS]` 日志）消费的都是这个合并位；不存在任何按侧/按通道健康判断。

### 1.2 根因二：融合把"发布序号变化"当成"编码器有效"（app/fusion.c + mw/xcore）

```c
/* xcore.c XCORE_encoderPublish：每次发布无条件自增，与 alive 无关 */
g_encoderStatus.seq = ++g_encoderSeq;
/* fusion.c motion()：freshEnc 只看 50 ms 内 seq 是否变过 */
freshEnc = s->encSeen && (now - s->encMs) <= 50u;
if (freshEnc) s->out.flags |= FUSION_ENCODER_OK;
```

- 发布链路恒定运行：CPU1 1 kHz 超循环每毫秒调 `ENCODER_task()` → `ENCODER_publish()`——**4 个编码器全死时也照样带新 seq 发布**（速度 0、alive FALSE）。
- `FusionInput` 结构上**没有编码器 alive 字段**，CPU0 只喂 `encoderSeq`。因此 `FUSION_ENCODER_OK` 的真实含义是"CPU1 的发布任务活着"，不是"编码器活着"。
- 后果链：前进分支的 `!(flags & FUSION_ENCODER_OK)` 判定（`FUSION_ENCODER_LOST` 硬停止）对传感器失效是**死代码**（只拦得住发布任务死/CPU1 挂/未标定）；速度锚 `meas=(L+R)/2` 在编码器死后恒 0 且被当有效值上报；超速判定基于 `wheelMmS`（死后恒 0）失效；`wheelYaw=(R-L)*DEG/track` 被死侧假 0 污染 → 误报 SLIP 或错误 yaw 混合。剩余保护只有 ToF 距离包络（独立生效，不至于失控，但 37 号承诺的"编码器失效阻止前进"失效）。
- **漏测原因**：`test_fusion.c` 只覆盖 `encoderSeq=0`（发布者从未发布）触发 ENCODER_LOST 的路径；"发布者活着、seq 每毫秒变、传感器死了"没有任何用例。

## 2. 修复设计

### 2.1 原语层（rt/encoder + mw/xcore）

- `XcoreEncoder` 新增 **`edgeAgeMs[2]`**：每侧"距该侧最近一次边沿的毫秒数"，饱和 0xFFFF（0xFFFF 同时表示"自启动无边沿"）。由既有 ISR 时间戳 `g_lastEdgeMs[]` 在发布时合成（本核 32 位对齐读，无撕裂风险）。
- 新阈值 `ENCODER_EDGE_FRESH_MS = 100`：侧速 1 mm/s ≈ 0.014 计数/ms ≈ 每 71 ms 一边沿（双侧求和），100 ms 覆盖到 ~0.7 mm/s；更慢与静止不可分，也不需要可分。
- `alive` 位**保留**，语义改为明示的"近期有运动"指示（Cpu0 状态回显行为不变）；`seq` 保留为发布/传输活性。新增 `ENCODER_getEdgeAgeMs()` 访问器。

### 2.2 融合层（app/fusion + Cpu0_Main）

- `FusionInput` 新增 `encEdgeAgeMs[2]`（Cpu0 每 10 ms 从快照填入），`Fusion` 新增 `encAbsentMs[2]` 计时器。健康判据改为**受驱侧必须持续出边沿**：

```
每侧 i：
  request[i]==0                    → absent=0（不要求边沿，静止永不误报）
  seq 新鲜 && age[i]≤100 ms        → absent=0（边沿在来）
  否则                             → absent += 真实步距 ms
FUSION_ENCODER_OK = seq 新鲜 && absent[0]<500 && absent[1]<500
```

- **500 ms 宽限（`FUSION_ENC_GRACE_MS`）是起步正确性的关键**：不能直接拿旧 `alive` AND 进来——静止超 500 ms 后 `alive=FALSE`，前进指令即被锁存，轮子永远不转，`alive` 永远不恢复，形成"从静止永远无法起步"的死锁。宽限窗口覆盖斜坡（0→1000 需 500 ms）+ 电机起动滞后；松杆即清零。
- **速度锚只用边沿新鲜的侧**：单侧死时车速取健康侧，不再把假 0 平均进去（修复前单侧死时车速恒减半）。
- `wheelYaw`/SLIP 一致性检查改用 `encBoth`（两侧边沿同时新鲜）作前提，死侧不再参与差速角速度。
- 前进分支的 `FUSION_ENCODER_LOST` 硬停止逻辑**一字未动**——它消费的标志现在真正反映传感器健康。

### 2.3 伺服层（rt/motor_algo）

```c
measOkL = g_closedLoopOk && (enc.edgeAgeMs[0] <= ENCODER_EDGE_FRESH_MS || g_left.cmd == 0);
measOkR = g_closedLoopOk && (enc.edgeAgeMs[1] <= ENCODER_EDGE_FRESH_MS || g_right.cmd == 0);
```

- 由全局 `enc.alive` 改为**按侧**：被驱侧边沿不新鲜即该侧回退开环（duty=目标、清积分，`SERVO_update` 既有兜底），修复"对侧运动掩蔽死侧 → 死侧 PI 对假 0 测量积分 windup"。`cmd==0` 豁免保持"未驱动侧无可测量"的正确语义；从静止起步先开环、边沿到来自动回闭环，与旧全局行为一致。

### 2.4 诊断（Cpu2_Main）

`[WHEELS]` 10 s 日志行新增 `left_edge_age_ms` / `right_edge_age_ms` 两字段（[38](30-tc275/38-developer-logging.md) V1.1 同批登记），台架可直接判读按侧健康。

## 3. 已知边界（诚实清单）

1. **同侧双轮之一失效不可分**：速度按侧求和、单轮无冗余判定；逐轮计数交叉比对（同侧两轮速比恒定）是后续增强项，本批不做。
2. **静止中损坏要等下一次驱动尝试后 ≤500 ms 才暴露**——"受驱侧才要求边沿"的推论，方向安全。
3. **持杆顶障/堵转 >500 ms 会以 ENCODER_LOST 锁存**（而非 OBSTACLE）：轮子不转时两者从编码器侧不可分；同为锁存停车、松杆恢复，UX 等价，但停车原因报告可能变化。
4. **全失效 + 零指令滑行**（直行、g≈1g、陀螺静）仍可能被静止判据接受 → 零偏学习污染；影响有界（稳态 k=0.002），未处理。
5. **encoder.c/motor_algo 硬件半截无主机单测**（33 §7 盲区）：按侧拔线注入、起步时序、[WHEELS] age 判读需台架。

## 4. 验证记录

| 项 | 结果 |
|---|---|
| test_fusion（gcc -Wall -Wextra -Werror） | ✅ 通过，新增 3 组回归：①双侧失效 490 ms 内不锁存、500 ms 锁存 ENCODER_LOST；②单侧失效车速锚=健康侧（修复前 150）、50 步内锁存；③静止陈旧边沿不锁存、起步 400 ms 不锁存、边沿恢复后持续行驶 |
| test_xcore | ✅ 1144 checks 0 failures（新增 edgeAgeMs 跨核透传 2 断言） |
| test_diag_txn / test_fusion_bridge / test_sf / test_adc / test_imu / test_calib_store | ✅ 1004 / 通过 / 2948 / 13 / 92 / 通过 |
| test_log_policy | ✅ 通过（顺带为并行的 UART 非阻塞泵改动补 harness `UART_printTry` mock——该测试此前在含该改动的树上链接失败） |
| TASKING SCons（TriCore Debug） | ✅ `tc275_car_v1.3.1.elf`，ROM 198243 B / RAM 91916 B，0 错误 |
| 台架（待做） | ①单侧拔线注入：期待 ≤500 ms `stop_encoder_unavailable` 锁存、松杆后保持、对侧转动不掩蔽；②从静止起步无假锁存；③`[WHEELS]` 两 age 字段随动判读 |

## 5. 同批变更清单

| 文件 | 变更 |
|---|---|
| `rt/encoder.h` / `rt/encoder.c` | `ENCODER_EDGE_FRESH_MS`、`ENCODER_getEdgeAgeMs()`、按侧边沿龄合成、`alive` 语义注释 |
| `mw/xcore/xcore.h` | `XcoreEncoder.edgeAgeMs[2]` + 块注释重写 |
| `app/fusion.h` / `app/fusion.c` | `FUSION_ENC_EDGE_FRESH_MS`/`FUSION_ENC_GRACE_MS`、`FusionInput.encEdgeAgeMs`、`Fusion.encAbsentMs`、motion() 健康/锚点/encBoth 重写 |
| `Cpu0_Main.c` | 融合输入喂 `encEdgeAgeMs[2]` |
| `rt/motor_algo.c` | measOk 按侧判定 |
| `Cpu2_Main.c` | `[WHEELS]` 行新增两 age 字段 |
| `mw/app_version.h` | 1.3.0 → **1.3.1** |
| `test/host/test_fusion.c` / `test_xcore.c` / `test_log_policy.py` | 新用例 + 透传断言 + UART 泵 mock |
| 文档 | 37→V1.3、21→V1.16、31→V2.9、33→V1.7、38→V1.1、00→V1.22（登记本文）、CHANGELOG |
