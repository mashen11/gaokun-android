#!/bin/bash
# 交叉编译两个相机诊断工具为【静态】aarch64 二进制，可直接 adb push 到 Android 上跑。
#
# 用法：bash scripts/camera/build.sh <内核树路径>
#   内核树用来 `make headers_install`，好让 <linux/media.h> / <linux/videodev2.h> /
#   <linux/v4l2-subdev.h> 来自【正在用的那个内核】—— 结构体不手抄（M13 的教训）。
#
# 产物：scripts/camera/out/{mediatopo,camtest}
set -euo pipefail
TREE=${1:?用法: $0 <内核树路径>}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HDR=$HERE/out/khdr
mkdir -p "$HERE/out"
make -C "$TREE" ARCH=arm64 INSTALL_HDR_PATH="$HDR" headers_install >/dev/null
for t in mediatopo camtest; do
    aarch64-linux-gnu-gcc -static -O2 -Wall -I"$HDR/include" -o "$HERE/out/$t" "$HERE/$t.c"
    echo "✓ $HERE/out/$t  ($(stat -c%s "$HERE/out/$t") 字节，静态)"
done
