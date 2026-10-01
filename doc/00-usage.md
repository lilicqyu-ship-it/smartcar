# 00 - smartcar monorepo 使用指南

> 面向日常开发者的完整操作手册：概念、场景、命令、脚本、CI、FAQ。
> 快速上手只看 §0 速查卡；第一次用建议通读 §3 和 §4；遇到报错直接查 §9。

---

## 0. 速查卡（TL;DR）

```bash
# —— 所有 just 命令都在 smartcar 根目录执行 ——
cd ~/Documents/Code/smartcar

just          # 列出所有命令
just doctor   # 检查本机环境是否就绪（新机器第一步）
git status    # 就一个仓库——git 本身就是全部状态，不再有 "(new commits)"
just contracts        # 校验共享接口副本一致性
just contracts-apply  # 用 contracts/ 覆盖各工程副本
just tag c6 v1.0.1    # 打工程版本 tag（前缀: c6/ r-s3/ app/ sbl/）
just fw-list            # 四工程固件产物 / 归档状态
just fw-build tc275_car  # 编译固件（esp32c6_car|smartcar_remote|tc275_car|tc275_sbl）
just fw-flash c6 all -m  # 烧录（串口自动识别；SBL+App 出厂整包: just fw-factory --flash）
```

三条铁律：

1. **平时开发 = 普通单仓开发**：直接进工程目录改代码、`git add/commit/push`，没有第二层仓库，没有指针要 bump。
2. **改共享接口必须走 `contracts/`**（§5.2/5.3）：改权威版本 → `contracts-apply` → 契约与所有副本放进**同一个提交**。
3. **禁止单方面改 `contracts/` 里的接口文件副本**——CI 逐字节校验会发现并拒绝。

---

## 1. 为什么是这个架构（背景）

车工程 2026-10 前是 4 个独立 GitHub 仓库 + 1 个 submodule 总控仓，实际暴露的问题：

| 痛点 | 后果 | monorepo 的解法 |
|---|---|---|
| `proto_frames.[ch]` 在 C6 和遥控器里是两份拷贝 | 改协议要手动同步两处，文档已经漂移过 | `contracts/link/` 唯一权威版本 + 逐字节校验 |
| OTA 的 SBL/APP 必须配套发布，但版本关系靠脑记 | 上板才发现槽位/包头解析错位 | `contracts/ota/` + 契约与实现在**同一个提交**里原子生效 |
| 跨仓改动要"两仓各一提交 + 回 meta bump 指针" | 漏一步就是 `git status` 里的 `(new commits)` 漂移，要靠 `just pin/lock` 收尾 | 一个提交一次推送，指针噪音这类问题**整类消失** |
| 标签/CI/分支保护在每个仓库各配一份 | 手动对齐，越拖越散 | 根目录一套 workflow 按 paths 触发；配置一份 |

monorepo（smartcar）承担三个职责：

1. **四个固件工程同仓**：`esp32c6_car/` 等就是普通目录，普通 git 管理。
2. **共享接口契约**：`contracts/` 是跨工程接口的唯一权威版本。
3. **固件工具链**：`firmware/fw.py` + `justfile` 统一编译/烧录/归档/OTA。

> 🗄️ 历史说明：四个工程以 subtree 方式**保历史**并入本仓——各仓原提交号原样保留
> （`git log` 可追到独立成仓时代），旧 tag 仍可从 GitHub 上已归档的旧仓库查阅。

---

## 2. 目录结构逐项说明

