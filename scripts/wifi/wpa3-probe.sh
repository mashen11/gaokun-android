#!/system/bin/sh
# WPA3 (SAE) 取证 —— 一次跑完，拿到判定"哪一层不支持"所需的全部证据。
#
# 用法（设备上，要 root）：
#   adb push scripts/wifi/wpa3-probe.sh /data/local/tmp/ && adb shell sh /data/local/tmp/wpa3-probe.sh
#   想顺带抓一次连接尝试：  sh /data/local/tmp/wpa3-probe.sh "<SSID>" "<密码>"
#
# ★ 为什么要这么多层：WPA3 能不能用取决于**四层同时成立**，而任何一层缺失的
#   表现都是"连不上"或"看不到这个网络"，从现象上分不开：
#     ① wpa_supplicant 编译时有没有 CONFIG_SAE      —— 已在构建机查过：有
#        （CONFIG_SAE=y / SAE_PK=y / OWE=y / DPP=y，二进制里 SAE-EXT-KEY、FT-SAE 都在）
#     ② 内核/驱动这一层：STA 模式的 SAE 由 wpa_supplicant 在用户态算、
#        经 mac80211 的 mgmt_tx 发认证帧。ath11k **没有**声明
#        NL80211_EXT_FEATURE_SAE_OFFLOAD，但那只是"固件不代劳"，不等于不支持。
#        真判据是 supplicant 自己报的 key_mgmt 里有没有 SAE。
#     ③ 框架这一层：WifiManager.isWpa3SaeSupported() —— 它来自 supplicant 的
#        getKeyMgmtCapabilities，不是 vendor HAL。
#     ④ AP 这一层：WPA3 的 H2E（hash-to-element）与 hunt-and-peck 是两种
#        SAE PWE 推导；`sae_pwe` 配错会只在**某些 AP** 上失败。
#        ⚠️ 本仓发的 wpa_supplicant.conf 里【没有设 sae_pwe】。
set -u
SSID="${1:-}"; PSK="${2:-}"
CTRL=/data/vendor/wifi/wpa/sockets
say() { echo; echo "######## $* ########"; }

say "0. 基本情况"
echo "内核: $(uname -r)  构建: $(getprop ro.build.date)"
echo "wlan0: $(ip link show wlan0 2>/dev/null | head -1)"
echo "supplicant: $(getprop init.svc.wpa_supplicant)  pid=$(pidof wpa_supplicant)"

say "1. ★ supplicant 自报的能力（最关键的一条）"
if command -v wpa_cli >/dev/null 2>&1; then
  for c in key_mgmt auth_alg proto pairwise group group_mgmt; do
    printf "  %-11s %s\n" "$c" "$(wpa_cli -p $CTRL -i wlan0 get_capability $c 2>/dev/null)"
  done
  echo "  ⇒ key_mgmt 里有 SAE = supplicant 这一层没问题"
else
  echo "  ⚠️ 设备上没有 wpa_cli —— 退回看 supplicant 自己的日志（第 4 节）"
fi

say "2. 框架这一层：isWpa3SaeSupported"
dumpsys wifi 2>/dev/null | grep -i -E "WPA3|SAE|supported.*feature" | head -12
echo "  --- cmd wifi status ---"
cmd wifi status 2>/dev/null | head -6

say "3. 扫到的网络里有没有 WPA3（RSN/SAE）"
cmd wifi start-scan >/dev/null 2>&1
sleep 4
cmd wifi list-scan-results 2>/dev/null | head -25
echo "  ⇒ 安全列里出现 SAE/WPA3 = AP 在广播 WPA3，且框架解析出来了"

say "4. supplicant 的原始日志（-dd，含 SAE 交互）"
logcat -d -s wpa_supplicant 2>/dev/null | tail -30

if [ -n "$SSID" ] && [ -n "$PSK" ]; then
  say "5. 实连一次 ${SSID}（抓失败点）"
  logcat -c 2>/dev/null
  cmd wifi connect-network "$SSID" wpa3 "$PSK" 2>&1 | head -5
  sleep 12
  echo "  --- 状态 ---"; cmd wifi status 2>/dev/null | head -4
  echo "  --- supplicant 日志 ---"
  logcat -d -s wpa_supplicant 2>/dev/null | tail -40
  echo "  --- 框架日志 ---"
  logcat -d 2>/dev/null | grep -i -E "sae|wpa3|ClientModeImpl|Supplicant" | tail -25
fi

say "6. 若第 1 节有 SAE 而仍连不上，下一步试 sae_pwe"
cat <<'HINT'
  WPA3-Personal 的 PWE 推导有两种：hunt-and-peck(0) 与 H2E(1)，2 = 两者都试。
  wpa_supplicant 默认只用 hunt-and-peck；而**要求 H2E 的 AP** 会直接拒。
  本仓 device/huawei/gaokun3/wifi/wpa_supplicant.conf 目前【没有这一行】。
  临时验证（不重启、不刷机）：
      wpa_cli -p /data/vendor/wifi/wpa/sockets -i wlan0 set sae_pwe 2
      wpa_cli -p /data/vendor/wifi/wpa/sockets -i wlan0 save_config
  能连上 ⇒ 把 `sae_pwe=2` 加进那个 conf，随下一版 ROM 固化。
HINT
