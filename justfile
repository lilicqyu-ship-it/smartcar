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
    git add esp32c6_car smartcar_remote tc275_car myCarSbl
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
