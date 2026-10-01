# 00 - smartcar 多仓库使用指南

> 面向日常开发者的完整操作手册：概念、场景、命令、脚本、CI、FAQ。
> 快速上手只看 §0 速查卡；第一次用建议通读 §3 和 §4；遇到报错直接查 §9。

---

## 0. 速查卡（TL;DR）

```bash
# —— 所有 just 命令都在 smartcar 根目录执行 ——
cd ~/Documents/Code/smartcar

just          # 列出所有命令
just doctor   # 检查本机环境是否就绪（新机器第一步）
just init     # 新机器还原 4 个子仓库（clone --recurse-submodules 后也建议跑一次）
just status   # 四仓库状态一览（每天的第一条命令）
just sync     # 批量拉取
just push     # 批量推送
just contracts        # 校验共享接口副本一致性
just contracts-apply  # 用 contracts/ 覆盖各仓库副本
just lock     # 记录当前四仓库版本组合（bump 指针）
just release v1.0.0   # 契约校验 -> 锁版本 -> 打总 tag
just gh-sync  # 下发 GitHub 统一配置（标签/分支保护/看板）
just fw-list            # 四工程固件产物 / 归档状态
just fw-build tc275_car  # 编译固件（esp32c6_car|smartcar_remote|tc275_car|tc275_sbl）
just fw-flash c6 all -m  # 烧录（串口自动识别；SBL+App 出厂整包: just fw-factory --flash）
```

三条铁律：

1. **平时开发 = 原来的方式**：直接进子仓库目录改代码、提交、推送，与多仓库机制无关。
2. **只有两类操作需要回到 smartcar 根目录**：改共享接口（§5.2/5.3）、发版（§5.4）。
3. **禁止在子仓库单方面改 contracts/ 里的接口文件副本**——CI 会发现并拒绝。

---

## 1. 为什么要有这个仓库（背景）

车工程原来有 4 个独立 GitHub 仓库，遇到四个问题：

| 痛点 | 后果 | 本方案的解法 |
|---|---|---|
| `proto_frames.[ch]` 在 C6 和遥控器里是两份拷贝 | 改协议要手动同步两处，文档已经漂移过 | `contracts/link/` 唯一权威版本 + 逐字节校验 |
| OTA 的 SBL/APP 必须配套发布，但版本关系靠脑记 | 上板才发现槽位/包头解析错位 | `contracts/ota/` + submodule 指针记录整车版本组合 |
| 标签/CI/分支保护在每个仓库各配一份 | 手动对齐，越拖越散 | `sync-gh.sh` 一条命令批量下发 |
| 四个仓库来回切，状态记不清 | 经常忘记哪个仓库没推 | `just status/sync/push` 批量命令 |

meta 仓库（smartcar）本身**不含业务代码**，只承担三个职责：

1. **版本配套清单**：submodule 指针即"当前用的是每个子仓库的哪个提交"。
2. **共享接口契约**：`contracts/` 是跨仓库接口的唯一权威版本。
3. **批量操作入口**：justfile + scripts/。

---

## 2. 目录结构逐项说明

```
smartcar/
├── .gitmodules              # submodule 声明：URL + path + branch=main
├── esp32c6_car/             # 子仓库①：ESP32-C6 小车主控固件（ESP-IDF）
├── smartcar_remote/         # 子仓库②：ESP32-S3 遥控器（LVGL + proto）
├── tc275_car/               # 子仓库③：TC275 车体控制固件（TriCore）
├── tc275_sbl/                # 子仓库④：TC275 OTA 二级引导（PFlash 双 bank + 回滚）
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
│   ├── repos.sh             # 四子仓库批量 status/sync/push/fix-head
│   ├── sync-gh.sh           # GitHub 配置批量下发
│   └── doctor.sh            # 本机环境自检
├── firmware/               # 固件统一入口（§5.6）
│   ├── fw.py               # 四工程编译/烧录/归档一条命令（纯 Python 标准库）
│   └── dist/               # 归档产物：<工程>/<时间戳-g提交号>/ + manifest.json（gitignore）
├── .github/workflows/contracts.yml   # CI 门禁：每次 push/PR 自动跑契约校验（Linux+Windows）
├── justfile                 # 命令入口（§6 逐条说明）
├── .gitattributes           # 全仓 LF；.bat/.cmd 强制 CRLF（跨平台校验一致的前提）
└── README.md                # 项目简介（本文是详细版）
```

