# 24 — TC275 OTA：SBL + PFlash 双 bank 升级设计方案

| 项 | 值 |
|---|---|
| 状态 | **已实现（软件层，App 侧已接线）** — SBL/双 bank linker/OTA 协议栈已落地，tc275_car 工程已切换槽 A 构建（冒烟链接 .start@0x80008020）并接入 OTA 接收/自检确认：host 单测 G-OTA-1/2 全绿，SBL 经 TASKING（v6.3r1 命令行）编译链接验证 ≤32 KB；**尚未上板**（G-OTA-3/4/5/6 的硬件半边仍待 ADS 工程构建 + 板上验证） |
| 需求追溯 | 需求 F05（双板 OTA：TC275 双 bank 切换 + 失败自动回滚，签名验签） |
| 依赖契约 | SF 帧 `mw/sf/sf_frame.h`（OTA_DATA/OTA_CTRL）、C6 `components/c6_ota/bundle.h`（包格式基线） |
| 硬件 | TC275 AURIX 三核 200 MHz，**2×2 MB PFlash（PF0/PF1 双 bank）**，128 KB DFlash0 + 64 KB DFlash1 |
| 作者/日期 | 设计评审草案，2026-09-30 |

> 实现索引（tc275_sbl 仓库，2026-09-30 落地）：
>
> | 文档章节 | 实现位置 |
> |---|---|
> | §3.1 双 bank linker | `Lcf_SBL.lsl`（本工程构建用）、`Lcf_AppA.lsl` / `Lcf_AppB.lsl`（App 工程切换用，冒烟链接已验证 `.start` 分别落在 0x80008020 / 0x80208020） |
> | §4 OtaMeta 双页 | `mw/ota/ota_meta.[ch]`（纯 C99，LE 线格式）+ `bsp/flash_ota.c`（DFlash 后端，DF0 扇区 13/14） |
> | §5.1 SBL 决策 | `mw/ota/ota_boot.[ch]`（纯逻辑，host 测）+ `sbl/sbl_boot.c`（跳转/安全态）+ `Cpu0_Main.c`（SBL 入口） |
> | §5.2 App 自检确认 | `OTABOOT_confirmSelftest()`（App 侧接入点，待 tc275_car 工程调用） |
> | §5.3 OTA 接收 | `mw/ota/ota_rx.[ch]`（状态机 + ACK/STATUS，host 测） |
> | §6 TCFW 包 | `mw/ota/tcfw_bundle.[ch]`（84B 验签 + SHA-512[:32]，与 C6 sign_bundle.py 逐字节一致，host 测） |
> | §7 PFlash 擦写 | `bsp/flash_ota.[ch]`（IfxFlash 封装：扇区表遍历擦除、32B 页 staging、读回校验） |
> | 加解密 | `mw/crypto/`（ed25519v/sha512/c6_consts 自 esp32c6_car 逐字拷贝）+ `mw/sf/sf_frame.[ch]`（自 tc275_car 拷贝） |
> | App 侧接入（tc275_car 工程） | `Lcf_Tasking_Tricore_Tc.lsl` 换为槽 A 布局（入口 0x80008020）+ `Lcf_AppB.lsl`；`com/ota_app.[ch]`（OtaRxOps 装配 + 槽位自识别 + §5.2 自检确认）；`com/link.c` OTA 帧分发到 `OTARX_frame`；`Cpu2_Main.c` init/tick；`mw/proto/protocol.h` 补 PROTO_CMD_OTA_*（0x60..0x65）；mw/ota、mw/crypto、bsp/flash_ota 与 SBL 工程同源拷贝 |
| host 测试 | `test/host/`（282 断言全绿：`make check`）+ `tools/gen_test_vectors.py` |
> | 构建脚本 | `tools/build_sbl.sh`（本机完整版 TASKING v6.3r1 命令行验证；正式产物仍应从 ADS 出） |
>
> 对原设计的**修正**（核对代码/手册后确认，详见各节内标注）：PF1 基址、TCFW 签名范围、§5.1 的 DFlash 磨写细化。

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

TC275 PFlash：PF0 = 2 MB @ `0xA000_0000`(non-cached) / `0x8000_0000`(cached)，PF1 = 2 MB @ `0xA020_0000` / `0x8020_0000`。（原文写 0x8030_0000 有误：本工程 `Lcf_Tasking_Tricore_Tc.lsl` 的 tc27D 衍生型定义将 pfls1 映射在 cached 0x80200000 / non-cached 0xA0200000，与 TC27x 存储映射一致，实现以该表为准。）

