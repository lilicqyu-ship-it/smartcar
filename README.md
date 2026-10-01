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
| ota/ota_layout.h | myCarSbl `mw/ota/` |
| ota/ota_meta.h | myCarSbl `mw/ota/` |
| ota/tcfw_bundle.h | myCarSbl `mw/ota/` |

> `tc275_car/mw/proto/protocol.[ch]` 是 LINK 协议的旧版实现，待其采纳
> `proto_frames.[ch]`（见 proto_frames.h 头注释的既定计划）后，
> 在 `scripts/check-contracts.sh` 的声明表中追加对应路径即可纳入校验。
> tc275_car 侧 OTA 尚在设计稿阶段（doc/24），采纳 contracts/ota/*.h 后同样追加。

## GitHub 统一配置

`just gh-sync` 会：统一 7 个标准标签（bug/enhancement/documentation/ci/proto/ota/hardware）、
给 4 个仓库的 main 分支加"禁 force-push/禁删除"保护、把 4 仓库的 open issue
汇总到 user 级看板 **Smartcar**。需要 token 有对应写权限（Issues / Administration / 项目）。