**判断一个文件该不该进 `contracts/`**：只有满足"**两个及以上仓库需要逐字节一致，且必须同步生效**"的接口文件才进；仓库私有的实现、构建脚本（CMakeLists、Makefile）一律留在各仓库。

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

### 3.3 克隆还原

```bash
git clone --recurse-submodules https://github.com/lilicqyu-ship-it/smartcar.git
cd smartcar
just init      # 补齐/更新子仓库，并把它们全部切回 main 分支
just doctor    # 最后确认一遍
```

> 已 clone 忘了 `--recurse-submodules`？补一句 `just init` 即可。

---

## 4. git submodule 五分钟速成（读懂后面的操作必需）

把 submodule 想成"meta 仓库里存了一张**书签**，记录每个子仓库当前用的提交号"：

- 子仓库是**完全独立的 git 仓库**：自己的提交历史、自己的 GitHub 远端、自己的分支。
- meta 只存一个 40 位提交号（gitlink，模式 160000）。"bump 指针"= 把书签改指向新提交。
- `.gitmodules` 里的 `branch = main` 只是给 `git submodule update --remote` 用的提示；**clone/init 之后子仓库默认停在 detached HEAD**（恰好停在书签指向的提交），所以有 `just init` 里的 `fix-head` 步骤。

由此产生两种容易困惑的状态（都是正常的）：

| meta 里 `git status` 显示 | 含义 | 处理 |
|---|---|---|
| `M esp32c6_car`（新提交） | 子仓库 main 前进了，书签还指着旧提交 | 想锁定就 `just lock`；不急可攒着 |
| 子仓库里 `* (no branch)` | detached HEAD | `just fix-head` 或 `git checkout main` |

**在 detached HEAD 上误提交了怎么办**：

```bash
cd 出问题的子仓库
git branch rescue            # 把当前提交存成分支
git checkout main
git cherry-pick rescue       # 摘回来
git branch -D rescue
```

---

## 5. 日常场景操作手册

### 5.1 场景 A：只改一个固件（90% 的情况）

和以前完全一样，直接进子仓库：

```bash
cd ~/Documents/Code/smartcar/esp32c6_car
# ……改代码、idf.py build、单测、git add/commit/push，一切照旧
```

批量视角（在 smartcar 根目录）：

```bash
just status   # 谁有脏文件、谁没推送，一目了然
just sync     # 下班前拉一下
just push     # 批量推送（detached 的仓库自动跳过）
```

推送后 meta 的书签自然"过期"——**不影响任何东西**，等你某天想固定版本组合时再 `just lock`。

### 5.2 场景 B：改 LINK 协议（contracts/link/）

适用：`proto_frames.[ch]` 的帧格式、命令表、CRC 参数等。C6 和遥控器必须同时升级。

```bash
cd ~/Documents/Code/smartcar

# ① 改权威版本（contracts/link/ 下的文件）
$EDITOR contracts/link/proto_frames.h

# ② 一键同步到所有副本（esp32c6_car 和 smartcar_remote）
just contracts-apply

# ③ 分别进两个仓库验证、提交、推送
cd esp32c6_car
idf.py build && (cd test/host && ./test_proto.sh 2>/dev/null || true)   # 按各自实际测试方式
git add -A && git commit -m "feat(proto): 新增 0x42 帧命令" && git push
cd ../smartcar_remote
idf.py build
git add -A && git commit -m "feat(proto): 同步 0x42 帧命令" && git push

# ④ 回 meta 锁定"两端已同步到新协议"这个事实
cd .. && just lock
git push
```

推送后 meta CI 自动跑契约校验（§8）。**本地想提前验证**：`just contracts`。

### 5.3 场景 C：改 OTA 接口 / 验签（contracts/ota/、contracts/crypto/）

适用：槽位布局、镜像头、签名范围、公钥、ed25519/SHA-512 实现。SBL 和 App 必须一致，否则只能上板才发现。

流程与 §5.2 完全相同，只是副本方变成 `tc275_sbl/mw/ota|crypto/`、`tc275_car/mw/ota|crypto/`、`esp32c6_car/components/c6_ota/`，涉及仓库更多：