落地分区（`mw/ota/ota_layout.h` 为唯一地址真源）：

| 区域 | 范围（cached / non-cached） | 大小 | 扇区 |
|---|---|---|---|
| SBL | 0x8000_0000..0x8000_7FFF / 0xA000_0000..0xA000_7FFF | 32 KB | PF0 S0+S1 |
| Slot A | 0x8000_8000..0x801F_FFFF / 0xA000_8000..0xA01F_FFFF | 2040 KB | PF0 S2..S26 |
| Slot B | 0x8020_8000..0x803F_FFFF / 0xA020_8000..0xA03F_FFFF | 2040 KB | PF1 S2..S26 |

两槽镜像对称（bank 内同偏移、同大小），槽入口 = 槽基址 + 0x20（`.start` 段，镜像 BMHD 复位约定；SBL 读此地址跳入）。注意 PF0 的 BMHD1 物理地址 0x8002_0000 落在 Slot A 内——SBL 构建已用 `IFX_CFG_CPUCSTART_BMI01_NOT_NEEDED` 去掉 bmhd_1 段，AppA 的 lsl 不在那附近安排可加载段。

**整包烧录**：SBL 与 App 各自独立链接，出厂/调试器一次烧录用
`tc275_sbl/tools/merge_hex.py` 在 Intel-HEX 层合成（地址互不重叠，重叠即
拒绝；合并入口 = SBL 0x80000020；可选 `--bin` 出整片二进制，空隙 0xFF）。

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

放 DFlash0（有独立擦写、掉电安全），A/B 双页轮换写，防写入中途掉电损坏。

落地地址：**DF0 逻辑扇区 13（0xAF01_A000）= 页 0，扇区 14（0xAF01_C000）= 页 1**（TC275 实配 128 KB/16 扇区；扇区 15 已被 tc275_car 的 calib 记录占用，0..12 预留）。线格式为 24 字节**显式小端**（TriCore 大端，不得结构体直拷），magic 字节序 'T','C','O','M'；CRC-32 覆盖前 20 字节。提交语义：写**非当前页**（seq+1）→ 读回校验 → 生效；旧页不失效（低 seq 影子，天然双副本）。

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
  │     → PENDING_VERIFY: boot_attempts++; 提交; jump 到 active 槽 App 入口
  │     → VALID:          直接 jump，**不写元数据**（见下）
  └ 无有效槽 → 停在 SBL 安全态（点错误 LED，等 UART 重刷）
