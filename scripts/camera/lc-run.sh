#!/usr/bin/env bash
# 在设备上跑 libcamera 最小验证客户端（M1 的上机那一半）。
#
#   SER=<adb序列号> bash scripts/camera/lc-run.sh [lctest 的参数...]
#
# ⚠️★ 前提一：必须启动到【带相机 DTB 的内核】。发布用的 gaokun3.dtb 关着 camss
#   （实测 /sys/.../ac5a000.camss/power/runtime_status = "unsupported"、
#   genpd 里没有 titan_top_gdsc、/dev/video0-1 是 Venus 而不是相机）。
#   走 ESP 上的 `…-cam2.conf` 条目（内核 #5 + slot_cam/gaokun3-no-rear.dtb + slot _a）。
#
# ⚠️★★ 前提二：开机后【第一次用相机之前】就要钉住 camss 的 runtime PM，
#   否则一旦 titan_top_gdsc 塌缩过就再也上不了电（#83/#87，根因未破）。
#   本脚本会自动做这件事，并检查是不是已经晚了。
set -uo pipefail
SER=${SER:?请设置 SER=<adb序列号>}
A() { adb -s "$SER" "$@"; }
S() { adb -s "$SER" shell "$@"; }
LC=/data/local/tmp/lc

[ "$(S id -u | tr -d '\r')" = "0" ] || { echo "✗ 需要 root"; exit 1; }

echo "═══ 1. 前提检查 ═══"
K=$(S 'cat /proc/version' | grep -oE '#[0-9]+' | tr -d '\r')
ST=$(S 'cat /sys/devices/platform/soc@0/ac5a000.camss/power/runtime_status 2>/dev/null' | tr -d '\r')
echo "  内核 $K · camss runtime_status = ${ST:-（读不到）}"
case "$ST" in
    unsupported|"") echo "✗ camss 没有 probe —— 你不在相机 DTB 内核上。先 oneshot 到 …-cam2.conf"; exit 2 ;;
    error)          echo "✗ camss 已经 runtime_error 锁死 —— GDSC 塌缩过了，必须重启"; exit 3 ;;
esac
TT=$(S 'grep -E "^titan_top_gdsc" /sys/kernel/debug/pm_genpd/pm_genpd_summary' | awk '{print $2}' | tr -d '\r')
echo "  titan_top_gdsc = $TT"

echo "═══ 2. 钉住 camss（必须在第一次塌缩之前）═══"
S 'echo on > /sys/devices/platform/soc@0/ac5a000.camss/power/control'
echo "  control = $(S 'cat /sys/devices/platform/soc@0/ac5a000.camss/power/control' | tr -d '\r')"

echo "═══ 3. 跑 lctest ═══"
# 环境变量名都是从 refs/libcamera 源码里 grep 出来的，不是记忆：
#   LIBCAMERA_IPA_MODULE_PATH / LIBCAMERA_IPA_PROXY_PATH / LIBCAMERA_LOG_LEVELS
S "LD_LIBRARY_PATH=$LC/lib \
   LIBCAMERA_IPA_MODULE_PATH=$LC/ipa \
   LIBCAMERA_IPA_PROXY_PATH=$LC/proxy \
   LIBCAMERA_LOG_LEVELS=${LCLOG:-*:INFO} \
   $LC/bin/lctest $*" 2>&1 | tr -d '\r'
