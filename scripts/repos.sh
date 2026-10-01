#!/usr/bin/env bash
# repos.sh - 四子仓库批量操作（macOS / Linux / Windows Git Bash 通用）
#
# 逻辑放在脚本而非 justfile 的 shebang 配方里：just 在 Windows 上执行
# shebang 配方依赖 cygpath，放进普通脚本后只需要 bash 本身。
#
# 用法: repos.sh status|sync|push|fix-head
set -euo pipefail

cd "$(dirname "$0")/.."
REPOS=(esp32c6_car smartcar_remote tc275_car tc275_sbl)

has_upstream() { git -C "$1" rev-parse --abbrev-ref '@{upstream}' &>/dev/null; }

cmd_status() {
    printf '%-17s %-10s %5s %6s  %s\n' REPO BRANCH DIRTY AHEAD HEAD
    for r in "${REPOS[@]}"; do
        b="$(git -C "$r" branch --show-current)"
        [[ -z "$b" ]] && b="(detached)"
        d="$(git -C "$r" status --porcelain | wc -l | tr -d ' ')"
        a="-"
        has_upstream "$r" && a="$(git -C "$r" rev-list --count '@{upstream}..HEAD')"
        h="$(git -C "$r" log --oneline -1 2>/dev/null | cut -c1-60)"
        printf '%-17s %-10s %5s %6s  %s\n' "$r" "$b" "$d" "$a" "$h"
    done
}

cmd_sync() {
    for r in "${REPOS[@]}"; do
        echo "== $r =="
        git -C "$r" fetch --all --prune -q
        b="$(git -C "$r" branch --show-current)"
        if [[ -n "$b" ]] && has_upstream "$r"; then
            git -C "$r" pull --ff-only -q && echo "  pulled $b"
        else
            echo "  (detached 或无 upstream，跳过 pull)"
        fi
    done
}

cmd_push() {
    for r in "${REPOS[@]}"; do
        b="$(git -C "$r" branch --show-current)"
        [[ -z "$b" ]] && { echo "skip $r (detached)"; continue; }
        echo "== push $r ($b) =="
        if has_upstream "$r"; then
            git -C "$r" push origin "$b"
        else
            git -C "$r" push -u origin "$b"
        fi
    done
}

cmd_fix_head() {
    for r in "${REPOS[@]}"; do
        git -C "$r" checkout -q main && echo "$r -> main"
    done
}

case "${1:-}" in
    status)   cmd_status ;;
    sync)     cmd_sync ;;
    push)     cmd_push ;;
    fix-head) cmd_fix_head ;;
    *) echo "用法: $0 status|sync|push|fix-head" >&2; exit 2 ;;
esac
