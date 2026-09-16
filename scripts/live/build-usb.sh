#!/usr/bin/env bash
# 把 Stage 7 的产物组装成一个可启动的 U 盘镜像（GPT + 单个 ESP）。
#
#   bash scripts/live/build-usb.sh \
#        --squashfs /tmp/gk3/gaokun3-live.squashfs \
#        --initramfs /tmp/gk3/initramfs.img \
#        --kernel  /path/to/vmlinuz.efi \
#        --dtb     /path/to/gaokun3.dtb \
#        --sdboot  /path/to/systemd-bootaa64.efi \
#        [--payload /path/to/release-dir] \
#        --out /tmp/gk3/gaokun3-live.img
#
# ★ 用 mtools 往 FAT 里塞文件，【不需要 root】，也不需要 loop 设备。
#   好处不只是省事：不用 root 就不会因为一次手滑把宿主机的分区写了。
#
# 写盘： sudo dd if=gaokun3-live.img of=/dev/sdX bs=4M conv=fsync status=progress
set -euo pipefail

SQUASH=; INITRAMFS=; KERNEL=; DTB=; SDBOOT=; PAYLOAD=; OUT=; SIZE_MIB=; WIFI=
die() { echo "!! $*" >&2; exit 1; }
say() { echo; echo "══ $*"; }
ok()  { echo "   ✓ $*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --squashfs)  SQUASH=$2; shift 2 ;;
        --initramfs) INITRAMFS=$2; shift 2 ;;
        --kernel)    KERNEL=$2; shift 2 ;;
        --dtb)       DTB=$2; shift 2 ;;
        --sdboot)    SDBOOT=$2; shift 2 ;;
        --payload)   PAYLOAD=$2; shift 2 ;;
        --wifi-conf) WIFI=$2; shift 2 ;;
        --size)      SIZE_MIB=$2; shift 2 ;;
        --out)       OUT=$2; shift 2 ;;
        *) die "不认识的参数：$1" ;;
    esac
done
for v in SQUASH INITRAMFS KERNEL DTB SDBOOT OUT; do
    eval "x=\${$v}"
    [ -n "$x" ] || die "缺参数 --$(echo "$v" | tr 'A-Z' 'a-z')"
done
for f in "$SQUASH" "$INITRAMFS" "$KERNEL" "$DTB" "$SDBOOT"; do
    [ -f "$f" ] || die "文件不在：$f"
done
head -c 2 "$SDBOOT" | grep -q MZ || die "$SDBOOT 不是 PE 文件"
head -c 2 "$KERNEL" | grep -q MZ || die "$KERNEL 不是 PE 文件（要 EFI stub 内核 / vmlinuz.efi）"
[ "$(od -An -tx1 -N4 "$DTB" | tr -d ' ')" = "d00dfeed" ] || die "$DTB 不是 FDT（magic 不对）"
ok "输入体检通过（PE / PE / FDT）"

for t in sgdisk mformat mmd mcopy; do
    command -v "$t" >/dev/null || die "缺工具：${t}（apt install gdisk mtools）"
done

# —— 算大小 ——
need=0
for f in "$SQUASH" "$INITRAMFS" "$KERNEL" "$DTB" "$SDBOOT"; do
    need=$(( need + $(stat -c %s "$f") ))
done
if [ -n "$PAYLOAD" ]; then
    [ -d "$PAYLOAD" ] || die "--payload 不是目录：$PAYLOAD"
    need=$(( need + $(du -sb "$PAYLOAD" | cut -f1) ))
fi
# 25% 余量 + 64 MiB 底
MIN=$(( need / 1048576 * 125 / 100 + 64 ))
SIZE_MIB=${SIZE_MIB:-$MIN}
[ "$SIZE_MIB" -ge "$MIN" ] || die "--size $SIZE_MIB MiB 不够，至少要 $MIN MiB"
say "镜像 ${SIZE_MIB} MiB（内容 $(( need / 1048576 )) MiB）"

PART_OFF=1048576   # 1 MiB 对齐
rm -f "$OUT"
truncate -s "${SIZE_MIB}M" "$OUT"

