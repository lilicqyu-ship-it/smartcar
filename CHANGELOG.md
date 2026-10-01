# 更新日志

本项目的所有显著变更都将记录在此文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
并且本项目遵循[语义化版本](https://semver.org/lang/zh-CN/)。

## [未发布]

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

[未发布]: https://github.com/lilicqyu-ship-it/smartcar/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/lilicqyu-ship-it/smartcar/releases/tag/v0.1.0
