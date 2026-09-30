# 24 — TC275 OTA：SBL + PFlash 双 bank 升级设计方案

| 项 | 值 |
|---|---|
| 状态 | **设计草案（DRAFT）** — 未实现，未经 TASKING 编译/上板验证 |
| 需求追溯 | 需求 F05（双板 OTA：TC275 双 bank 切换 + 失败自动回滚，签名验签） |
| 依赖契约 | SF 帧 `mw/sf/sf_frame.h`（OTA_DATA/OTA_CTRL）、C6 `components/c6_ota/bundle.h`（包格式基线） |
| 硬件 | TC275 AURIX 三核 200 MHz，**2×2 MB PFlash（PF0/PF1 双 bank）**，128 KB DFlash0 + 64 KB DFlash1 |
| 作者/日期 | 设计评审草案，2026-09-30 |

> ⚠️ 本文是**设计交付**，不含可运行的 SBL/linker 代码。SBL 与双 bank linker 必须在 AURIX Development Studio (TASKING) 环境中实现与验证；本方案给出结构、地址、状态机与验收门禁，供实现时逐项落地。

---

## 1. 目标与非目标

### 1.1 目标
- TC275 可通过 **C6→SPI(SF 帧)** 接收一个已签名的 TriCore 固件包，写入**非当前运行 bank**，校验通过后切换并重启进入新镜像。
- 升级失败（传输中断 / 验签失败 / 新镜像自检失败）**自动回滚**到旧 bank，车辆不变砖。
- 与 C6 的 OTA 状态机对齐：TC275 对 `OTA_BEGIN/CHUNK/ABORT` 回 `OTA_ACK` 与 `OTA_STATUS`，最后 `OTA_SWAP` 触发重启。

### 1.2 非目标（本期不做）
- Secure Boot（HSM 验证 SBL 自身）—— 归后续信息安全专题。
- C6 与 TC275 的**联合升级编排**（谁先谁后、失败联动）—— 归 F05 系统级方案，本文只做 TC275 单侧。
- OTA over UART 回退通道 —— SPI 为主链路，UART 仅调试。

---

## 2. 现状事实（实现前必须知道，均已核对代码）

| 事实 | 证据 |
|---|---|
| 当前只有**单一** linker 布局，App 定位在 PFlash0 cached 区 | `Lcf_Tasking_Tricore_Tc.lsl`：`RESET 0x80000020`、`INTVEC0 0x801F4000`、`TRAPVEC 0x8000_01xx`，全部落在 `0x8000_0000` 段（PF0 cached） |
| **无 SBL**：上电直接进 App 的 `Cpu0_Main` | `Cpu0_Main.c` 是 App 入口，无二级引导；三核经 `IfxCpu_syncEvent` 同步 |
| **无 flash 写入应用层**：只有 iLLD 底层 `IfxFlash` 驱动 | `Libraries/iLLD/TC27D/Tricore/Flash/Std/IfxFlash.h` 存在；`com/mw/rt/bsp/app` 无任何擦写封装 |
| **OTA 帧当前被丢弃** | `com/link.c:link_dispatch`：`SF_TYPE_OTA_*` 落 `unhandledType++` |
| C6 的 bundle 是 **C6 自己的包**（"C6FW"，含 c6.bin+assets.bin），**不能**直接喂给 TC275 | `components/c6_ota/bundle.h` 头布局 |

**结论**：完整 OTA 需要从零补三层——(A) 双 bank linker + SBL 引导层，(B) PFlash 擦写应用层，(C) SF-OTA 协议接收 + bundle 验签。(A) 是一次性引导/硬件工程，必须在 TASKING 里做；(B)(C) 代码可写且 (C) 可 host 单测。

---

## 3. PFlash 分区与地址映射（双 bank）

TC275 PFlash：PF0 = 2 MB @ `0xA000_0000`(non-cached) / `0x8000_0000`(cached)，PF1 = 2 MB @ `0xA030_0000` / `0x8030_0000`（具体基址以所用衍生型的存储映射为准，实现时对照 TC27x User Manual "Memory Maps" 章节核实）。

