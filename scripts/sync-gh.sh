#!/usr/bin/env bash
# sync-gh.sh - 用 gh CLI 把统一的仓库配置批量下发到 4 个子仓库。
#
# 前提: gh 已登录（gh auth status），token 具备对应权限：
#   labels     -> Issues: write          protection -> Administration: write
#   board      -> 项目读写权限
# 权限不足时对应项会报 403 并跳过，不影响其他项。
#
# 用法: sync-gh.sh [labels|protection|board|all]   默认 all
set -uo pipefail

OWNER="lilicqyu-ship-it"
REPOS=(esp32c6_car smartcar_remote tc275_car myCarSbl)
PROJECT_TITLE="Smartcar"
MAIN_BRANCH="main"

# 标准标签集: name|color|description （跨仓库统一，看板/筛选依赖这些名字）
LABELS=(
  "bug|d73a4a|Bug"
  "enhancement|a2eeef|新功能"
  "documentation|0075ca|文档"
  "ci|1d76db|CI/构建门禁"
  "proto|5319e7|LINK 协议/共享接口"
  "ota|fbca04|OTA/升级"
  "hardware|c5def5|硬件/结构/PCB"
)

MODE="${1:-all}"

say() { echo "== $* =="; }

sync_labels() {
  say "labels"
  for repo in "${REPOS[@]}"; do
    for spec in "${LABELS[@]}"; do
      IFS='|' read -r name color desc <<<"$spec"
      if gh label create "$name" -R "$OWNER/$repo" --color "$color" --description "$desc" --force >/dev/null 2>&1; then
        echo "  ok   $OWNER/$repo  $name"
      else
        echo "  FAIL $OWNER/$repo  $name（需要 Issues:write 权限?）"
      fi
    done
  done
}

sync_protection() {
  say "branch protection ($MAIN_BRANCH: 禁 force-push/删除, 不强制 PR)"
  for repo in "${REPOS[@]}"; do
    if gh api -X PUT "repos/$OWNER/$repo/branches/$MAIN_BRANCH/protection" \
         --input - >/dev/null 2>&1 <<'JSON'
{"required_status_checks":null,"enforce_admins":false,"required_pull_request_reviews":null,"restrictions":null,"allow_force_pushes":false,"allow_deletions":false,"required_linear_history":false}
JSON
    then
      echo "  ok   $OWNER/$repo"
    else
      echo "  FAIL $OWNER/$repo（需要 Administration:write 权限?）"
    fi
  done
}

# 找到标题匹配的 ProjectV2 id；不存在则创建。"created" 提示走 stderr。
project_id() {
  local id owner
  id=$(gh api graphql -f query='query{viewer{projectsV2(first:50){nodes{id title}}}}' \
        --jq ".data.viewer.projectsV2.nodes[] | select(.title==\"$PROJECT_TITLE\") | .id" 2>/dev/null)
  if [[ -z "$id" ]]; then
    owner=$(gh api graphql -f query='query{viewer{id}}' --jq .data.viewer.id)
    id=$(gh api graphql -f query='mutation($owner:ID!,$title:String!){createProjectV2(input:{ownerId:$owner,title:$title}){projectV2{id}}}' \
          -f owner="$owner" -f title="$PROJECT_TITLE" --jq .data.createProjectV2.projectV2.id 2>/dev/null)
    [[ -n "$id" ]] && echo "  created project: $PROJECT_TITLE" >&2
  fi
  echo "$id"
}

sync_board() {
  say "看板: $PROJECT_TITLE"
  local pid
  pid=$(project_id)
  if [[ -z "$pid" ]]; then
    echo "  FAIL 无法获取/创建 ProjectV2（权限?）"
    return 1
  fi

  # 已在板上的 issue（按 url 去重）
  local -A existing=()
  while IFS= read -r u; do [[ -n "$u" ]] && existing["$u"]=1; done < <(
    gh api graphql -f query="query(\$id:ID!){node(id:\$id){... on ProjectV2{items(first:100){nodes{content{__typename ... on Issue{url}}}}}}}" \
      -f id="$pid" --jq '.data.node.items.nodes[].content.url? // empty' 2>/dev/null
  )

  for repo in "${REPOS[@]}"; do
    # contentId 必须是 GraphQL node id（.id），不是 issue 编号
    while IFS=$'\t' read -r url num nid; do
      [[ -z "$url" ]] && continue
      if [[ -n "${existing[$url]:-}" ]]; then
        echo "  skip $repo#$num（已在板上）"
        continue
      fi
      if gh api graphql -f query="mutation(\$p:ID!,\$c:ID!){addItemProjectV2(input:{projectId:\$p,contentId:\$c}){item{id}}}" \
           -f p="$pid" -f c="$nid" >/dev/null 2>&1; then
        echo "  add  $repo#$num"
      else
        echo "  FAIL $repo#$num"
      fi
    done < <(gh issue list -R "$OWNER/$repo" --state open --limit 200 --json number,url,id \
               --jq '.[] | "\(.url)\t\(.number)\t\(.id)"' 2>/dev/null)
  done
}

case "$MODE" in
  labels)     sync_labels ;;
  protection) sync_protection ;;
  board)      sync_board ;;
  all)        sync_labels; sync_protection; sync_board ;;
  *) echo "用法: $0 [labels|protection|board|all]"; exit 2 ;;
esac
