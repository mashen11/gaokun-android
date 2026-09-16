#!/usr/bin/env bash
# 在局域网里找回设备。
#
#   bash scripts/find-device.sh          # 找 Android（adb 5555）
#   bash scripts/find-device.sh --ssh    # 找救援系统（sshd 22）
#
# ⚠️★ 这个脚本被改过两次，两次都是因为【判据认错了对象】：
#
#   第一版：MAC 找不到时退化成"扫谁的端口开着就返回谁" —— 局域网里另有一台
#           开着 sshd 的机器，于是我照着【陌生人的机器】查了两轮
#           "公钥为什么被拒"。
#   第二版：只按 MAC 匹配 —— 而 Android 有【按 SSID 随机化 MAC】，
#           换个频段 MAC 就变了（实测 00:03:7f:12:4b:19 → …:de:1d），
#           于是又找不到了。
#
# ★ 现在的判据是【按协议确认身份】，不是看端口开不开、也不是看 MAC：
#     5555 → adb 连上去问 ro.crdroid.device 是不是 gaokun3
#     22   → ssh 上去问 hostname 是不是 gaokun3-rescue / gaokun3-live
#   端口和 MAC 只用来【缩小候选范围】，确认永远靠协议。
#
# ⚠️ 必须绕开沙箱跑（sandbox 代理会把所有 TCP 连接都答应下来，
#    于是端口扫描每个 IP 都"开着"，完全没有信息量）。
set -u
MODE=android; [ "${1:-}" = "--ssh" ] && MODE=rescue
KEY=${GAOKUN3_KEY:-$HOME/.ssh/ed25519}

SELF=$(ipconfig 2>/dev/null | grep -oE "192\.168\.[0-9]+\.[0-9]+" | head -1)
NET=$(echo "$SELF" | cut -d. -f1-3)
[ -n "$NET" ] || { echo "!! 认不出本机网段" >&2; exit 1; }

# 广播 ping 填 ARP（顺带把刚换过 IP 的设备勾出来）
for i in $(seq 1 254); do (ping -n 1 -w 150 "$NET.$i" >/dev/null 2>&1 &); done
sleep 5

CANDS=$(arp -a 2>/dev/null | grep -oE "$NET\.[0-9]+" | sort -u | grep -v "^$SELF$" | grep -v "\.255$")
[ -n "$CANDS" ] || { echo "!! ARP 表是空的 —— 网络有问题？" >&2; exit 1; }

for ip in $CANDS; do
    if [ "$MODE" = android ]; then
        timeout 1 bash -c "echo > /dev/tcp/$ip/5555" 2>/dev/null || continue
        timeout 8 adb connect "$ip:5555" >/dev/null 2>&1
        d=$(timeout 8 adb -s "$ip:5555" shell getprop ro.crdroid.device 2>/dev/null | tr -d '\r\n')
        if [ "$d" = gaokun3 ]; then echo "$ip"; exit 0; fi
        timeout 5 adb disconnect "$ip:5555" >/dev/null 2>&1
    else
        timeout 1 bash -c "echo > /dev/tcp/$ip/22" 2>/dev/null || continue
        h=$(timeout 10 ssh -o BatchMode=yes -o StrictHostKeyChecking=no \
              -o UserKnownHostsFile=/dev/null -o ConnectTimeout=6 -i "$KEY" \
              "root@$ip" hostname 2>/dev/null | tr -d '\r\n')
        # ⚠️ 登录失败也是有用的信息：说明那台机器【不是】我们的救援系统，
        #    或者是我们的但公钥没生效 —— 两者要区分，所以把主机名打到 stderr。
        [ -n "$h" ] && echo "  $ip 的 hostname = $h" >&2
        # rescue profile 叫 gaokun3-rescue，live profile 叫 gaokun3-live
        case "$h" in gaokun3-rescue|gaokun3-live) echo "$ip"; exit 0 ;; esac
    fi
done
echo "!! 扫完 $(echo "$CANDS" | wc -w) 个邻居，没有一台确认是我们的设备（${MODE}）" >&2
exit 1