```
smartcar/
├── esp32c6_car/             # 工程①：ESP32-C6 小车主控固件（ESP-IDF）
├── smartcar_remote/         # 工程②：ESP32-S3 遥控器（LVGL + proto）
├── tc275_car/               # 工程③：TC275 车体控制固件（TriCore）
├── tc275_sbl/                # 工程④：TC275 OTA 二级引导（PFlash 双 bank + 回滚）
├── contracts/               # ★ 共享接口契约（唯一权威版本，逐字节校验）
│   ├── link/                #   LINK 通信协议：proto_frames.[ch]（帧格式/命令常量表/CRC16）
│   ├── ota/                 #   SBL↔App 接口：ota_layout.h（槽位/分区/DFlash 分配）
│   │                        #     ota_meta.h（DFlash 启动元数据页）、
│   │                        #     tcfw_bundle.h（TCFW 包头 148B + 签名范围 84B）、
│   │                        #     ota_keys.h（验签公钥）
│   └── crypto/              #   三方验签实现：ed25519v.[ch] / sha512.[ch] / c6_consts.h
│                            #     （C6 的 c6_ota、SBL、tc275 App 三方共用同一份）
├── scripts/
│   ├── check-contracts.sh   # 契约校验（--apply = 用契约覆盖副本）
│   ├── sync-gh.sh           # 旧多仓 GitHub 配置下发（旧仓库归档后可删）
│   └── doctor.sh            # 本机环境自检
├── firmware/                # 固件统一入口（§7.4）
│   ├── fw.py                # 四工程编译/烧录/归档一条命令（纯 Python 标准库）
│   └── dist/                # 归档产物：<工程>/<时间戳-g提交号>/ + manifest.json
├── .github/workflows/       # CI：contracts.yml（门禁）+ 四工程 CI/Release（按 paths 触发）
├── justfile                 # 命令入口（§6 逐条说明）
├── .gitattributes           # 全仓 LF；.bat/.cmd 强制 CRLF（跨平台校验一致的前提）
└── README.md                # 项目简介（本文是详细版）
```

**判断一个文件该不该进 `contracts/`**：只有满足"**两个及以上工程需要逐字节一致，且必须同步生效**"的接口文件才进；工程私有的实现、构建脚本（CMakeLists、Makefile）一律留在各工程目录。

---

## 3. 首次上手

### 3.1 安装工具

macOS：

```bash
brew install just gh bash    # bash 4+ 是 sync-gh.sh 的要求（系统自带 3.2 不够）
gh auth login                # 按提示走浏览器；或用 token: gh auth login --with-token
```

Windows：