# —— GPT + ESP ——
# ⚠️ 分区名用 "esp"：本仓的 boot_control HAL 靠 by-name/esp 找 ESP
#    （install-gaokun3.sh 的注释里记过原因）。U 盘上虽然用不到，
#    但保持一致，免得两处规则不一样。
sgdisk --zap-all "$OUT" >/dev/null
sgdisk -n 1:2048:0 -t 1:ef00 -c 1:"esp" "$OUT" >/dev/null
ok "GPT + ESP 分区"

mformat -i "$OUT@@$PART_OFF" -F -v GK3LIVE ::
ok "FAT32（卷标 GK3LIVE）"

M() { mcopy -i "$OUT@@$PART_OFF" -o "$@"; }
mmd -i "$OUT@@$PART_OFF" ::/EFI ::/EFI/BOOT ::/loader ::/loader/entries ::/gaokun3

M "$SDBOOT"    ::/EFI/BOOT/BOOTAA64.EFI
M "$KERNEL"    ::/gaokun3/Image
M "$DTB"       ::/gaokun3/gaokun3.dtb
M "$INITRAMFS" ::/gaokun3/initramfs.img
M "$SQUASH"    ::/gaokun3/rescue.squashfs
ok "引导链 + 内核 + initramfs + squashfs"

if [ -n "$PAYLOAD" ]; then
    mmd -i "$OUT@@$PART_OFF" ::/gaokun3/payload
    for f in "$PAYLOAD"/*; do
        [ -f "$f" ] || continue
        M "$f" "::/gaokun3/payload/$(basename "$f")"
    done
    ok "带上了安装载荷（$(ls -1 "$PAYLOAD" | wc -l) 个文件）"
fi

# WiFi 凭据（可选）。放在【介质】上而不是镜像里 —— 公开发布的 LiveCD
# 一个字都不带，而自用的这根 U 盘带上就能开机自动联网、方便远程调试。
# ⚠️ 本仓不收这个文件。
if [ -n "$WIFI" ]; then
    [ -f "$WIFI" ] || die "--wifi-conf 指的文件不在：$WIFI"
    grep -q 'network=' "$WIFI" || die "$WIFI 不像 wpa_supplicant 配置"
    M "$WIFI" ::/gaokun3/wpa_supplicant.conf
    ok "带上了 WiFi 配置（$(grep -c 'network=' "$WIFI") 个网络）"
fi

TMP=$(mktemp)
cat > "$TMP" <<'EOF'
# ⚠️ 不设 default：U 盘只有一个条目，systemd-boot 会直接进。
#    也【不要】设 timeout 0 —— 万一起不来，用户连菜单都进不去。
timeout 5
console-mode keep
editor no
EOF
M "$TMP" ::/loader/loader.conf

cat > "$TMP" <<'EOF'
title      gaokun3 安装器 / 救援系统
version    live
linux      /gaokun3/Image
devicetree /gaokun3/gaokun3.dtb
initrd     /gaokun3/initramfs.img
options    console=tty0 clk_ignore_unused pd_ignore_unused arm64.nopauth iommu.passthrough=0 iommu.strict=0 efi=noruntime fbcon=rotate:1 usbhid.quirks=0x12d1:0x10b8:0x20000000 loglevel=4 gk3.squash=/gaokun3/rescue.squashfs
EOF
M "$TMP" ::/loader/entries/gaokun3-live.conf
rm -f "$TMP"
ok "启动项"

say "体检"
LIST=$(mdir -i "$OUT@@$PART_OFF" -b ::/gaokun3 ::/EFI/BOOT ::/loader/entries 2>/dev/null)
for want in Image gaokun3.dtb initramfs.img rescue.squashfs BOOTAA64.EFI gaokun3-live.conf; do
    printf '%s' "$LIST" | grep -qi "$want" && ok "$want" || die "镜像里没有 $want"
done
echo
echo "$OUT  $(du -h "$OUT" | cut -f1)"
echo "写盘： sudo dd if=$OUT of=/dev/sdX bs=4M conv=fsync status=progress"
