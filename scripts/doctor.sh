#!/usr/bin/env bash
# doctor.sh - 检查本机是否满足 smartcar 多仓库开发要求（macOS / Windows Git Bash）
set -uo pipefail
cd "$(dirname "$0")/.."

ok=0
pass() { echo "  OK   $*"; }
warn() { echo "  WARN $*"; }
fail() { echo "  FAIL $*"; ok=1; }

is_win=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) is_win=1 ;; esac

echo "== 工具 =="
for t in git just gh python3; do
    if command -v "$t" >/dev/null 2>&1; then pass "$t: $("$t" --version 2>&1 | head -1)"
    else
        [[ "$t" == python3 && $is_win == 1 ]] && command -v python >/dev/null && { pass "python: $(python --version 2>&1)"; continue; }
        fail "$t 未安装"
    fi
done
# sync-gh.sh 用到关联数组，需要 bash >= 4（macOS 自带 /bin/bash 是 3.2）
if (( BASH_VERSINFO[0] >= 4 )); then pass "bash $BASH_VERSION"; else fail "bash $BASH_VERSION < 4（macOS: brew install bash）"; fi
gh auth status >/dev/null 2>&1 && pass "gh 已登录" || warn "gh 未登录（gh auth login），just gh-sync 不可用"

echo "== git 配置 =="
# 换行由各仓库 .gitattributes 统一（eol=lf），autocrlf 设 false 避免与其叠加
ac="$(git config --get core.autocrlf || echo unset)"
[[ "$ac" == "true" ]] && warn "core.autocrlf=true（建议: git config --global core.autocrlf false）" || pass "core.autocrlf=$ac"
if (( is_win )); then
    [[ "$(git config --get core.longpaths)" == "true" ]] && pass "core.longpaths=true" \
        || fail "core.longpaths 未开启（git config --global core.longpaths true；FreeRTOS 路径较深）"
    root="$(pwd -W 2>/dev/null || pwd)"
    (( ${#root} <= 40 )) && pass "仓库根路径长度 ${#root}: $root" \
        || warn "仓库根路径较长（${#root}）: $root，建议放在 C:/Code/smartcar"
fi

echo "== 工作区 =="
for r in esp32c6_car smartcar_remote tc275_car myCarSbl; do
    if [[ ! -e "$r/.git" ]]; then fail "$r 未初始化（just init）"; continue; fi
    n="$(git -C "$r" ls-files --eol | grep -c 'w/crlf' || true)"
    [[ "$n" == 0 ]] && pass "$r 工作区换行 LF" || warn "$r 有 $n 个 CRLF 文件（git -C $r add --renormalize . 后检查）"
done

exit $ok
