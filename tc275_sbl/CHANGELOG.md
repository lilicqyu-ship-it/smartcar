# 更新日志

本项目的所有显著变更都将记录在此文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
并且本项目遵循[语义化版本](https://semver.org/lang/zh-CN/)。

## [未发布]

## [1.0.0] - 2026-10-01

首个稳定版 tag，对齐 `mw/app_version.h` 1.0.0；自 0.1.0 无代码变更。

## [0.1.0] - 2026-10-01

首个版本 tag，对齐 `mw/app_version.h` 0.1.0。

### 新增
- OTA SBL：双 bank 引导 + TCFW 验签 + OTA 接收栈（doc/24）
- SBL+App 整包合成：tools/merge_hex.py（Intel-HEX 合并，整片烧录一个文件）
- tools/flash.py：AURIXFlasher CLI 的 Python 封装（命令行烧录）
- 固件版本号落地：`mw/app_version.h` 唯一真源，产物名携带版本（SBLFW tc275_sbl vX.Y.Z）
- 版本串定址 code flash 0x80007E00（sblfls0 尾部）——App 可从该地址直读
- ADS 调试启动配置
- CI：host 单测接入 GitHub Actions（test/host make check）

### 变更
- 命令行构建统一 SCons（删除 build_sbl.sh）；SConsignFile 收进 build/
- 工程名 myCarSbl → tc275_sbl，清除 Blinky 模板残留
- 统一 LF 换行（.gitattributes）

### 修复
- ADS 构建/链接：编译器 include 路径补工程根；源文件改文件相对包含 + SBL lsl 去 CPU1/2 栈

[未发布]: https://github.com/lilicqyu-ship-it/tc275_sbl/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/lilicqyu-ship-it/tc275_sbl/releases/tag/v1.0.0
[0.1.0]: https://github.com/lilicqyu-ship-it/tc275_sbl/releases/tag/v0.1.0