1. 装 [Git for Windows](https://git-scm.com/download/win)（自带 Git Bash，**必须用它**，见 §9 WSL 陷阱）
2. 装 [just](https://github.com/casey/just/releases)（scoop: `scoop install just`）
3. `gh` 可选——只影响 `just gh-sync`

### 3.2 自检

```bash
just doctor
```

逐项 OK 才继续。常见FAIL：
- `bash < 4`（macOS）→ `brew install bash`，且确认 PATH 里 `/opt/homebrew/bin` 在前
- `core.longpaths`（Windows）→ `git config --global core.longpaths true`（FreeRTOS 仓库路径很深）
- `core.autocrlf=true` → `git config --global core.autocrlf false`（换行已由 .gitattributes 管，双重转换会打架）

### 3.3 克隆

```bash
git clone https://github.com/lilicqyu-ship-it/smartcar.git
cd smartcar
just doctor    # 最后确认一遍
```

没有第二层仓库：clone 下来即是全部，没有 `--recurse-submodules`、没有 `just init`。

---

## 4. monorepo 心智模型（替代原 submodule 速成）

- **工程即目录**：`esp32c6_car/` 等四个目录是普通目录，由本仓唯一的一个 git 管理。没有 gitlink、没有 detached HEAD、没有 `submodule update`。
- **版本组合 = 提交本身**：整车在任何时刻的代码组合就是某个提交的树快照，`git checkout <提交>` 一步精确复现；不再需要"bump 指针 + lock"来记录组合。
- **工程版本 = 带前缀的 tag**：`c6/v1.0.1`、`r-s3/v1.0.1`、`app/v1.0.1`、`sbl/v1.0.1`（与 fw.py 工程别名一致；四工程曾有同名 v1.0.0，前缀避免撞名）。整车总 tag 可直接打裸版本号 `vX.Y.Z`。
- **历史完整**：四工程的独立历史经 subtree 合并原样保留（提交号不变），`git log -- <工程>/`、`git blame` 可贯穿到独立成仓时代；旧 tag 在 GitHub 已归档的旧仓库里查阅。

---

## 5. 日常场景操作手册

### 5.1 场景 A：只改一个工程（90% 的情况）

和任何普通单仓项目一样：

```bash
cd ~/Documents/Code/smartcar/esp32c6_car
# ……改代码、idf.py build、单测、git add/commit/push，一切照旧
git pull --ff-only    # 多台机器/多人时的日常同步
```

跨工程状态？在根目录 `git status` 一个命令看全，不再有四仓状态一览脚本。

### 5.2 场景 B：改 LINK 协议（contracts/link/）

适用：`proto_frames.[ch]` 的帧格式、命令表、CRC 参数等。C6 和遥控器必须同时升级。

```bash
cd ~/Documents/Code/smartcar

# ① 改权威版本（contracts/link/ 下的文件）
$EDITOR contracts/link/proto_frames.h

# ② 一键同步到所有副本（esp32c6_car 和 smartcar_remote）
just contracts-apply

# ③ 两侧构建验证
( cd esp32c6_car && idf.py build )
( cd smartcar_remote && idf.py build )

# ④ 一个提交同时带上契约与两侧副本——原子闭环
git add -A
git commit -m "feat(proto): 新增 0x42 帧命令——contracts 同步 C6/S3 副本"
git push
```

推送后 CI 自动按路径触发契约校验与两工程 CI（§8）。**本地想提前验证**：`just contracts`。

### 5.3 场景 C：改 OTA 接口 / 验签（contracts/ota/、contracts/crypto/）

适用：槽位布局、镜像头、签名范围、公钥、ed25519/SHA-512 实现。SBL 和 App 必须一致，否则只能上板才发现。

流程与 §5.2 完全相同，只是副本方变成 `tc275_sbl/mw/ota|crypto/`、`tc275_car/mw/ota|crypto/`、`esp32c6_car/components/c6_ota/`：

1. 改 `contracts/ota/...` 或 `contracts/crypto/...`
2. `just contracts-apply`（同步进所有相关工程副本）
3. 各相关工程构建验证
4. **一个提交**（契约 + 全部副本）→ push

> ⚠️ OTA 接口变更意味着**旧 SBL 无法校验新 App 包**（或反之）。这类改动的提交说明里要写清兼容性影响，并优先走"新增字段/版本号前向兼容"而不是改老字段。

### 5.4 场景 D：发版

前提：要发的提交已推上 main。

```bash
just tag c6 v1.0.1        # 例：esp32c6_car 发 v1.0.1（tag 名 c6/v1.0.1）
git push origin c6/v1.0.1 # push tag 后对应 Release workflow 自动构建/发布（§8）
```

- 整车总 tag：`git tag -a v1.2.0 -m "..."` 打裸版本号即可——tag 所指提交的树就是整车代码，OTA 场景 SBL/App 配套关系天然固化。
- 任何一台机器 `git checkout <tag或提交>` 即精确复现当时整车全部代码。

### 5.5 场景 E：GitHub 配置（旧仓库遗留）

`just gh-sync`（§7.2）针对四个**旧工程仓库**（标签/分支保护/看板）。旧仓库归档（archive）后，本配方与 `scripts/sync-gh.sh` 可一并删除。新 issue 一律建在本仓库，标题用 `[c6]/[remote]/[tc275]/[sbl]` 前缀辅助阅读。

### 5.6 场景 F：编译 / 烧录 / 归档固件（firmware/）

四工程的构建烧录方式各不相同（ESP-IDF 环境、TASKING 命令行、AURIXFlasher），
`firmware/fw.py` 把它们收拢成一套命令，在 smartcar 根目录即可操作：

```bash
just fw-list                    # 各工程有没有产物、最近构建时间、最新归档
just fw-build tc275_car         # 编译一个工程（--collect 顺带归档）
just fw-build sbl --collect     # 别名: c6 / r-s3 / app / sbl
just fw-flash esp32c6_car       # 烧录（ESP 串口按 USB VID 自动识别，-p COMx 指定）
just fw-flash c6 all -m         # 透传子命令/参数（assets+固件+监视器）
just fw-collect                 # 四工程产物全部归档到 firmware/dist/
just fw-factory                 # SBL + App 槽 A 合成 factory_full.hex（--flash 一步烧录）
```

归档目录名带**提交号**（脏工作区加 `-dirty`），manifest.json 记录
产物清单与来源路径——烧到板子上的固件永远能对回源码版本。

前置条件（§7.4 有细节）：ESP 两工程需 EIM 装的 ESP-IDF v6.1（自动发现）；
TC275 两工程需完整版 TASKING（ADS 内置版禁止 IDE 外运行）+ 首次在 ADS 里
构建一次以生成构建文件；TC275 烧录需 DAS 服务在跑。

TC275 两工程另有一条 **SCons 直编路线**（`just scons-car` / `just scons-sbl`）：
SConstruct 直接解析 `.cproject` 取 include/宏/源码排除，编译链接参数复刻 ADS
生成命令行，**无需先在 ADS 里构建**；产物在 `<工程>/build/tasking-debug/`
（elf/hex/map），与 ADS 产物体积一致（tc275_sbl 逐字节相同）。余参透传：
`just scons-sbl size`、`just scons-car cfg=release`、`-c` 清理。

---

## 6. just 命令手册

| 命令 | 作用 | 备注 |
|---|---|---|
| `just` | 列出全部命令 | |
| `just doctor` | 本机环境自检 | 新机器第一步（§3.2） |
| `just contracts` | 契约校验（只读） | 与 CI 同款，推前自查 |
| `just contracts-apply` | 用 contracts/ 覆盖各工程副本 | 契约与副本放**同一个提交** |
| `just tag <proj> <ver>` | 打工程前缀 tag，如 `just tag c6 v1.0.1` | proj: c6/r-s3/app/sbl；push tag 触发 Release（§5.4） |
| `just gh-sync` | 旧仓库 GitHub 配置批量下发 | 遗留；旧仓归档后可删（§7.2） |
| `just fw-list` | 四工程固件产物 / 归档状态一览 | §5.6 |
| `just fw-build <工程>` | 编译固件 | 工程: esp32c6_car/smartcar_remote/tc275_car/tc275_sbl，别名 c6/r-s3/app/sbl（§5.6） |
| `just fw-flash <工程> [参数]` | 烧录固件 | 参数透传各工程入口；ESP 串口自动识别（§5.6） |
| `just fw-collect [工程]` | 归档产物到 firmware/dist/ | 目录名带提交号，含 manifest（§5.6） |
| `just fw-factory` | SBL+App 出厂整包合成 | `--flash` 顺带 AURIXFlasher 整包烧录（§5.6） |
| `just fw-ota <工程> [参数]` | 编译+OTA 签包推送 | 经 S3 中转或 `--direct` PC 直推（§5.6） |
| `just scons-car` / `just scons-sbl` | TASKING SCons 直编 TC275 工程 | 免 ADS 生成文件；余参透传 `size`/`cfg=release`/`-c`（§5.6） |
| `just fw-clean` | 清空 firmware/dist/ | |

> 已退役的多仓配方：`init` / `fix-head` / `status` / `sync` / `push` / `lock` / `pin` /
> `release`——monorepo 下由普通 `git` 命令直接覆盖（`git status` / `git pull --ff-only`
> / `git push` / 打 tag），不再需要批量脚本。

Windows 注意：justfile 已写死 `windows-shell := Git Bash`；Git 装在非默认路径时临时覆盖：
`just --shell "D:/Git/bin/bash.exe" --shell-arg -cu doctor`

---

## 7. 脚本手册

### 7.1 check-contracts.sh —— 契约校验

```bash
bash scripts/check-contracts.sh          # 校验模式：任何副本不一致/缺失 → 退出码 1
bash scripts/check-contracts.sh --apply  # 同步模式：用 contracts/ 覆盖所有副本
```

- 校验是**逐字节**（cmp）的，不考虑换行差异——所以全仓 LF 的 .gitattributes 是前提。
- 校验清单是脚本里一段**声明表**，新增覆盖项就是加一行：

```bash
CHECKS=(
  "link/proto_frames.h  esp32c6_car/components/c6_proto/proto_frames.h smartcar_remote/main/proto/proto_frames.h"
  ...
  # 新增行：contracts/内路径  各工程副本路径...
  "ota/ota_layout.h     tc275_sbl/mw/ota/ota_layout.h  tc275_car/mw/ota/ota_layout.h"
)
```

当前覆盖 27 项：link 2 文件 ×2 工程、ota 4 文件 ×2 工程（tc275_sbl+tc275_car）、crypto 5 文件 ×3 工程（esp32c6_car+tc275_sbl+tc275_car）。

### 7.2 sync-gh.sh —— 旧仓库 GitHub 配置批量下发（遗留）

```bash
bash scripts/sync-gh.sh [labels|protection|board|all]   # 默认 all，幂等可重复跑
```

- **labels**：统一 7 个标签到 4 仓库（`--force` 语义：不存在则建，存在则更新颜色/描述）

  | 标签 | 颜色 | 用途 |
  |---|---|---|
  | bug | d73a4a | Bug |
  | enhancement | a2eeef | 新功能 |
  | documentation | 0075ca | 文档 |
  | ci | 1d76db | CI/构建门禁 |
  | proto | 5319e7 | LINK 协议/共享接口 |
  | ota | fbca04 | OTA/升级 |
  | hardware | c5def5 | 硬件/结构/PCB |

- **protection**：main 分支 = 禁 force-push、禁删除，不强制 PR（单人开发）；等多人协作再在脚本里加 `required_pull_request_reviews`
- **board**：找/建 user 级 ProjectV2「Smartcar」，把 4 仓库全部 open issue 收进板（按 URL 去重）
- 权限不足的项报 `FAIL ...（需要 xx 权限?）` 并跳过，不影响其他项。fine-grained PAT 需勾选：各仓库 **Issues: write**（标签）、**Administration: write**（分支保护）、项目读写（看板）

> 旧仓库在 GitHub 归档后，本脚本与 `just gh-sync` 可一并删除。

### 7.3 doctor.sh —— 环境自检

工具版本（git/just/gh/python3）、bash≥4、gh 登录、core.autocrlf、（Windows）longpaths 与路径长度、四工程工作区换行是否 LF（发现 CRLF 会给出修复建议：`git add --renormalize .`）。

### 7.4 fw.py —— 固件统一管理（just fw-* 配方的实现层）

`python firmware/fw.py <命令>`，子命令 `list / build / flash / collect /
factory / clean`，详见 [firmware/README.md](../firmware/README.md)。只做发现、
委托与归档，不重复实现各工程的构建烧录：

| 工程 | 编译 | 烧录 |
|---|---|---|
| esp32c6_car | 委托 `esp32c6_car/flash.py build`（EIM 自动发现，含 assets） | 委托 `flash.py`（full/assets/all，-p 串口，-m 监视） |
| smartcar_remote | EIM 环境 + `idf.py build` | `idf.py -p <串口> flash`，串口自动识别 |
| tc275_car | `python -m SCons`（解析 `.cproject` 与 ADS 同源，产物名带版本） | `tc275_sbl/tools/flash.py flash <App槽A.hex>` |
| tc275_sbl | `python -m SCons`（同上） | `tools/flash.py flash`（自动取 SCons 最新版本化 hex） |

tc275 双工程的命令行编译统一为 SCons：源集/include/宏/排除表直接解析 `.cproject`，
编译链接参数复刻 IDE 生成的命令行，与 IDE 零漂移；产物在 `build/tasking-<cfg>/`
（文件名自动携带 `mw/app_version.h` 里的版本号）。余参透传：`fw.py build tc275_car
cfg=release opt=-O2 -c`。TASKING 工具链由 SCons 自动发现
（`TASKING_TRICORE_HOME` 可覆盖）。

---

## 8. CI 说明（.github/workflows/）

monorepo 下共 6 个 workflow，全部在根目录：

| workflow | 触发 | 内容 |
|---|---|---|
| `contracts.yml` | 每次 push/PR/手动（不加 paths——副本被单方面改坏也要拦） | `check-contracts.sh`；ubuntu + windows 双平台（Windows 路保证 Git Bash 可用、换行两平台一致） |
| `esp32c6-car.yml` | paths: `esp32c6_car/**`、`contracts/**` | host 单测（`make -C test/host check`）+ ESP-IDF v6.1-beta1 固件构建 + `tools/ci_size_report.py` 大小门禁（app ≤90% OTA 槽位、静态 DIRAM ≤95%） |
| `smartcar-remote.yml` | paths: `smartcar_remote/**`、`contracts/**` | ESP-IDF v6.1 容器构建（esp32s3）+ 大小门禁 + host proto 测试 + 固件 artifact |
| `tc275-car.yml` | paths: `tc275_car/**`、`esp32c6_car/components/c6_proto|c6_sf/**`、`contracts/**` | host 测试（gcc）：SF 帧编解码、38 字节遥测布局**直接编译仓内 C6 源码**交叉校验（C6 契约变更在这里炸 CI，不在台架上）、时序图 JSON 校验 |
| `tc275-car-release.yml` | tag `app/v*` | 先复用 tc275-car.yml 全部测试，再创建 GitHub Release（0.x 带 prerelease）；不附固件二进制（TASKING 无法在托管 runner 编译） |
| `esp32c6-car-release.yml` | tag `c6/v*` | 生产口味构建 → 单文件合并镜像 → GitHub Release（notes 取 tag 注释） |

- TC275 的 TASKING 固件编译不在 CI（专有编译器上不了托管 runner），host 测试兜底。
- 工程内 `git describe` 现面向全仓（含带前缀 tag），版本串可能带前缀——语义不受影响（§9 Q10）。
- 红了怎么修：见 §9 Q2。

---

## 9. 故障排查 FAQ

**Q1：`just` 提示找不到命令？**
必须在 smartcar 根目录跑（justfile 在那）；没装就 `brew install just`（Windows: scoop）。

**Q2：CI 契约校验红了 / `just contracts` 报 FAIL？**
输出里每个 FAIL 行都写了哪个副本不一致。两种修法：
- 副本是对的、契约旧了 → 把改动补进 `contracts/` 对应文件 → `just contracts-apply` 确认无 diff → 提交（契约与副本同一提交）；
- 契约是对的、有人手改了副本 → `just contracts-apply` 覆盖回权威版本 → 提交。

**Q3：`git push` 被拒 non-fast-forward？**
远端有新提交。先 `git pull --ff-only` 再推。**main 已禁 force-push**（分支保护），不要试图强推。

**Q4：sync-gh.sh 某项 FAIL（403）？**
token 权限不够（§7.2 末尾），补权限后重跑，脚本幂等。

**Q5：Windows 上 just 执行的是 WSL 的 bash，git 凭据对不上？**
justfile 已写死 Git Bash 路径；若 Git 装在非默认位置，见 §6 的 `--shell` 覆盖法。裸 `bash` 在 Windows 常解析到 `System32\bash.exe`（WSL），看不到 Windows 侧凭据。

**Q6：大量 CRLF 警告 / 校验因换行失败？**
`git add --renormalize .` 重新归一化后提交；确认 `core.autocrlf` 未设 true（§3.2）。

**Q7：想加第 5 个工程（比如 tc275_car-freecad）？**
普通目录直接 `git add` 进来即可，然后按需登记：README 布局、`firmware/fw.py`（如要进固件工具链）、`scripts/check-contracts.sh` 声明表（如有共享接口）、根目录 workflow（如要 CI）。没有 `git submodule add` 这种仪式了。

**Q8：误把私有接口文件塞进了 contracts/？**
从声明表和 contracts/ 删掉即可，历史无副作用——contracts/ 只是拷贝，删除不影响任何工程构建。

**Q9：四个工程的旧历史和旧 tag 去哪了？**
历史经 subtree 合并**原样保留**在本仓（提交号不变，`git log -- <工程>/` 可查到独立成仓时代的每个提交）。旧 tag（v0.1.0、v1.0.0 等）未搬到本仓——push 旧 tag 会触发按路径设计的 Release workflow，而历史提交的目录布局与 monorepo 不同。旧 tag 请到 GitHub 上已归档的旧仓库查阅；新发版一律用带前缀的 tag（§5.4）。

**Q10：工程里 `git describe` 显示的是别的工程的 tag（带前缀）？**
正常现象。describe 面向全仓 tag 集，最近的可达 tag 可能来自其他工程。版本串语义不受影响（OTA 元数据只关心字符串本身）；若某工程要求版本串严格来自自己的 tag，可在该工程构建里显式固定版本源（如 version.txt / PROJECT_VER），按需再做。

---

## 10. 约定规范

- **commit**：conventional 风格 + 中文描述，如 `feat(proto): ...`、`fix(c6_http): ...`、`chore: ...`（沿用各工程既有风格）
- **tag**：工程版本一律带前缀 `c6/`、`r-s3/`、`app/`、`sbl/`（`just tag <proj> <ver>`）；整车总 tag 用裸版本号 `vX.Y.Z`
- **分支**：统一 main 单分支 + 短期 feature 分支（推 PR 或直接合，分支保护只挡 force-push/删除）
- **issue**：建在本仓库，打标签（§7.2 标签表），标题建议 `[c6]/[remote]/[tc275]/[sbl]` 前缀
- **接口变更**：一律走 `contracts/` → `contracts-apply` → 契约与副本**同一个提交**，禁止跳过契约直接改副本（§9 Q2 会兜底）

---

## 11. 附录

### 11.1 当前契约清单

| contracts/ 文件 | 内容 | 各工程副本 |
|---|---|---|
| link/proto_frames.h/.c | LINK v2 帧编解码 + 命令常量表 + CRC16 | esp32c6_car `components/c6_proto/`；smartcar_remote `main/proto/` |
| ota/ota_layout.h | PFlash 槽位 A/B、DFlash 扇区分配 | tc275_sbl `mw/ota/`；tc275_car `mw/ota/` |
| ota/ota_meta.h | DFlash 双页启动元数据（24B LE） | 同上 |
| ota/tcfw_bundle.h | TCFW 包头 148B、签名范围 84B | 同上 |
| ota/ota_keys.h | ed25519 验签公钥 | 同上 |
| crypto/ed25519v.h/.c | ed25519 验签实现 | esp32c6_car `components/c6_ota/`；tc275_sbl `mw/crypto/`；tc275_car `mw/crypto/` |
| crypto/sha512.h/.c | SHA-512 实现 | 同上 |
| crypto/c6_consts.h | C6FW 常量（与 C6 侧对齐） | 同上 |

### 11.2 相关文档索引

- `esp32c6_car/doc/02-proto.md` —— LINK 协议设计
- `tc275_car/doc/20-design/24-ota-sbl-dualbank.md` —— OTA 方案设计
- `tc275_sbl/doc/24-ota-sbl-dualbank.md` —— SBL 实现
- `README.md` —— 项目简介与命令速览
