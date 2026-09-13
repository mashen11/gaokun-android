/* SPDX-License-Identifier: Apache-2.0 */
/*
 * libcamera 在 Soong 下的 config.h —— 替代 meson 自动生成的那一份。
 *
 * ★ HAVE_* 这几项照抄 meson 针对 **Android/bionic** 探测出的结果
 *   （NDK r27c 上 meson 生成的 build-android/config.h）：
 *   没有它们的话，`src/libcamera/base/memfd.cpp` 里给老 glibc 准备的兼容垫片
 *   会被编进来，和 bionic 自带的撞车 —— 实测报
 *   "F_ADD_SEALS macro redefined" 与 "call to 'memfd_create' is ambiguous"。
 *
 * ⚠️★ 但那几个【路径】必须改：meson 那份是 `--prefix=/data/local/tmp/lc`
 *   留下的、给命令行测试用的路径。装进 ROM 要用 vendor 下的真实位置，
 *   照抄会让 HAL 在设备上找不到 IPA 模块（而且是运行时才报，构建期看不出来）。
 */
#pragma once

#define HAVE_BACKTRACE 1
#define HAVE_CLOSE_RANGE 1
#define HAVE_FILE_SEALS 1
#define HAVE_LOCALE_T 1
#define HAVE_MEMFD_CREATE 1
#define HAVE_POSIX_IOCTL 1

/*
 * ⚠️ 故意【不】定义 HAVE_IPA_PUBKEY：
 *   定义它就要一并提供签名公钥与 ipa_pub_key.cpp 的真实内容；而没有它时
 *   `IPAManager::isSignatureValid()` 直接返回 false（ipa_manager.cpp:313），
 *   IPA 会走【独立进程】而不是进程内。两条路都能跑，但进程内那条要在
 *   Soong 里复刻 meson 的签名步骤 —— 留给之后做。
 *   ⬜ TODO：接上签名，让 IPA 回到进程内（少一个进程、少一次 IPC）。
 */

/* ── 设备上的真实路径（vendor 分区）── */
#define IPA_CONFIG_DIR       "/vendor/etc/libcamera/ipa"
#define IPA_MODULE_DIR       "/vendor/lib64/libcamera/ipa"
#define IPA_PROXY_DIR        "/vendor/lib64/libcamera/proxy"
#define LIBCAMERA_DATA_DIR   "/vendor/etc/libcamera"
#define LIBCAMERA_SYSCONF_DIR "/vendor/etc/libcamera"