推荐分区（每 bank 内自洽，两 bank 镜像对称）：

```
┌──────────────────────────────────────── PFlash 总 4 MB ────────────────────────────────────────┐
│ SBL (二级引导)         Slot A (App)              Slot B (App)             共享/元数据             │
│ 32 KB, 固定不参与OTA   ~1.9 MB, PF0 主体          ~1.9 MB, PF1 主体         DFlash: OTA 状态双页     │
│ 上电最先执行           出厂镜像                    OTA 目标槽                                        │
└──────────────────────────────────────────────────────────────────────────────────────────────┘
```

- **SBL 固定驻留**在 PF0 最低 32 KB（含 reset 向量），**永不被 OTA 覆盖**。这是回滚的信任根。
- **Slot A / Slot B** 各含一份完整可执行 App（含各自的 INTVEC/TRAPVEC，位置无关或按槽重定位）。
- 关键设计约束：**两个槽的 App 必须能在各自基址独立运行**。TriCore 代码默认非 PIC，因此需要**两份 linker 输出**（Slot A 版、Slot B 版），或让 SBL 做地址重映射。推荐前者（两份 `.lsl`，构建两个 hex），简单可靠，代价是构建产物 ×2。

### 3.1 需要新增的 linker 文件（TASKING，需你实现验证）
- `Lcf_SBL.lsl` — SBL 定位在 `0x8000_0000..0x8000_7FFF`，含 reset。
- `Lcf_AppA.lsl` — App 定位在 Slot A（reset 由 SBL 跳入，App 自身不占物理 reset 向量，改用 SBL 约定的入口表）。
- `Lcf_AppB.lsl` — 同上，定位 Slot B。

> ⚠️ reset 向量只有一个物理地址（`0x80000020`）。方案：**reset 向量归 SBL**，SBL 读 OTA 元数据后 `jump` 到选定槽的 App 入口。App 的 `Cpu0_Main` 不再是上电第一段代码。这是本方案对现有启动流程的**唯一破坏性改动**，实现时 `Cpu0/1/2_Main.c` 的 startup 汇编/`crt0` 需配合调整。

---

## 4. OTA 元数据（DFlash 双页提交，掉电安全）

放 DFlash0（有独立擦写、掉电安全），A/B 双页轮换写，防写入中途掉电损坏：

```c
typedef struct {
    uint32 magic;          /* 'T''C''O''M' = 0x4D4F4354 */
    uint32 seq;            /* 单调递增，选 seq 大且 crc 正确的页为准 */
    uint8  active_slot;    /* 0=A, 1=B —— SBL 据此跳转 */
    uint8  pending_slot;   /* OTA 写入中的目标槽，成功后 = active */
    uint8  slot_a_state;   /* 见 §5 槽状态枚举 */
    uint8  slot_b_state;
    uint8  boot_attempts;  /* SBL 每次尝试 +1；超阈值判定新镜像自检失败→回滚 */
    uint8  rsv[3];
    uint32 crc32;          /* 覆盖前面所有字节 */
} OtaMeta;                 /* 双页各一份，写新页→校验→标旧页失效 */
```

槽状态枚举（对齐 esp_ota 的 `PENDING_VERIFY`/`VALID`/`INVALID` 语义）：
```
EMPTY / WRITING / DOWNLOADED / PENDING_VERIFY / VALID / INVALID
```

---

## 5. 状态机

### 5.1 SBL 引导决策（上电）
```
上电 → 读 OtaMeta（选 seq 大且 crc 正确的页）
  ├ pending_slot 有效且 state==DOWNLOADED
  │     → active_slot = pending_slot; state = PENDING_VERIFY; boot_attempts=0; 提交元数据
  ├ active 槽 state==PENDING_VERIFY 且 boot_attempts >= 阈值(如3)
  │     → 判定新镜像自检失败 → 回滚: active_slot 切回另一槽(需其为 VALID); 目标槽标 INVALID
  ├ active 槽 state==VALID 或 PENDING_VERIFY(未超阈值)
  │     → boot_attempts++; 提交; jump 到 active 槽 App 入口
  └ 无有效槽 → 停在 SBL 安全态（点错误 LED，等 UART 重刷）
```

