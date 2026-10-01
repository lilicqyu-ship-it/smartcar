# smartcar 固件 monorepo 总控
# 四个固件工程（esp32c6_car / smartcar_remote / tc275_car / tc275_sbl）与 contracts/
# 共享接口、firmware/fw.py 工具链同仓管理：一次提交一次推送，git status 只会有真实
# 文件差异；版本 tag 带工程前缀（c6/ r-s3/ app/ sbl/，与 fw.py 工程别名一致）。
#
# 跨平台：所有配方只调用 bash + scripts/*.sh，不用 shebang 配方。
# Windows 上必须是 Git Bash —— 裸 "bash" 在 Windows 常解析到 WSL 的
# System32\bash.exe（看不到 Windows 侧 git/gh 凭据），所以写死 Git Bash 路径。
# Git 装在别处时临时覆盖:  just --shell "D:/Git/bin/bash.exe" --shell-arg -cu <配方>
set shell := ["bash", "-cu"]
set windows-shell := ["C:/Program Files/Git/bin/bash.exe", "-cu"]

# 列出所有命令
default:
    @just --list

# 校验各工程接口副本与 contracts/ 一致（CI 门禁同款）
contracts:
    @bash scripts/check-contracts.sh

# 用 contracts/ 覆盖各工程副本；接口变更与副本同步放进同一个提交
contracts-apply:
    @bash scripts/check-contracts.sh --apply

# 打工程版本 tag（例: just tag c6 v1.0.1 → tag c6/v1.0.1；push 后触发对应 Release workflow）
tag proj ver:
    git tag -a "{{proj}}/{{ver}}" -m "{{proj}} {{ver}}"
    @echo "已建 tag {{proj}}/{{ver}}——git push origin {{proj}}/{{ver}} 后触发 Release"

# —— 固件统一管理（firmware/fw.py；详见 firmware/README.md）——
# 工程名: esp32c6_car | smartcar_remote | tc275_car | tc275_sbl（别名 c6/r-s3/app/sbl）
py := if os() == "windows" { "python" } else { "python3" }

# 四工程固件产物 / 归档状态一览
fw-list:
    @{{py}} firmware/fw.py list

# 编译固件: just fw-build <project>（成功后自动归档 firmware/dist/；--no-collect 跳过）
fw-build project *args:
    @{{py}} firmware/fw.py build {{project}} {{args}}

# 一条指令编译+烧录: 默认先增量编译再烧，不会烧到旧产物
#   --no-build 跳过编译只烧最近构建；编译不触发归档（归档走 fw-build）
#   透传参数原样转给各工程烧录入口（esp32c6_car all -m / r-s3 -p COM7 monitor / tc275 --id N）
# 用法: just fw-flash <project> [透传参数]
fw-flash project *args:
    @{{py}} firmware/fw.py flash {{project}} {{args}}

# 归档产物到 firmware/dist/<工程>/[v版本-]<时间戳-g提交号>/（省略 project 则四工程全归档）
fw-collect project="":
    @{{py}} firmware/fw.py collect {{project}}

# 把 firmware/dist 的新归档提交并推送 GitHub（固件镜像版本管理；镜像+manifest 入库）
fw-dist msg="fw(dist): 归档固件镜像（构建产物入库）":
    git add firmware/dist
    git diff --cached --quiet || git commit -m "{{msg}}"
    git push origin main

# SBL+App 出厂整包: 构建两工程 + 合成 factory_full.hex（--flash 顺带烧录）
fw-factory *args:
    @{{py}} firmware/fw.py factory {{args}}

# 一条指令编译+OTA: 编译最新代码打签包写进遥控器暂存分区，
#   遥控器 Settings > FIRMWARE 页点更新（S3 用自己的 token 推给 C6/TC275）
#   --no-build 打包最近构建；--direct PC 直推（需在车网络）；--port/--version/--file/--seed 透传 fw.py ota
# 用法: just fw-ota c6 | just fw-ota app
fw-ota project *args:
    @{{py}} firmware/fw.py ota {{project}} {{args}}

# 清空 firmware/dist/ 归档
fw-clean:
    @{{py}} firmware/fw.py clean --yes

# —— TASKING SCons 直编（tc275_car / tc275_sbl）——
# 各工程根目录的 SConstruct 直接解析 .cproject（include/宏/源码排除），
# 不依赖 ADS 生成文件；需本机装完整版 TASKING TriCore v6.3r1 + pip install scons。
# 产物在 <工程>/build/tasking-debug/（elf/hex/map），配置与 ADS Debug 完全同源。
# 并行度默认 -j8（SConstruct 内置），命令行 -j 可覆盖。

# SCons 编译 tc275_car（余参透传: cfg=release / opt=-O2 / size / -c）
scons-car *args:
    cd tc275_car && {{py}} -m SCons {{args}}

# SCons 编译 tc275_sbl（余参透传: cfg=release / opt=-O2 / size / -c）
scons-sbl *args:
    cd tc275_sbl && {{py}} -m SCons {{args}}

# 检查本机开发环境（git/just/gh/bash 版本、换行与长路径配置）
doctor:
    @bash scripts/doctor.sh

# —— 旧多仓架构遗留 ——
# 批量下发 GitHub 配置到四个旧工程仓库（统一标签/分支保护/看板）；
# 旧仓库在 GitHub 归档（archive）后可连同 scripts/sync-gh.sh 一起删除
gh-sync:
    @bash scripts/sync-gh.sh all
