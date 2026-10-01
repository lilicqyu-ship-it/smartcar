# smartcar

智能车工程的多仓库总控（meta）仓库。四个子仓库以 git submodule 形式挂在这里，
本仓库本身不含业务代码，负责三件事：

1. **版本配套清单** — submodule 指针即"整车版本组合"；发版时四仓库各打 tag，
   再在 meta 里 bump 指针并打总 tag。
2. **共享接口契约** — `contracts/` 是跨仓库接口的唯一权威版本，`scripts/check-contracts.sh`
   逐字节校验各仓库副本，杜绝拷贝漂移。
3. **批量操作入口** — `justfile` 一条命令操作四个仓库。

## 布局

```
smartcar/
├── esp32c6_car/       ESP32-C6 小车主控固件（ESP-IDF）
├── smartcar_remote/   ESP32-S3 遥控器（LVGL + proto）
├── tc275_car/         TC275 车体控制固件（TriCore/AUTOSAR 风格）
├── myCarSbl/          TC275 OTA 二级引导（PFlash 双 bank + 自动回滚）
├── contracts/
│   ├── link/          LINK 通信协议: proto_frames.[ch]（帧格式/命令表/CRC）
│   └── ota/           SBL↔App 接口: ota_layout.h（槽位/分区）、
│                      ota_meta.h（DFlash 启动元数据）、tcfw_bundle.h（TCFW 包头/签名范围）
├── scripts/
│   ├── sync-gh.sh     统一下发 GitHub 标签/分支保护/看板
│   └── check-contracts.sh  接口副本一致性校验（--apply 用 contracts/ 覆盖副本）
└── justfile           批量命令
```

## 新机器初始化

```bash
git clone --recurse-submodules https://github.com/lilicqyu-ship-it/smartcar.git
# 或 clone 后执行:
just init
```

## 常用命令

| 命令 | 作用 |
|---|---|
| `just status` | 四仓库分支/脏文件/最新提交一览 |
| `just sync` | 批量 fetch + fast-forward pull |
| `just push` | 批量 push 当前分支（无 upstream 自动建立） |
| `just contracts` | 校验接口副本与 contracts/ 一致 |
| `just contracts-apply` | 用 contracts/ 覆盖各仓库副本（修漂移） |
| `just gh-sync` | 下发 GitHub 配置（标签/分支保护/看板） |

## 改共享接口的流程

以 LINK 协议为例（`contracts/link/proto_frames.[ch]`）：

1. 在 meta 仓库修改 `contracts/link/`；
2. `just contracts-apply` 同步到 `esp32c6_car/components/c6_proto/` 与
   `smartcar_remote/main/proto/`；
3. 分别进入两个子仓库提交并 push，各自 CI 验证；
4. 在 meta 仓库 bump 两个 submodule 指针提交，契约与实现同步闭环。

## 校验覆盖范围

| contracts/ 文件 | 各仓库副本 |
|---|---|
| link/proto_frames.h/.c | esp32c6_car `components/c6_proto/`，smartcar_remote `main/proto/` |
| ota/ota_layout.h, ota_meta.h, tcfw_bundle.h, ota_keys.h | myCarSbl `mw/ota/`，tc275_car `mw/ota/` |
| crypto/ed25519v.[ch], sha512.[ch], c6_consts.h | esp32c6_car `components/c6_ota/`，myCarSbl `mw/crypto/`，tc275_car `mw/crypto/` |

CI：`.github/workflows/contracts.yml` 在 push/PR 时按 submodule 指针拉取四仓库并运行同一校验。

> `tc275_car/mw/proto/protocol.[ch]` 是 LINK 协议的旧版实现，待其采纳
> `proto_frames.[ch]`（见 proto_frames.h 头注释的既定计划）后，
> 在 `scripts/check-contracts.sh` 的声明表中追加对应路径即可纳入校验。

## 发版

```bash
just lock               # 校验契约 + 记录当前四仓库版本组合
just release r2026.10   # 校验 + 锁版本 + 打总 tag
git push origin main --tags
```

## GitHub 统一配置

`just gh-sync` 会：统一 7 个标准标签（bug/enhancement/documentation/ci/proto/ota/hardware）、
给 4 个仓库的 main 分支加"禁 force-push/禁删除"保护、把 4 仓库的 open issue
汇总到 user 级看板 **Smartcar**。需要 token 有对应写权限（Issues / Administration / 项目）。

## Windows 开发环境

TC275 / myCarSbl 只能在 Windows 的 AURIX Development Studio（TASKING）下编译，
ESP32 两仓库在 Windows / macOS 均可。

```powershell
winget install Git.Git Casey.Just GitHub.cli
git config --global core.longpaths true    # FreeRTOS 深层路径，必开
git config --global core.autocrlf false    # 换行由 .gitattributes 统一为 LF
gh auth login
git clone --recurse-submodules https://github.com/lilicqyu-ship-it/smartcar.git C:\Code\smartcar
cd C:\Code\smartcar; just init; just doctor
```

- 根目录放短路径（`C:\Code\smartcar`），避免超 260 字符。
- `justfile` 在 Windows 固定用 `C:/Program Files/Git/bin/bash.exe`，不用 WSL bash。
- 换行：五个仓库都有 `.gitattributes`（`eol=lf`，`.bat` 为 CRLF），两平台字节一致。
- ADS：`File → Import → Existing Projects into Workspace`，选 `tc275_car`（工程名 `myCar`）
  和 `myCarSbl`，**不要勾 Copy into workspace**，否则改动不在 git 里。
  `.launch` 里写死了 `C:/Code/TC275/AURIX-v1.10.36-workspace/...` 的 elf 路径和
  ADS 安装路径，换位置后需在 Debug Configurations 里重选一次 elf。
- ESP-IDF 用官方 Windows 安装器；`esp32c6_car/flash.bat` 可直接用（串口 `COMx`）。
