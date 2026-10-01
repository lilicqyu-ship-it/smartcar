# smartcar 多仓库总控
# 四个子仓库在 .gitmodules 中固定 branch=main；本仓库只做总控/清单/契约。
set shell := ["bash", "-cu"]

repos := "esp32c6_car smartcar_remote tc275_car myCarSbl"

# 列出所有命令
default:
    @just --list

# 新机器一键还原 4 个子仓库（clone 本仓库后先跑这个）
init:
    git submodule update --init --recursive

# 四仓库状态一览：分支 / 未提交文件数 / 最新提交
status:
    #!/usr/bin/env bash
    set -euo pipefail
    printf '%-17s %-10s %5s  %s\n' REPO BRANCH DIRTY HEAD
    for r in {{repos}}; do
        b="$(git -C "$r" branch --show-current)"
        [[ -z "$b" ]] && b="(detached)"
        d="$(git -C "$r" status --porcelain | wc -l | tr -d ' ')"
        h="$(git -C "$r" log --oneline -1 2>/dev/null | head -c 70)"
        printf '%-17s %-10s %5s  %s\n' "$r" "$b" "$d" "$h"
    done

# 批量 fetch --all --prune；在分支上的仓库顺手 fast-forward
sync:
    #!/usr/bin/env bash
    set -euo pipefail
    for r in {{repos}}; do
        echo "== $r =="
        git -C "$r" fetch --all --prune -q
        b="$(git -C "$r" branch --show-current)"
        if [[ -n "$b" ]] && git -C "$r" rev-parse --abbrev-ref '@{upstream}' &>/dev/null; then
            git -C "$r" pull --ff-only -q && echo "  pulled $b"
        else
            echo "  (detached 或无 upstream，跳过 pull)"
        fi
    done

# 批量 push 当前分支；无 upstream 时自动建立（detached 的仓库跳过）
push:
    #!/usr/bin/env bash
    set -euo pipefail
    for r in {{repos}}; do
        b="$(git -C "$r" branch --show-current)"
        [[ -z "$b" ]] && { echo "skip $r (detached)"; continue; }
        echo "== push $r ($b) =="
        if git -C "$r" rev-parse --abbrev-ref '@{upstream}' &>/dev/null; then
            git -C "$r" push
        else
            git -C "$r" push -u origin "$b"
        fi
    done

# 校验各仓库接口副本与 contracts/ 一致（CI 门禁同款）
contracts:
    @bash scripts/check-contracts.sh

# 用 contracts/ 覆盖各仓库副本；之后需在各子仓库分别提交
contracts-apply:
    @bash scripts/check-contracts.sh --apply

# 批量下发 GitHub 配置：统一标签 / 分支保护 / 看板
gh-sync:
    @bash scripts/sync-gh.sh all
