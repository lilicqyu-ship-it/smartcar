# smartcar 多仓库总控
# 四个子仓库在 .gitmodules 中固定 branch=main；本仓库只做总控/清单/契约。
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

# 新机器一键还原 4 个子仓库（clone 本仓库后先跑这个），并切回 main 避免 detached HEAD 上提交丢失
init:
    git submodule update --init --recursive
    bash scripts/repos.sh fix-head

# 所有子仓库切到 main（submodule update 后默认是 detached HEAD）
fix-head:
    @bash scripts/repos.sh fix-head

# 四仓库状态一览：分支 / 未提交文件数 / 未推送提交数 / 最新提交
status:
    @bash scripts/repos.sh status

# 批量 fetch --all --prune；在分支上的仓库顺手 fast-forward
sync:
    @bash scripts/repos.sh sync

# 修复: 子仓库 fast-forward 后父仓库报 "modified: xxx (new commits)"——
# 把四个子仓库当前 HEAD 固化进父仓库（只记指针，不走契约校验；发版组合请用 lock）
pin msg="pin: bump submodule versions":
    git add esp32c6_car smartcar_remote tc275_car tc275_sbl
    git diff --cached --quiet || git commit -m "{{msg}}"
    @echo "当前子仓库组合:" && git submodule status

# 批量 push 当前分支；无 upstream 时自动建立（detached 的仓库跳过）
push:
    @bash scripts/repos.sh push

# 校验各仓库接口副本与 contracts/ 一致（CI 门禁同款）
contracts:
    @bash scripts/check-contracts.sh

# 用 contracts/ 覆盖各仓库副本；之后需在各子仓库分别提交
contracts-apply:
    @bash scripts/check-contracts.sh --apply

# 记录当前四仓库版本组合（bump submodule 指针）；先过契约校验
lock msg="lock: bump submodule versions":
    bash scripts/check-contracts.sh
    git add esp32c6_car smartcar_remote tc275_car tc275_sbl
    git diff --cached --quiet || git commit -m "{{msg}}"

# 整车发版：校验契约 -> 锁版本 -> 打总 tag（push 需手动 git push origin main --tags）
release tag: (lock "release: " + tag)
    git tag -a "{{tag}}" -m "smartcar {{tag}}: $(git submodule status | awk '{print $2"@"$3}' | tr '\n' ' ')"
    @echo "tagged {{tag}}"

# 批量下发 GitHub 配置：统一标签 / 分支保护 / 看板
gh-sync:
    @bash scripts/sync-gh.sh all

# 检查本机开发环境（git/just/gh/bash 版本、换行与长路径配置）
doctor:
    @bash scripts/doctor.sh

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
# 各仓库根目录的 SConstruct 直接解析 .cproject（include/宏/源码排除），
# 不依赖 ADS 生成文件；需本机装完整版 TASKING TriCore v6.3r1 + pip install scons。
# 产物在 <仓库>/build/tasking-debug/（elf/hex/map），配置与 ADS Debug 完全同源。

# SCons 编译 tc275_car（余参透传: cfg=release / opt=-O2 / size / -c）
scons-car *args:
    cd tc275_car && {{py}} -m SCons -j8 {{args}}

# SCons 编译 tc275_sbl（余参透传: cfg=release / opt=-O2 / size / -c）
scons-sbl *args:
    cd tc275_sbl && {{py}} -m SCons -j8 {{args}}