### 5.2 App 侧自检确认（进入新镜像后）
新镜像启动后跑自检（三核同步 OK、SPI LINK 起来、看门狗正常）：
- 自检过 → 写元数据 `active 槽 state=VALID, boot_attempts=0`（等价 `esp_ota_mark_app_valid`）。
- 自检不过 / 卡死不写 → SBL 下次上电 `boot_attempts` 累加，最终回滚。

### 5.3 OTA 接收状态机（App 运行期，CPU2 link 侧 + CPU0 编排）
```
IDLE
 └ 收 OTA_BEGIN{total_len, crc32}      → 选非 active 槽为 target; 校验 total_len 合理;
                                          擦除 target 槽; state=WRITING; 回 OTA_STATUS{RUNNING,0}
RECEIVING
 ├ 收 OTA_CHUNK{idx, data<=240}        → 顺序校验 idx; 累计 CRC/SHA; 写 PFlash target;
 │                                        回 OTA_ACK{idx, OK/FAIL}; 周期回 OTA_STATUS{RUNNING,pct}
 ├ 收 OTA_ABORT                        → 丢弃 target; state=EMPTY; 回 OTA_STATUS{FAILED}
 └ 全部收完                            → 校验 total CRC32 + bundle 头 ed25519 签名 + 镜像 SHA
                                          ├ 通过 → state=DOWNLOADED; 写 OtaMeta pending_slot=target;
                                          │        回 OTA_STATUS{DONE,100}
                                          └ 失败 → 丢弃; 回 OTA_STATUS{FAILED}
DONE
 └ 收 OTA_SWAP                         → 提交元数据; 软复位(IfxScuRcu) → SBL 接管 §5.1
```

---

## 6. TC275 固件包格式（"TCFW"，平行于 C6 的 "C6FW"）

**不要复用 C6 的 bundle**。定义 TC275 专用包，头结构照抄 C6 布局以复用验签逻辑，载荷换成 TriCore 镜像：

```
 0   magic "TCFW"
 4   u8  fmt_ver = 1
 5   u8  flags        bit0 = 目标槽无关(两槽通用镜像时置位；否则区分 A/B 版)
 6   u16 hdr_len = 148
 8   u32 total_len
 12  u32 app_len          /* TriCore App 镜像字节数 */
 16  32B app_sha256
 48  u32 rsv_len          /* 预留(=0，对齐 C6 的 assets 槽位) */
 52  32B rsv_sha256       /* 全 0 */
 84  64B ed25519 签名，覆盖 [0, 116) */
 148 app.bin ...
```

- 验签范围与 C6 完全一致（前 116 字节），可**移植 C6 的 `ed25519v` + `sha512` + bundle 解析器**到 TC275（纯 C99，无 IDF 依赖，`bundle.h` 已声明这点）。
- SHA 用 SHA-512 截断或 SHA-256？C6 用的是 `sha512.h` 但字段名 `sha256`——实现时**必须核对 C6 实际算法**，两侧一致才验得过。（待办：确认 C6 `bundle.c` 里 hash 实际是 SHA-256 还是 SHA-512/256，本方案按"与 C6 逐字节一致"约束。）

---

## 7. PFlash 擦写应用层（可写、但无法本地编译）

基于 iLLD `IfxFlash` 封装一个 `bsp/flash_ota.[ch]`：

```c
boolean FLASHOTA_eraseSlot(uint8 slot);                 /* 擦除整槽(按 PFlash sector 循环) */
boolean FLASHOTA_writePage(uint32 slotOff, const uint8 *data, uint32 len); /* 32B burst 对齐写 */
boolean FLASHOTA_verifyCrc(uint8 slot, uint32 len, uint32 expectCrc32);
```

TriCore PFlash 写入硬约束（实现时严格遵守，否则 ECC/时序错）：
- **写以 page 为单位**（TC27x PFlash page = 32 字节），必须 32B 对齐、整页写。
- 擦除以 **sector** 为单位。
- 擦/写期间**该 flash bank 不可取指**——所以**擦写代码必须运行在另一个 bank 或 PSPR(程序 SRAM)**。这是关键设计点：OTA 写 Slot B 时，运行代码在 Slot A(不同 bank)，天然满足；但擦写函数本身建议放 PSPR 以防边界情况。
- 写后必须 `IfxFlash_waitUnbusy` + 校验。

