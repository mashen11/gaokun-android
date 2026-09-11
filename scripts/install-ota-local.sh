#!/usr/bin/env bash
# 用本机上的 payload.bin 给 gaokun3 装 OTA（update_engine 的 file:// 通路）。
#
#   bash scripts/install-ota-local.sh --check     # 只检查前提，不动机器
#   bash scripts/install-ota-local.sh --go        # 真装
#
# ⚠️★★ **装机前必须确认现场有人能按电源键** —— 新槽从没启动过这版系统，
#   万一硬挂死（不 panic）没有远程办法救。本仓为此付过两次账（#79 / #83）。
#
# ★ 为什么要有这个脚本：装机路上有三颗地雷，每一颗都"看着毫无关系"：
#  1. `update_engine` **拒绝在 overlayfs 生效时工作**（`kOverlayfsenabledError(64)`）。
#     要先 `adb enable-verity` **并重启**。⚠️ 它会报
#     "boot_b does not look like a vbmeta footer"，无害。
#     ⚠️ 这一步会连带抹掉用 `adb remount` 推上去的东西（比如临时的 audio-route.sh）
#     —— 那正是本次装 ROM 要接手的内容，所以是预期行为。
#  2. ★★ `update_engine` 把新槽标成 active 之后，**boot_control HAL 会立刻把
#     ESP 的 `default` 改成新槽的条目**（M20 实测）。新槽起不来就连回落都没有了。
#     **重启前必须把 `default` 掰回已知可用的那个槽，只用 oneshot 过去。**
#  3. 这个版本的 `update_engine_client` **没有 `--status`**（M17 实测）。
#     判断装完看 `bootctl get-active-boot-slot` 是否切槽。
set -uo pipefail
SER=${SER:-gaokun3}
MODE=${1:---check}
A() { adb -s "$SER" "$@"; }
S() { adb -s "$SER" shell "$@"; }
die() { echo "✗ $*" >&2; exit 1; }
ok()  { echo "✓ $*"; }

echo "═══ 1. 前提检查 ═══"
S true >/dev/null 2>&1 || die "adb 连不上 $SER"
CUR=$(S getprop ro.boot.slot_suffix | tr -d '\r')
ok "当前槽 $CUR · 内核 $(S 'cat /proc/version' | grep -o '#[0-9]*' | tr -d '\r') · 构建 $(S getprop ro.build.date.utc | tr -d '\r')"

[ "$(S id -u | tr -d '\r')" = "0" ] || die "需要 root（先 setprop service.adb.root 1 && adb root）"

for f in /data/local/tmp/payload.bin /data/local/tmp/payload_properties.txt; do
    S "[ -f $f ]" || die "设备上缺 $f —— 先从 OTA zip 里解出来 push 过去"
done
ok "payload 与 properties 都在设备上"

# ⚠️ overlayfs 必须是关的
if S 'mount' | grep -q "overlay on /vendor"; then
    echo "⚠️ overlayfs 还生效着 —— update_engine 会直接拒绝（错误码 64）"
    echo "   要跑：adb enable-verity && adb reboot，然后重新执行本脚本"
    [ "$MODE" = "--go" ] && die "先处理 overlayfs"
else
    ok "overlayfs 未生效"
fi

DEF=$(S 'mkdir -p /mnt/esp; mount -t vfat /dev/block/by-name/esp /mnt/esp 2>/dev/null; grep ^default /mnt/esp/loader/loader.conf' | tr -d '\r')
ok "ESP 的 $DEF"

if [ "$MODE" != "--go" ]; then
    echo; echo "（--check 模式，没有改动任何东西。确认现场有人能按电源键后用 --go）"
    S 'umount /mnt/esp 2>/dev/null'
    exit 0
fi

echo "═══ 2. 下发更新 ═══"
HDRS=$(S 'cat /data/local/tmp/payload_properties.txt' | tr -d '\r' | tr '\n' '|' | sed 's/|$//')
S "update_engine_client --payload=file:///data/local/tmp/payload.bin --update --headers=\"\$(cat /data/local/tmp/payload_properties.txt)\"" 2>&1 | tail -5

echo "═══ 3. 等装完（看 active slot 是否切换；这个版本没有 --status）═══"
for i in $(seq 1 120); do
    NEW=$(S bootctl get-active-boot-slot | tr -d '\r')
    CURN=$([ "$CUR" = "_a" ] && echo 0 || echo 1)
    [ "$NEW" != "$CURN" ] && { ok "active slot 已切到 $NEW"; break; }
    sleep 10
done

echo "═══ 4. ⚠️ 把 default 掰回已知可用的槽，只用 oneshot 过去 ═══"
echo "   （boot_control HAL 刚把它改成新槽了 —— 这一步是安全网）"
S "sed -i 's|^default .*|default *-android${CUR}.conf|' /mnt/esp/loader/loader.conf; sync; grep ^default /mnt/esp/loader/loader.conf" 2>&1 | tr -d '\r'
echo
echo "⬜ 剩下的手工两步（故意不自动做）："
echo "   1) 写 LoaderEntryOneShot 指向新槽的条目"
echo "   2) adb reboot，然后验收：uname / getprop ro.build.date.utc / tinymix 看 PA"
S 'sync; umount /mnt/esp 2>/dev/null'
