#!/bin/sh
# 推流"稳定不卡顿"的构建期不变量：逐条比对生成的 sdkconfig。
# 这些值散在 sdkconfig.defaults 里，看起来像"随手可调"的网络参数，但每一条都有
# 真机依据（算术见 doc/19-camera.md §3.1、绑核依据见 doc/20-core-assignment.md）。
# CI（s3-gateway.yml）与 Release（s3-gateway-release.yml）都调它；发版前本地想自查
# 也可以直接跑：
#
#   sh tools/check_streaming_budget.sh [sdkconfig 路径]
#
# 要重新调优：先改 doc/19 的算术与结论，再改这张表——别只改一处。
set -eu

SDKCONFIG=${1:-sdkconfig}
if [ ! -f "$SDKCONFIG" ]; then
    echo "$SDKCONFIG 不存在：先 idf.py build（或 reconfigure）生成它" >&2
    exit 1
fi

fail=0
for kv in \
    CONFIG_IDF_TARGET=\"esp32s3\" \
    CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y \
    CONFIG_SPIRAM=y \
    CONFIG_SPIRAM_MODE_OCT=y \
    CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y \
    CONFIG_LWIP_TCP_SND_BUF_DEFAULT=14400 \
    CONFIG_LWIP_TCP_WND_DEFAULT=14400 \
    CONFIG_HTTPD_WS_SUPPORT=y \
    CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y \
    CONFIG_CAMERA_CORE1=y \
    CONFIG_OV5640_SUPPORT=y
do
    if ! grep -qxF "$kv" "$SDKCONFIG"; then
        echo "缺 $kv" >&2
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "推流预算/核分工不变量被改动（见本脚本头部说明）" >&2
    exit 1
fi
echo "推流预算不变量 OK（$SDKCONFIG 11 项全中）"