> 我可以把 `flash_ota.c` 和 bundle 验签器写出来，并给 bundle 解析器配 host 单测（`test/host/`），但 `IfxFlash` 时序、"从另一 bank 取指"这两点**只能上板验证**。

---

## 8. link.c / protocol.c 改动点（协议接收层，可 host 测）

1. `link.c:link_dispatch`：新增 `SF_TYPE_OTA_DATA/OTA_CTRL` 分支，不再 `unhandledType++`；解析 CID `OTA_BEGIN/CHUNK/ABORT/SWAP`，转发到 CPU0 的 OTA 编排。
2. 新增 `mw/ota/ota_rx.[ch]`：§5.3 状态机 + bundle 验签 + 调 `flash_ota` + 组 `OTA_ACK/OTA_STATUS` 回帧。
3. `protocol.h`：补齐 `PROTO_CMD_OTA_*`(0x60–0x65) 常量（对齐 C6，见 F7 命令表统一）。
4. TX 路径：`OTA_ACK`(TYPE_OTA_CTRL,CID 0x32)、`OTA_STATUS`(0x33) 经 link TX 队列回发。

---

## 9. 验收门禁

| 门 | 内容 | 可自动化？ |
|---|---|---|
| G-OTA-1 | bundle 验签器 host 单测：合法包通过、篡改任一字节验签失败、截断包报 TRUNCATED | ✅ host |
| G-OTA-2 | SF OTA 帧编解码 host 单测：BEGIN/CHUNK/ACK/STATUS 字节布局与 C6 逐字节一致 | ✅ host |
| G-OTA-3 | 双 bank linker 各自 build 出可执行 hex | ❌ TASKING |
| G-OTA-4 | 上板：写 Slot B → SWAP → SBL 跳 B → 自检 VALID | ❌ 上板 |
| G-OTA-5 | 上板：传输中断/验签失败/自检失败三种回滚路径 | ❌ 上板 |
| G-OTA-6 | 掉电测试：OtaMeta 双页提交在写入中掉电后仍可恢复 | ❌ 上板 |

---

## 10. 落地顺序建议

1. **P1（我可交付，纯软件+host 测）**：`mw/ota/ota_rx` 协议状态机 + bundle 验签器 + SF OTA 帧回发 + host 单测（G-OTA-1/2）。此时 C6 推 OTA，TC275 能握手、能验签、能明确回 FAILED（而非黑洞丢弃），只是最后 `flash_ota` 返回"未装配"。
2. **P2（我写代码，你上板验）**：`bsp/flash_ota` 基于 IfxFlash 的擦写实现。
3. **P3（你在 TASKING 主导）**：SBL 工程 + 双 bank linker + startup 改造（§3.1/§3.2）。
4. **P4（联调）**：C6↔TC275 端到端 OTA + 回滚 + 掉电测试（G-OTA-4/5/6）。

---

## 11. 风险与开放问题

- **R1 reset 向量归属**：改由 SBL 持有 reset、App 经 SBL 跳入，是对现有启动流程的破坏性改动，需改 startup（§3.2）。风险高，务必先在 TASKING 小样验证 SBL→App 跳转。
- **R2 hash 算法对齐**：C6 `bundle.c` 用 `sha512.h` 但字段名 `sha256`，需确认真实算法，两侧不一致则永远验签失败。→ 实现 P1 前先核对 C6 源码。
- **R3 两份 App 镜像 vs PIC**：TriCore 非 PIC，双槽需两份 linker 输出（构建产物 ×2）。若想单镜像双槽，需 SBL 做地址重定位，复杂度更高，不推荐首版。
- **R4 擦写取指约束**：擦 Slot X 时不能从 Slot X 取指；建议擦写例程驻 PSPR。
- **R5 doc §8 规则**：本设计落地为代码时，行为变更需同提交更新 doc 21/22/31 与本文。