1. 改 `contracts/ota/...` 或 `contracts/crypto/...`
2. `just contracts-apply`（同步进所有相关仓库）
3. **每个**相关仓库：构建验证 → commit → push
4. `just lock && git push`

> ⚠️ OTA 接口变更意味着**旧 SBL 无法校验新 App 包**（或反之）。这类改动要在 `just lock` 的说明里写清兼容性影响，并优先走"新增字段/版本号前向兼容"而不是改老字段。

### 5.4 场景 D：发版（整车版本组合）

前提：四个子仓库各自已经把要发的提交推上 main，并建议各自打了 tag（沿用各仓库习惯，如 `v0.1.2`）。

```bash
cd ~/Documents/Code/smartcar

just release v1.2.0
# 等价于：契约校验 → just lock（bump 四个指针）→ git tag -a v1.2.0（tag 说明里自动写入四个提交号）

git push origin main --tags     # release 配方不自动 push，确认后手动推
```

之后任何一台机器 `git checkout v1.2.0` + `git submodule update --init` 就能**精确复现当时整车的全部代码**。OTA 场景下 SBL 和 App 的配套关系由这一步固化。

日常（非发版）想记录版本组合：`just lock`（只 bump 指针不打 tag）。

### 5.5 场景 E：GitHub 配置（标签 / 分支保护 / 看板）

一次性配置已下发（§7.2）。之后：