```

> 实现细化（`mw/ota/ota_boot.c`）：原文对 VALID 槽也要求"每次上电 attempts++ 并提交"，这会每次上电烧一次 DFlash 擦写却不改变任何后续决策（attempts 只对 PENDING_VERIFY 有意义）。实现改为**仅 PENDING_VERIFY 槔升计数并提交**，VALID 槽零写启动。掉电安全语义不变。

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

- **R2 已定论（读 esp32c6_car 代码而非注释）**：签名覆盖**前 84 字节**——`tools/sign_bundle.py` 的 `SIGNED_LEN = 84` 与 `bundle.c` 的 `c6_ed25519_verify(pub, &h[84], h, 84)` 一致；c6 `bundle.h` 头注释里的 "[0, 84+32) 即前 116 字节"是**过时注释**，勿信。载荷摘要是 **SHA-512 截断前 32 字节**（`hashlib.sha512(...).digest()[:32]`），字段名 sha256 是历史命名。TCFW 验签器（`mw/ota/tcfw_bundle.c`）与上述逐字节一致，host 测试用同一 seed 生成的向量对拍通过（G-OTA-1）。
- C6 的 `ed25519v` + `sha512` + `c6_consts` 已逐字拷贝至 `mw/crypto/`（无堆、纯 C99），TC275 与 C6 两侧共用同一套真源，避免漂移。

---

## 7. PFlash 擦写应用层（可写、但无法本地编译）

已实现：`bsp/flash_ota.[ch]`（SBL 构建内编译通过；上板时序待验）。基于 iLLD `IfxFlash`：

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

## 8. link.c / protocol.c 改动点（协议接收层，host 测 + tc275_car 已接入）

1. ✅ `link.c:link_dispatch`：新增 `SF_TYPE_OTA_DATA/OTA_CTRL` 分支（不再 `unhandledType++`），转入 `OTARX_frame()`；tc275_car 的 `com/ota_app.c` 注入全部回调（槽位自识别、flash_ota、元数据、`IfxCpu_triggerSwReset`、`LINK_send`）。全部 OTA 状态驻留 CPU2（链路核），无需跨核加锁；CPU2 看门狗按设计禁用，扇区擦除的长等待不会触发复位。
2. 新增 `mw/ota/ota_rx.[ch]`：§5.3 状态机 + bundle 验签 + 调 `flash_ota` + 组 `OTA_ACK/OTA_STATUS` 回帧。
3. `protocol.h`：补齐 `PROTO_CMD_OTA_*`(0x60–0x65) 常量（对齐 C6，见 F7 命令表统一）。
4. TX 路径：`OTA_ACK`(TYPE_OTA_CTRL,CID 0x32)、`OTA_STATUS`(0x33) 经 link TX 队列回发。

---

## 9. 验收门禁

| 门 | 内容 | 可自动化？ | 状态 |
|---|---|---|---|
| G-OTA-1 | bundle 验签器 host 单测：合法包通过、篡改任一字节验签失败、截断包报 TRUNCATED | ✅ host | ✅ `test_tcfw`（9 用例全绿） |
| G-OTA-2 | SF OTA 帧编解码 host 单测：BEGIN/CHUNK/ACK/STATUS 字节布局与 C6 逐字节一致 | ✅ host | ✅ `test_ota_frames` + `test_ota_rx`（12 场景） |
| G-OTA-3 | 双 bank linker 各自 build 出可执行 hex | ❌ TASKING |
| G-OTA-4 | 上板：写 Slot B → SWAP → SBL 跳 B → 自检 VALID | ❌ 上板 |
| G-OTA-5 | 上板：传输中断/验签失败/自检失败三种回滚路径 | ❌ 上板 |
| G-OTA-6 | 掉电测试：OtaMeta 双页提交在写入中掉电后仍可恢复 | ❌ 上板 |

---

## 10. 落地顺序建议

1. **P1 ✅（已交付，host 全绿）**：`mw/ota/ota_rx` 协议状态机 + TCFW 验签器 + SF OTA 帧回发 + host 单测（G-OTA-1/2，282 断言）。
2. **P2 ✅ 代码就绪（待上板）**：`bsp/flash_ota` IfxFlash 擦写（编译通过；擦写时序/跨 bank 取指需板上验证）。
3. **P3 ✅ 编译验证通过（待 ADS 构建 + 上板）**：SBL 工程（本仓库，`Cpu0_Main.c`→`sbl_boot`）+ 双 bank linker ×3 + CStart 配置（单核启动、去 BMHD1）。命令行 TASKING v6.3r1 验证：SBL 11.2 KB @ 32 KB 区域内，入口 0x80000020。**R1 的 SBL→App 跳转仍需小样上板验证。**
4. **P4（联调，待做）**：C6↔TC275 端到端 OTA + 回滚 + 掉电测试（G-OTA-4/5/6）。C6 侧还需 TC275 推手（读 STATUS/推 TCFW 分片）与 TCFW 打包工具（改 `sign_bundle.py` 的 magic/载荷即可）。

---

## 11. 风险与开放问题

- **R1 reset 向量归属**：代码已落地（SBL 持有 reset @0x80000020，App `.start` 落槽基址+0x20，冒烟链接验证两个槽的地址正确）。风险剩余半边：**App CStart 从"SBL 运行态"二次初始化（时钟 PLL 重配、watchdog 重使能）的上板行为**，小样验证后才算关闭。
- **R2 hash 算法对齐 ✅ 已关闭**：签名覆盖前 84 字节、载荷摘要 SHA-512[:32]，与 `sign_bundle.py` 逐字节一致（见 §6）。
- **R3 两份 App 镜像 vs PIC**：TriCore 非 PIC，双槽需两份 linker 输出（构建产物 ×2）。若想单镜像双槽，需 SBL 做地址重定位，复杂度更高，不推荐首版。
- **R4 擦写取指约束**：擦 Slot X 时不能从 Slot X 取指；建议擦写例程驻 PSPR。
- **R5 doc §8 规则**：本设计落地为代码时，行为变更需同提交更新 doc 21/22/31 与本文。
