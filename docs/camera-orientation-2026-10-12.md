# 两颗摄像头 orientation 的实测标定（2026-10-12）

> 只记**实测与复现步骤**。设备树里的 `rotation` 这次**没有改**（见文末「与 0049 的冲突」）。

## 结论

| 相机 | 设备树 `rotation`（逆时针） | 直接用设备树 ⇒ SENSOR_ORIENTATION | 实测取景转正所需的 `rotation`（逆时针） | ⇒ SENSOR_ORIENTATION |
|---|---|---|---|---|
| 后摄 ov13b10（internal/0） | 180 | 180 | **270** | **90** |
| 前摄 hi846（internal/1） | 0 | 0 | **90** | **270** |

即：两颗都要在设备树值上 **+90（逆时针）**，等价于 SENSOR_ORIENTATION **−90**。

## 被测环境

- ROM：crDroid `lineage_gaokun3-bp4a`，构建 2026-10-06，Android 16（BP4A.251205.006）
- 内核：6.6.30-android15-8-g0a4d3c12b3（2026-10-11 编）
- SELinux：**Enforcing**
- 相机应用：Aperture versionCode 36 / versionName 16（targetSdk 36）
- 持机姿态：**横屏**，显示 `mOverrideDisplayInfo` rotation=1（2560x1600）

## 怎么测的（可复现）

```sh
# 1) 看 HAL 实际算出来的朝向（设备树原始值在括号里，属性覆盖不影响它）
setprop ctl.restart vendor.camera-provider-gaokun3
logcat | grep 朝向
#   后摄：就绪：阵列 4208x3120 … 朝向 90（libcamera rotation=180，逆时针）
#   前摄：就绪：阵列 3264x2448 … 朝向 270（libcamera rotation=0，逆时针）

# 2) 试角（属性给的数 = 想写进设备树的那个数，不用换算）
setprop debug.gaokun3.camera.orientation.rear  270
setprop debug.gaokun3.camera.orientation.front 90
setprop ctl.restart vendor.camera-provider-gaokun3
dumpsys media.camera | grep -E 'Facing|Orientation'   # Back 90 / Front 270

# 3) 目视：横屏下前后摄取景都要正；改完把属性清掉、把值写进设备树
```

★ 判据一律以**用户在真机上的目视**为准（camera.md §2.3 的纪律：助手读截图判朝向在本项目翻过 3 次车）。

## 与 patches/0049 的冲突（重要，别直接照抄这次的值改设备树）

- 0049（2026-09-24）：用户目视确认后摄 `rotation = <180>` 取景是正的，并明确写
  「PR #6 描述里的"前 90 / 后 270"与目视结论相反，别照它改」；前摄保持 `<0>` 待看。
- 本次（2026-10-12）：同一台机、同样的设备树值（HAL 日志显示后 180 / 前 0），
  目视结论**恰好就是 PR #6 那组**：后 270 / 前 90，取景才正。

两次都是目视，差 90°，可能来自：Aperture / CameraX 版本变了（现在 versionCode 36，targetSdk 36）、
显示旋转不同（本机面板竖装、横屏时 display rotation=90）、或 09-24 那次只有一个朝向被验证过。
**在两边对齐之前，设备树不动**；本机先用 `debug.gaokun3.camera.orientation.*` 顶着。

复验建议：改值之后**横屏和竖屏各看一次**——正确的 SENSOR_ORIENTATION 应当在两种姿态下取景都正；
只在一种姿态下正，说明测到的其实是显示旋转而不是安装角。

## 顺带记下的两个坑（同一台机，enforcing 下）

1. **enforcing 下 `processCaptureRequest` 被 SELinux 拒**：
   `avc: denied { use } … anon_inode:sync_file scontext=hal_camera_default tcontext=hal_graphics_composer_default tclass=fd`
   ⇒ binder `BR_FAILED_REPLY`（EPERM）⇒ `Unable to submit capture request … Broken pipe` ⇒ `ERROR_CAMERA_DEVICE(3)`，
   Aperture 反复弹 "Error while setting up the session"。
   `device/huawei/gaokun3/sepolicy/hal_camera_default.te` 里 2026-10-08（issue #23）已补
   `allow hal_camera_default hal_graphics_composer:fd use;` —— **构建早于这一天的 ROM 仍会中**
   （本机 10-06 的 ROM 就是）。用 `ksud sepolicy patch "allow …"` 可以在线验证。
2. **Aperture 界面不跟着转**：`dumpsys activity activities` 显示
   `requestedOrientation=SCREEN_ORIENTATION_NOSENSOR`。NOSENSOR 不看加速度计、只按 `user_rotation`，
   于是设备横着拿、界面仍按竖屏画（取景是对的，因为它走真实传感器方向）。
   本机改用声明 `fullSensor` 的 Aperture 构建后正常；`user_rotation` 也能临时改变它。
