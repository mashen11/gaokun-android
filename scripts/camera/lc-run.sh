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
# ⚠️★ LIBCAMERA_IPA_CONFIG_PATH 不能漏：软件 ISP 的 IPA 在 init 里【必须】
#   读到调优文件，读不到就 return 错误 → "IPA init failed" →
#   simple 流水线打一行 "disabling software debayering" 然后**照常出图**
#   —— 于是拿到的是原始拜耳而不是 YUV，而整个流程看起来是成功的。
#   ★ 这是本轮最像成功的失败：3 帧全收到、退出码 0。
#   路径布局是 <配置路径>/<ipa名>/<文件名>（`ipa_proxy.cpp:57`），
#   所以文件要放在 .../ipaconf/softisp/uncalibrated.yaml。
# ⚠️★ 输出必须落到设备上的文件再 cat 回来，【不能】直接让 adb shell 打印：
#   软件 ISP 的 IPA 跑在独立进程（softisp_ipa_proxy）里，它会**比 lctest 活得久**
#   并继承 stdout ⇒ 直接打印时 `adb shell` 永远不返回，看起来像程序挂死，
#   其实程序早就正常退出了。实测踩过一次，卡了 5 分钟。
S "LD_LIBRARY_PATH=$LC/lib \
   LIBCAMERA_IPA_MODULE_PATH=$LC/ipa \
   LIBCAMERA_IPA_PROXY_PATH=$LC/proxy \
   LIBCAMERA_IPA_CONFIG_PATH=$LC/ipaconf \
   LIBCAMERA_LOG_LEVELS=${LCLOG:-*:INFO} \
   $LC/bin/lctest $* > $LC/last.log 2>&1; echo LCTEST_RC=\$?"  2>&1 | tr -d '\r'
S "cat $LC/last.log" 2>&1 | tr -d '\r'
# 收尾：孤儿 proxy 不杀掉会一直占着，下次再跑会堆叠
S "pkill -f softisp_ipa_proxy 2>/dev/null; true" >/dev/null 2>&1
