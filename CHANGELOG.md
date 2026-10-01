# 更新日志

本项目的所有显著变更都将记录在此文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
并且本项目遵循[语义化版本](https://semver.org/lang/zh-CN/)。

## [未发布]

### 变更
- **仓库架构 monorepo 化**：四个固件工程（esp32c6_car / smartcar_remote / tc275_car / tc275_sbl）
  以 subtree 保历史并入本仓（各仓原提交号全部保留可追溯），退役 submodule 总控架构——
  一次提交一次推送、跨工程改动原子化，`git status` 不再有 "(new commits)" 指针噪音；
  CI 收编至根目录按 paths 触发（tc275_car 的 C6 交叉校验改用仓内源码）；
  版本 tag 加工程前缀（c6/ r-s3/ app/ sbl/，与 fw.py 别名一致）；
  justfile 退役 init/status/sync/push/fix-head/lock/pin 配方，新增 tag 配方；
  旧工程仓库在 GitHub 归档只读，历史 tag 仍可从旧仓库查阅
- fw.py `flash` / `ota` 默认先增量编译再烧录/打包（一条指令到位，不再烧/推旧构建产物）；`--no-build`（ota 另有 `--file`）跳过编译，flash/ota 内的编译不触发归档

### 修复
- fw.py `flash` 透传参数（`-m`/`-p`/`--id` 等选项）被 argparse 误拒（改 REMAINDER 原样透传）

## [1.0.0] - 2026-10-01

首个整车稳定版 tag。本版锁定：esp32c6_car@v1.0.0、smartcar_remote@v1.0.0、tc275_car@v1.0.0、tc275_sbl@v1.0.0（四仓版本号统一升至 1.0.0）。

### 新增
- About 页固件版本 tap 刷新全链路：S3 → C6（`{"t":"tcver"}` → SPI DIAG 0x53/0x24）→ TC275 即答版本信标；C6 侧走 /api/health
- 四仓版本真源（C6 PROJECT_VER / TC275 APP+SBL app_version.h）统一落位 1.0.0

### 修复
- C6 send_json 状态行补完整状态码（S3 /api/health 超时根因）
- S3 hello.tc 占位串覆盖 tc_on；WS 连续失败强制重连 Wi-Fi 自愈

## [0.1.0] - 2026-10-01

首个整车版本 tag。本仓版本即整车总 tag：一次 lock 组合出的四个子仓版本即为一次整车发布，
子仓明细见各自 CHANGELOG（esp32c6_car / smartcar_remote / tc275_car / tc275_sbl）。

本版锁定：esp32c6_car@v0.1.2+30、smartcar_remote@v0.1.0、tc275_car@v0.2.2、tc275_sbl@v0.1.0。

### 新增
- 多仓库总控：submodule 清单 + contracts 共享接口 + 批量脚本
- contracts：OTA 接口扩至 tc275_car，新增 crypto 三方验签契约与 ota_keys
- firmware/fw.py：四工程固件统一管理（编译/烧录/归档/出厂整包）；dist 归档入库（固件镜像 + manifest 版本管理）
- justfile：fw-* 固件配方 + scons-car/scons-sbl TASKING 直编配方
- 多仓库详细使用指南（概念/场景/命令/脚本/CI/FAQ）
- CI：contracts 门禁双平台（Linux+Windows）；doctor/repos 跨平台脚本，justfile 委托 repos.sh

### 变更
- 四仓命名统一 `<平台>_<功能>`：myCarSbl 仓库改名 tc275_sbl
- TC275 双仓命令行构建统一 SCons（`python -m SCons`，删除 build_sbl.sh）

[未发布]: https://github.com/lilicqyu-ship-it/smartcar/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/lilicqyu-ship-it/smartcar/releases/tag/v1.0.0
[0.1.0]: https://github.com/lilicqyu-ship-it/smartcar/releases/tag/v0.1.0
