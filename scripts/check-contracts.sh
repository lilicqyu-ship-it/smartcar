#!/usr/bin/env bash
# check-contracts.sh - 校验各仓库中的共享接口副本与 contracts/ 逐字节一致。
#
# contracts/ 是唯一权威版本（canonical copy）；各仓库里保留原位副本，
# 便于各自构建系统直接编译，但禁止单方面修改——改接口的流程：
#   1. 在 meta 仓库改 contracts/
#   2. `just contracts-apply` 同步到各仓库
#   3. 各仓库分别提交 + push，CI 各自验证
#
# 用法:
#   scripts/check-contracts.sh          校验模式：不一致/缺失则退出码 1
#   scripts/check-contracts.sh --apply  同步模式：用 contracts/ 覆盖各仓库副本
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

APPLY=0
[[ "${1:-}" == "--apply" ]] && APPLY=1

# 声明表: contracts/内文件  各仓库副本路径...
# 各仓库采纳接口后，把它的路径追加到对应行即可纳入校验。
CHECKS=(
  "link/proto_frames.h  esp32c6_car/components/c6_proto/proto_frames.h smartcar_remote/main/proto/proto_frames.h"
  "link/proto_frames.c  esp32c6_car/components/c6_proto/proto_frames.c smartcar_remote/main/proto/proto_frames.c"
  # Camera WS 帧头（S3-CAM camera_ws TX ↔ S3 Remote scr_cam RX）；
  # gateway 侧副本随 camera_ws 组件采纳时追加到本行（Remote 设计文档 G-1/G-2）
  "camera/cam_frame.h   smartcar_remote/main/proto/cam_frame.h  s3-gateway/components/s3_proto/cam_frame.h"
  # Vision / cam_cmd 文本面 JSON schema（双端消息名与单位口径）
  "vision/vision.h      smartcar_remote/main/proto/vision.h  s3-gateway/components/s3_proto/vision.h"
  # SBL↔App：两侧必须一致，否则槽位/元数据/包头解析错位只能上板才发现
  "ota/ota_layout.h     tc275_sbl/mw/ota/ota_layout.h  tc275_car/mw/ota/ota_layout.h"
  "ota/ota_meta.h       tc275_sbl/mw/ota/ota_meta.h    tc275_car/mw/ota/ota_meta.h"
  "ota/tcfw_bundle.h    tc275_sbl/mw/ota/tcfw_bundle.h tc275_car/mw/ota/tcfw_bundle.h"
  "ota/ota_keys.h       tc275_sbl/mw/ota/ota_keys.h    tc275_car/mw/ota/ota_keys.h"
  # 验签实现：C6 / SBL / App 三方共用同一份 ed25519 + SHA-512
  "crypto/ed25519v.h    esp32c6_car/components/c6_ota/ed25519v.h  tc275_sbl/mw/crypto/ed25519v.h  tc275_car/mw/crypto/ed25519v.h"
  "crypto/ed25519v.c    esp32c6_car/components/c6_ota/ed25519v.c  tc275_sbl/mw/crypto/ed25519v.c  tc275_car/mw/crypto/ed25519v.c"
  "crypto/sha512.h      esp32c6_car/components/c6_ota/sha512.h    tc275_sbl/mw/crypto/sha512.h    tc275_car/mw/crypto/sha512.h"
  "crypto/sha512.c      esp32c6_car/components/c6_ota/sha512.c    tc275_sbl/mw/crypto/sha512.c    tc275_car/mw/crypto/sha512.c"
  "crypto/c6_consts.h   esp32c6_car/components/c6_ota/c6_consts.h tc275_sbl/mw/crypto/c6_consts.h tc275_car/mw/crypto/c6_consts.h"
)

fail=0
for entry in "${CHECKS[@]}"; do
  read -r canon rest <<<"$entry"
  canon="contracts/$canon"
  if [[ ! -f "$canon" ]]; then
    echo "FAIL $canon: contracts/ 内缺失（声明表指向不存在的文件）"
    fail=1
    continue
  fi
  for copy in $rest; do
    if [[ ! -f "$copy" ]]; then
      echo "FAIL $copy: 副本缺失（接口尚未被该仓库采纳）"
      fail=1
      continue
    fi
    if (( APPLY )); then
      if ! cmp -s "$canon" "$copy"; then
        cp "$canon" "$copy"
        echo "SYNC $copy <- $canon"
      fi
    else
      if cmp -s "$canon" "$copy"; then
        echo "OK   $copy"
      else
        echo "FAIL $copy 与 $canon 不一致"
        fail=1
      fi
    fi
  done
done

if (( fail )); then
  echo "== contracts 校验失败：请在各仓库修正副本，或运行 --apply 后分别提交 =="
else
  echo "== contracts 校验通过：所有接口副本一致 =="
fi
exit $fail