- **新建 issue** 时打统一标签（§7.2 标签表）；跨仓库问题建在哪边都行，看板会聚合。
- 加了新 issue 后跑 `just gh-sync`，看板 [Smartcar](https://github.com/users/lilicqyu-ship-it/projects) 自动收录所有仓库的 open issue（重复跑安全，已在板上的会 skip）。
- 新增第 5 个子仓库后，改 `scripts/sync-gh.sh` 的 `REPOS` 数组即可批量生效。

### 5.6 场景 F：编译 / 烧录 / 归档固件（firmware/）

四工程的构建烧录方式各不相同（ESP-IDF 环境、TASKING 命令行、AURIXFlasher），
`firmware/fw.py` 把它们收拢成一套命令，在 smartcar 根目录即可操作：

```bash
just fw-list                    # 各工程有没有产物、最近构建时间、最新归档
just fw-build tc275_car         # 编译一个工程（--collect 顺带归档）
just fw-build sbl --collect     # 别名: c6 / remote / app / sbl
just fw-flash esp32c6_car       # 烧录（ESP 串口按 USB VID 自动识别，-p COMx 指定）
just fw-flash c6 all -m         # 透传子命令/参数（assets+固件+监视器）
just fw-collect                 # 四工程产物全部归档到 firmware/dist/
just fw-factory                 # SBL + App 槽 A 合成 factory_full.hex（--flash 一步烧录）
```

归档目录名带**子仓库提交号**（脏工作区加 `-dirty`），manifest.json 记录
产物清单与来源路径——烧到板子上的固件永远能对回源码版本。

前置条件（§7.5 有细节）：ESP 两工程需 EIM 装的 ESP-IDF v6.1（自动发现）；
TC275 两工程需完整版 TASKING（ADS 内置版禁止 IDE 外运行）+ 首次在 ADS 里
构建一次以生成构建文件；TC275 烧录需 DAS 服务在跑。

TC275 两工程另有一条 **SCons 直编路线**（`just scons-car` / `just scons-sbl`）：
SConstruct 直接解析 `.cproject` 取 include/宏/源码排除，编译链接参数复刻 ADS
生成命令行，**无需先在 ADS 里构建**；产物在 `<仓库>/build/tasking-debug/`
（elf/hex/map），与 ADS 产物体积一致（tc275_sbl 逐字节相同）。余参透传：
`just scons-sbl size`、`just scons-car cfg=release`、`-c` 清理。

---

## 6. just 命令手册

| 命令 | 作用 | 备注 |
|---|---|---|
| `just` | 列出全部命令 | |
| `just doctor` | 本机环境自检 | 新机器第一步（§3.2） |
| `just init` | 还原子仓库 + 全部切回 main | 等价 `git submodule update --init --recursive` + `repos.sh fix-head` |
| `just fix-head` | 仅把四个子仓库切回 main | 治 detached HEAD |
| `just status` | 四仓库：分支/脏文件/未推送数/最新提交 | 每天第一条 |
| `just sync` | 批量 `fetch --all --prune` + fast-forward pull | detached 或无 upstream 的仓库只 fetch |
| `just push` | 批量 push 当前分支 | 无 upstream 自动 `push -u`；detached 跳过 |
| `just contracts` | 契约校验（只读） | 与 CI 同款，推前自查 |
| `just contracts-apply` | 用 contracts/ 覆盖各仓库副本 | 之后**必须**在各仓库提交 |
| `just lock` | 契约校验 → bump 四个指针 → 提交 | 校验不过会中止；可带说明 `just lock "接入 0x42 命令"` |
| `just release <tag>` | lock + 打附注 tag（含四提交号） | 不自动 push：`git push origin main --tags` |
| `just gh-sync` | 标签/分支保护/看板批量下发 | 需 gh 登录 + 权限（§7.2） |
| `just fw-list` | 四工程固件产物 / 归档状态一览 | §5.6 |
| `just fw-build <工程>` | 编译固件 | 工程: esp32c6_car/smartcar_remote/tc275_car/tc275_sbl，别名 c6/remote/app/sbl（§5.6） |
| `just fw-flash <工程> [参数]` | 烧录固件 | 参数透传各工程入口；ESP 串口自动识别（§5.6） |
| `just fw-collect [工程]` | 归档产物到 firmware/dist/ | 目录名带子仓库提交号，含 manifest（§5.6） |
| `just fw-factory` | SBL+App 出厂整包合成 | `--flash` 顺带 AURIXFlasher 整包烧录（§5.6） |
| `just scons-car` / `just scons-sbl` | TASKING SCons 直编 TC275 工程 | 免 ADS 生成文件；余参透传 `size`/`cfg=release`/`-c`（§5.6） |
| `just fw-clean` | 清空 firmware/dist/ | |

Windows 注意：justfile 已写死 `windows-shell := Git Bash`；Git 装在非默认路径时临时覆盖：
`just --shell "D:/Git/bin/bash.exe" --shell-arg -cu status`

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
  # 新增行：contracts/内路径  各仓库副本路径...
  "ota/ota_layout.h     tc275_sbl/mw/ota/ota_layout.h  tc275_car/mw/ota/ota_layout.h"
)
```

当前覆盖 27 项：link 2 文件 ×2 仓库、ota 4 文件 ×2 仓库（tc275_sbl+tc275_car）、crypto 5 文件 ×3 仓库（esp32c6_car+tc275_sbl+tc275_car）。

### 7.2 sync-gh.sh —— GitHub 配置批量下发

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

### 7.3 repos.sh —— 批量操作（just 配方的实现层）

`status|sync|push|fix-head` 四个子命令，逻辑放脚本而非 justfile shebang 配方是为了 Windows 兼容（just 的 shebang 配方依赖 cygpath）。

### 7.4 doctor.sh —— 环境自检

工具版本（git/just/gh/python3）、bash≥4、gh 登录、core.autocrlf、（Windows）longpaths 与路径长度、四个子仓库工作区换行是否 LF（发现 CRLF 会给出修复命令：`git -C <仓库> add --renormalize .`）。

---

### 7.5 fw.py —— 固件统一管理（just fw-* 配方的实现层）

`python firmware/fw.py <命令>`，子命令 `list / build / flash / collect /
factory / clean`，详见 [firmware/README.md](../firmware/README.md)。只做发现、
委托与归档，不重复实现各工程的构建烧录：

| 工程 | 编译 | 烧录 |
|---|---|---|
| esp32c6_car | 委托 `esp32c6_car/flash.py build`（EIM 自动发现，含 assets） | 委托 `flash.py`（full/assets/all，-p 串口，-m 监视） |
| smartcar_remote | EIM 环境 + `idf.py build` | `idf.py -p <串口> flash`，串口自动识别 |
| tc275_car | 解析 ADS 生成的 subdir.mk → 完整版 TASKING 增量重编 | `tc275_sbl/tools/flash.py flash <App槽A.hex>` |
| tc275_sbl | 委托 `tools/build_sbl.sh` | `tools/flash.py flash Debug/tc275_sbl.hex` |

tc275_car 的命令行编译说明：ADS 生成的 makefile 带字面引号目标，make/mktc
都无法驱动；fw.py 解析生成文件提取每个 .c 的 cctc 命令直接执行，标志与
IDE 零漂移。**首次需在 ADS 里构建一次**（生成构建文件）；增量只看 .c 的
mtime，改头文件后 touch 对应 .c。环境变量 `FW_TASKING` / `FW_IDF_PROFILE`
可覆盖工具路径。

---

## 8. CI 说明（.github/workflows/contracts.yml）

- **触发**：push 到 main、任何 PR、手动（workflow_dispatch），仓库：smartcar（meta）
- **矩阵**：ubuntu-latest + windows-latest 双平台——Windows 那路保证脚本在 Git Bash 下可用、换行规则两平台一致
- **内容**：`actions/checkout@v4`（`submodules: recursive`，子仓库均 public，默认 token 即可拉取）→ `check-contracts.sh` → `repos.sh status` 冒烟
- **红了怎么修**：见 §9 Q3

---

## 9. 故障排查 FAQ

**Q1：`just` 提示找不到命令？**
必须在 smartcar 根目录跑（justfile 在那）；没装就 `brew install just`（Windows: scoop）。

**Q2：子仓库里 `git branch` 显示 `* (no branch)`？**
`submodule update` 后的正常现象。`just fix-head` 一键切回 main。在 detached HEAD 上已提交的救援见 §4。

**Q3：CI 契约校验红了 / `just contracts` 报 FAIL？**
输出里每个 FAIL 行都写了哪个副本不一致。两种修法：
- 副本是对的、契约旧了 → 把改动补进 `contracts/` 对应文件 → `just contracts-apply` 确认无 diff → 提交 contracts 和各仓库；
- 契约是对的、有人手改了副本 → `just contracts-apply` 覆盖回权威版本 → 在各仓库提交。

**Q4：`git push` 被拒 non-fast-forward？**
分支落后了。先 `just sync` 再推。**main 已禁 force-push**（分支保护），不要试图强推。

**Q5：sync-gh.sh 某项 FAIL（403）？**
token 权限不够（§7.2 末尾），补权限后重跑，脚本幂等。

**Q6：Windows 上 just 执行的是 WSL 的 bash，git 凭据对不上？**
justfile 已写死 Git Bash 路径；若 Git 装在非默认位置，见 §6 的 `--shell` 覆盖法。裸 `bash` 在 Windows 常解析到 `System32\bash.exe`（WSL），看不到 Windows 侧凭据。

**Q7：大量 CRLF 警告 / 校验因换行失败？**
`git -C <仓库> add --renormalize .` 重新归一化后提交；确认 `core.autocrlf` 未设 true（§3.2）。

**Q8：meta 里子仓库显示 "new commits" 但我不确定该不该 lock？**
不急。lock 只是"把书签按下去"，随时可做；发版前必须做（`just release` 内含）。

**Q9：想加第 5 个子仓库（比如 tc275_car-freecad）？**

```bash
git submodule add -b main https://github.com/lilicqyu-ship-it/tc275_car-freecad.git tc275_car-freecad
# 然后同步更新：scripts/repos.sh、scripts/check-contracts.sh（如有接口）、scripts/sync-gh.sh 的 REPOS、README 表格
```

**Q10：误把私有接口文件塞进了 contracts/？**
从声明表和 contracts/ 删掉即可，历史无副作用——contracts/ 只是拷贝，删除不影响任何仓库构建。

---

## 10. 约定规范

- **commit**：conventional 风格 + 中文描述，如 `feat(proto): ...`、`fix(c6_http): ...`、`chore: ...`（沿用各仓库既有风格）
- **tag**：子仓库沿用各自 `vX.Y.Z`；meta 总 tag 用 `just release vX.Y.Z`，tag 说明自动含四提交号
- **分支**：统一 main 单分支 + 短期 feature 分支（推 PR 或直接合，分支保护只挡 force-push/删除）
- **issue**：打 §7.2 标签表中的标签；标题建议 `[c6]/[remote]/[tc275]/[sbl]` 前缀辅助看板阅读
- **接口变更**：一律走 `contracts/` → `contracts-apply` → 各仓库提交 → `just lock`，禁止跳过契约直接改副本（§9 Q3 会兜底）

---

## 11. 附录

### 11.1 当前契约清单

| contracts/ 文件 | 内容 | 各仓库副本 |
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
- `README.md` —— 本仓库简介与命令速览
