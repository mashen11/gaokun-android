#!/usr/bin/env bash
# 在构建机上把 libcamera 交叉编到 Android aarch64（M1：先证明能出图，再谈 HAL）。
#
#   bash scripts/camera/build-libcamera-android.sh        # 在构建机上执行
#
# ★ 为什么是【独立交叉编译】而不是直接写 Android.bp：
#   本仓传感器 M11 验证过的路径是"先做独立命令行客户端，那就是 HAL 逻辑的 90%"。
#   meson→bp 的转换（M3）是纯工程量，而"libcamera 在这台机器上到底能不能出图"
#   是真正的不确定性 —— 先解决不确定的那个。
#
# ⚠️ 踩过的坑（都写在这里，别再踩一次）：
#  1. meson 的 machine file **必须用单引号**。双引号在 meson 1.0.1 上报
#     `Malformed value in machine file variable 'c'`，而报错完全不提引号。
#  2. 需要 python 模块 **ply**（生成 IPA 接口代码用）。缺了报
#     `ERROR: Problem encountered: Python module 'ply' not found`，
#     在 configure 的最后一步才炸。Debian 上是 `python3-ply`。
#  3. 选项名不能猜：是 **`-Dipas=softisp`** 不是 `simple`
#     （`meson_options.txt:49-53` 的 choices 里没有 simple）。
#  4. ⚠️★ 没有 gnutls/libcrypto 时 `isSignatureValid()` **无条件返回 false**
#     （`src/libcamera/ipa_manager.cpp:313` 的 `#else return false`），
#     于是 **IPA 一律跑在独立进程里**（Isolated 而非 Threaded）。
#     configure 会打一行 WARNING 说明这件事。
#     ★ 这是 M1 的临时状态：M3 在 AOSP 里编时有 BoringSSL 的 libcrypto，
#     签名可用，IPA 就会回到进程内。
#  5. 到构建机的 ssh 会被掐（本仓多次记过）⇒ 长任务一律 `nohup` 后台跑 + 轮询日志，
#     不要指望一条 ssh 从头挂到尾。
#  6. 故意 **不编 `cam` 工具**：它依赖 libevent，又是一个要交叉编的东西。
#     用 `scripts/camera/lctest.cpp`（几百行，只打公开 API）代替。
set -euo pipefail

NDK_VER=${NDK_VER:-r27c}
API=${API:-34}
NDK=${NDK:-$HOME/android-ndk-$NDK_VER}
SRC=${SRC:-$HOME/libcamera}
BUILD=${BUILD:-$SRC/build-android}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64

[ -d "$NDK" ] || { echo "✗ 没有 NDK: $NDK"; exit 1; }
[ -d "$SRC" ] || { echo "✗ 没有 libcamera 源码: $SRC"; exit 1; }

CROSS=$HOME/cross-android-arm64.ini
cat > "$CROSS" <<INI
[binaries]
c          = '$TC/bin/aarch64-linux-android$API-clang'
cpp        = '$TC/bin/aarch64-linux-android$API-clang++'
ar         = '$TC/bin/llvm-ar'
strip      = '$TC/bin/llvm-strip'
ranlib     = '$TC/bin/llvm-ranlib'
pkg-config = 'false'

[host_machine]
system     = 'android'
cpu_family = 'aarch64'
cpu        = 'aarch64'
endian     = 'little'
INI

cd "$SRC"
rm -rf "$BUILD"
# prefix 用设备上的路径：libcamera 会把它编进去找 IPA 模块/配置文件。
meson setup "$BUILD" --cross-file "$CROSS" \
    --buildtype=release --prefix=/data/local/tmp/lc \
    -Dandroid=disabled -Dpipelines=simple -Dipas=softisp \
    -Dcam=disabled -Dqcam=disabled -Dgstreamer=disabled \
    -Dlc-compliance=disabled -Dpycamera=disabled -Dv4l2=disabled \
    -Dudev=disabled -Ddocumentation=disabled -Dtracing=disabled \
    -Dlibdw=disabled -Dlibunwind=disabled -Dsoftisp-gpu=disabled \
    -Dtest=false

ninja -C "$BUILD" -j"$(nproc)"
echo "✅ 编译完成，产物："
find "$BUILD" -name "libcamera*.so*" -o -name "*ipa_soft*" | head
