# 待办清单

最后更新：2026-09-11（可重建性修好 + 新内核上机通过；音量还差一版 ROM）

这份清单的排序原则是**用户能不能感觉到**，而不是有趣程度。每条都尽量写出
**具体的第一步** —— 没有第一步的条目只是愿望，不是待办。

状态表与公开招募项在 [`../README.md`](../README.md)；每条的证据在
[`stage4-findings.md`](stage4-findings.md) 等案卷里。

---

## A. 用户能感觉到的缺口

### A9. ✅ 内核可重建性已修好并上机验证通过

**起因**：音量补丁内核起不来，查明是照本仓配方重建的内核缺两份**从未入库的
树外源码**（[#79](stage4-findings.md)）。**2026-09-11 全部做完**
（[#80](stage4-findings.md)）：

* `patches/0016`（ashmem，底本 ACK `android15-6.6` —— ★ 最后一个还带它的分支）
  与 `patches/0017`（xt_quota2，底本 `android16-6.12`）已入库并进 KPATCHES。
  内容取自构建机旧树 `~/gaokun/mainline-linux`，**是当年实测过的那一份**。
  6 处 API 漂移逐条写在补丁头里。
* ⚠️★★ 顺带查出 **`kernel-apply-patches.sh` 的指纹判据有假阳性**，
  已让 `patches/0009`（CPU 温控降频）静默漏打 —— **正是 M17 写这个脚本要防的
  那一个补丁**。判据已加固（挑最长的 3 条新增行、要求全部命中）。
* ✅ **上机一次成功**：新内核 `#3` 重启 37 秒回来、`boot_completed` 在 uptime 34 s；
  `/dev/ashmem` 在、`/proc/net/xt_quota/globalAlert` 在（netd 已在用）、
  8 个 CPU 温区全部 `trips=2 / cdev=1`、pstore 零记录；
  WiFi / 蓝牙 / 传感器 / GPU / Venus / root / 声卡全部正常。
* ✅ 新内核已**提升为常驻**（覆盖 `slot_b/Image`），测试脚手架已清，
  ESP 仍是 4 个条目、84% / 50 MB 可用。

★ **下次测内核照抄这套安全阀**（这次代价为零、没用上，但值得常备）：
条目里加 `androidboot.init_fatal_panic=true loglevel=7 panic=10` ——
`panic=10` 让 init 炸掉后**自动重启回 `default` 自愈**，不用按电源键；
`init_fatal_panic` 把 init 的 LOG(FATAL) 转成真 panic 落 pstore。

★ **重建验收判据（进发版收尾清单，两条都要）**：
① 设备 `/proc/config.gz` 与重建树 `.config` 做 diff —— 差出的每个符号都是一份
没入库的源码；② 两个内核镜像做**全字符串差集**，**外加 dtb 比 sha256**。
⚠️ 字符串差集**看不见 DTS**，0009 漏打就完全逃过了它。

### A0. ⚠️ 侧滑返回手势失效（用户报告，**假说未证实**）
用户在装了 v0.4.0-alpha 之前的那一版（桌面模式开着）时报告边缘侧滑返回不工作。

**已知**：`navigation_mode = 2`（手势导航）本身是对的；当时 `dumpsys window`
显示所有应用都在 **freeform 窗口**里（`mWindowingMode=freeform`、`Task name=Desk`），
而桌面窗口模式下边缘返回的处理方式本来就不同。

⚠️ **这只是嫌疑，没有证实** —— 当时没有运行时开关能把桌面模式关掉再对照
（那个开发者选项开关只能强制打开，见 A′/#桌面模式）。v0.4.0-alpha 已把桌面
模式关闭，**所以第一步就是让用户在新版上再试一次**：
* 好了 ⇒ 就是桌面模式，本条关闭；
* 还是不行 ⇒ 与桌面模式无关，查 `back_gesture_inset_scale_left/right`
  （现在是 null）、`dumpsys window | grep -i gesture` 的排除区域，
  以及触摸驱动在屏幕边缘的上报（#26 那套 evdev 录制方法可复用）。

### A1. 音频与蓝牙长期运行后死锁 ⚠️ 次高优先
用户实机报告，我未复现、未定位（[#38](stage4-findings.md)）。
两者共用同一条到 DSP 的 QRTR/FastRPC 通路，而这条通路上**已经实测到过**
会话级卡死（使能光感会污染整个 SSC 会话）。

★ **取证看门狗已随 v0.2.0 起的镜像发布**（`bin/gaokun3-hangdump.sh`）：
现实是死锁时用户只会重启、证据就没了，所以证据必须自动留下。它 60 秒采一次
`/proc` 线程状态（刻意不跑 dumpsys，很便宜），判据是**同一个 tid 连续三次都在 D**
（≥2 分钟），命中后把 stack/wchan/QRTR 服务表/PCM 状态/binder 日志/logcat
写到 `/data/vendor/gaokun3/hangdump-<uptime>/`。

**第一步**：下次死锁后把那个目录整个要过来 —— 不必再追问"多久、什么负载"。
若目录是空的，说明它没判定成死锁（比如卡的不是 D 状态），那本身就是线索。
手工对照仍可用 `gaokun3-qrtr-lookup` 比服务表：少了哪个服务就指向哪个 DSP。
⚠️ 别把 `Handover signaled` 当崩溃证据，那是良性噪声（#37 已用对照实验证明）。

### A2. 硬件视频【编码】（Venus）— ⚠️ **已查明并【故意关闭】，不是待验证项**
解码 ✅ 已随 v0.4.0-alpha 发布（`c2.v4l2.avc.decoder` 实测解出 30 帧，
案卷 [#41](stage4-findings.md)）。

★ **编码这一侧 2026-08-22 已经实测并定性**，结论写在
[device.mk](../device/huawei/gaokun3/device.mk) 那段属性旁边（原文可查）：

```
E EncodeComponent: Unable to parse RGBX_8888 from IMPLEMENTATION_DEFINED
E EncodeComponent: Failed to get input block layout
E ...: Attempted to lock() a buffer that was not allocated with a
       BufferUsage::CPU_* usage.
```

SurfaceFlinger 交出来的是 **RGBX_8888 / IMPLEMENTATION_DEFINED**，
而 Venus 编码器要 **NV12**，`v4l2_codec2` 的 `EncodeComponent` **不做这个转换**
—— 属于组件本身的功能缺失，不是配置能解决的。

⚠️★ **而且开着比关着更糟**：编码组件 rank `0x80` 会压过软编的 `0x200`，
于是应用**直接失败，而不是回退到软编**。所以两条
`ro.vendor.v4l2_codec2.encoder.supported.*` 属性是**有意注释掉**的，
`scripts/verify-venus-codec2.sh:57` 也据此断言"**必须没有**编码组件"。
录屏/录像走软编，能用。

**真要做的话**，工作量在 `external/v4l2_codec2` 的 `EncodeComponent`：
让它认 `IMPLEMENTATION_DEFINED`，向 gralloc 问出真实布局并协商 NV12
（和已经修掉的两条解码 bug 同型：v4l2_codec2 照搬 ChromeOS 行为、venus 守规范）。

⚠️ 记一条方法论：2026-08-23 我又跑了一次 `screenrecord`，失败信息是
`ERROR: UNASSIGNED_LAYER_STACK` —— **那跟编码器毫无关系**，
是当时**屏幕是灭的**（`mWakefulness=Asleep`），SurfaceFlinger 拒绝抓屏。
**灭屏状态下的 screenrecord 结果不能用来判断编解码器。**

### A3. 自动亮度（环境光）— 芯片在总线上不应答，四个软件维度已扫空
详见 [#72](stage4-findings.md)（并已作废 #43/#68/#70 的历次归因）。

**当前定性**：光感驱动**在 SLPI 固件里**（`strings` 见 tcs3701 120 次），
`default_sensors.json` **声明了** `.ambient_light`，SEE 也确实在 probe ——
**但芯片不应答**。阴性对照给出了判据：把能用的加速度计地址改错，
得到的失败签名与光感**一模一样**（都是「SSC 说没有传感器提供 data_type=...」）。

**已扫空的维度**（每格实测，不是推理）：
`bus_instance` **0–7 全扫** ／ `bus_type` 0–3 ／ `rail_on_state` 1 与 2 ／
Linux 没占那条总线（只有 4 条 i2c 适配器、无 0x39/0x46）／
Linux 没占中断脚（TLMM 32 与 127 都 `UNCLAIMED`）。
另：`is_dri = 0` ⇒ 光感**本来就走轮询**，「DRI 中断没到」这条假说**不成立**。

**新掌握的结构**：配置是**二供料**方案 —— IMU（sh3001 ✅／t1000）在 bus 1，
光感（tcs3701／sy3133cs）都在 bus 5。t1000 与 sh3001 同参数而只有后者注册出来，
⇒ SEE 的「探测择优」机制本身是正常的。

★ **一条一直没人注意的事实**：喂给 DSP 的 `hw_platform = QRD` 是**我们自己编的**
（主线不导出它）。所以这整套是**高通参考设计的配置**，板级差异原本在
Windows DriverData 注册表里，**已随抹除 Windows 丢失**。

**剩下的可能性**（按可行性）：
① 芯片没上电／被复位钉住（与触摸屏 gpio174 同一类；那些脚在 SSC 域，AP 看不到）；
② 真实总线/地址只存在于已丢失的注册表里；
③ 抄一台光感在主线上能用的 sc8280xp（ThinkPad X13s）。

★ **可复用工具已入库**：`scripts/ssc/`（60–90 秒一次的实验循环 +
带回读验证的改字段工具 + 必跑的阳性/阴性对照）。**下次动这块先读它的 README。**

⇒ **自动亮度暂时做不了。自动旋转与游戏体感不受影响。**

### A5. 恢复出厂设置不起作用
设置里那条路走 misc 的 BCB + recovery，而本机没有可用 recovery
（[#39](stage4-findings.md)）。实机证据：misc 里躺着一条没人消费的 `boot-recovery`。

**现在的替代**：从救援 Linux `mkfs.ext4 -F /dev/disk/by-partlabel/userdata`。
**真正的修法**：见 B3（EFI 加载器）或让 recovery 能启动（已搁置）。

### A6. USB-C 外接显示（UCSI）★ 现在还欠着待机那笔账
`PPM init failed -ETIMEDOUT`，本机主线已知缺陷，`/sys/class/typec/` 是空的。
代价还包括 USB 只有 high-speed（SuperSpeed 需要 UCSI 切 orientation）。

★ **它现在是三个问题的共同根因**：没有 role 源正是 `a600000.usb` 停在
半初始化 `device` 态的原因（[#52](stage4-findings.md)），而我们为此付的代价
就是**息屏时 USB adb 断开**。UCSI 修好 → role 被正确指派 → 那个取舍自动消失。
⚠️ 顺带说明这条为什么值得排在摄像头前面：**USB adb 掉线是本项目迄今最大的
效率税**（#27），每次都要靠扫网段 + TCP adb 找回来。

### A8. 设备的 WAN 吞吐只有 PC 的 1/20（原因未定）
详见 [#44](stage4-findings.md)。⚠️ **不是 WiFi、不是 ath11k** ——
那条归因我下错过并已推翻：ping 网关 **0% 丢包**（1400 字节大包也是），
从本机拉 200 MB 跑到 **61.7 MB/s**，全程 `msdu_done` 新增 **0**。

真正剩下的问题：设备从 R2 拉只有 **1–2 MB/s**，而同一网络里的 PC 拉同一个
URL 是 **36.9 MB/s**。对"用户走系统内 OTA 升级"有实际影响（1 GB 要二十多分钟）。

**第一步**：在设备上对同一个 URL 抓一次 `ss -ti` 看拥塞窗口与重传，
再换一个不同 CDN 的大文件对照 —— 先分清是"到 Cloudflare 这条路"还是
"设备的 TCP 行为"。

### A7. 摄像头
基本没碰 —— 但**不是"完全没碰"**：08-31 编过一个相机内核并在 ESP 上留了
启动条目（`camss`/`cci`/`hi846`/`s5k3l6xx` 从 `=m` 翻成 `=y`），
**结果如何没有任何记录**，案卷和 git 里都查不到。那个二进制已于 09-11 删除，
删前把配方挖出来存进了 [#79](stage4-findings.md) 第四节 ——
要重来的话从那 8 个符号开始，不用再摸一遍。

★★ **而且当时已经查到了后摄不工作的真凶**（09-11 从构建机旧树捞出来的，
见 `patches/camera-wip/README.md`）：**后摄的 `vdda`(l2b) 被 DSI 的 `vddi`
钉在 1.8 V，而 S5K3L6 要 2.8 V** —— sensor 在 CCI 上直接 NAK（i2c `-6`）。
且 v7.2 的 camss 用 `fwnode_graph_for_each_endpoint` 遍历端点**不检查可用性**，
一个"接了但永远绑不上"的 sensor 会**卡死整个 v4l2-async notifier**，
**连前摄也拿不到 `/dev/v4l-subdev*`**。
⇒ **后摄不是驱动问题，是供电轨被显示占了。第一步是查 l2b 能不能独立，
不是继续调驱动。** 驱动与 dtsi 已归档在 `patches/camera-wip/`（不应用）。

---

## A′. 已关闭（只留索引，细节在案卷里）

* **s2idle 待机** ✅ v0.3.0-alpha。真凶是我们自己加的 `dr_mode="otg"`。
  [#52](stage4-findings.md)–[#57](stage4-findings.md)
* **耳机口 + 内置麦克风** ✅ 用户确认出声。[#40](stage4-findings.md)
* **普通应用能 panic 内核** ✅ v0.4.0-alpha，`patches/0013` 删掉 `drm_crtc`
  里那行竞态 `BUG_ON`。[#58](stage4-findings.md) / [#62](stage4-findings.md)
* **硬件视频解码** ✅ v0.4.0-alpha。[#41](stage4-findings.md)
* **亮度调节** ✅ 真 lights HAL 取代了只接受数值不干活的 stub。
* **扬声器音量偏小** ⚠️ **返工中，别当已解决。** 原修法（WSA 数字上限 81→90，
  +5.7 dB）**抬错了那一级** —— 数字级在 DAC 之前，+6 dB 把 −6 dBFS 的内容顶到
  0 dBFS，实测 THD **−20 dB（约 10% 失真）**。现改为数字压在单位增益 84、
  响度由 PA 出（上限 17→23，运行时 17→试 21）。
  ✅ **内核这一半已上机验实**（2026-09-11，[#80](stage4-findings.md) 第七节）：
  `tinymix` 查到 `SpkrLeft PA Volume` 范围变成 **`0->23`**、
  `WSA_RX0 Digital Volume` 变成 **`0->84`**，写 24/90 都被钳住。
  ⬜ **但用户还听不到** —— 设备上的 ROM 是 08-24 那版，`/vendor/bin/audio-route.sh`
  仍是旧脚本，开机照老结论设 `Digital Volume 90`（**被新内核拒了**，
  这反倒是新上限生效的证据）和 `PA=12`。
  ⚠️★ **内核与 ROM 在本项目是分开发布的，所以"补丁已入库"≠"用户听得到"。**
  **第一步：构建一版带新 `audio-route.sh` 的 ROM**（本仓那份早就改好了，
  两步写法让同一份 ROM 在新旧内核上都对）。
  在那之前运行值已手工设成 PA 21 / dig 84，**重启会被旧脚本改回 12**。
  [#78](stage4-findings.md) / [#79](stage4-findings.md)
* **插着键盘时屏幕键盘不弹** ✅ `show_ime_with_hard_keyboard` 默认置 1。
  ⚠️ 键盘开关**不是**这条的解药（Android 看键盘设备存不存在，不看 inhibited）。

---

## B. 工程债与正确性

### B1. SELinux 转 enforcing —— **四步已走完，剩两个结构性阻塞**
[#75](stage4-findings.md) / [#76](stage4-findings.md) / [#77](stage4-findings.md)。
2026-08-23 夜随构建戳 `1787436126` 装机验收：

* ✅ 第 1 步 定义域 —— **`init` 域里只剩 PID 1**（此前 5 个服务挤在里面）
* ✅ 第 2 步 设备节点类型（c2 服务、6 个 nvme 分区、`/dev/fastrpc-*`、`/dev/mem`）
* ✅ 第 3 步 sysfs genfs 标签 —— ★ **`network_stack` 236→0、
  `hal_health_default` 235→0，零 allow 规则**
* ✅ 第 4 步 按真实主体写 allow（hexagonrpcd / 传感器 HAL 的 QRTR /
  boot_control 的挂载 / 键盘），`sepolicy_neverallows` 通过
* ✅ 功能零回归：传感器、声卡、WiFi、root 8/8

**还剩两个加规则解决不了的**（这才是 enforcing 的真正门槛）：

1. **`gaokun3_hangdump`** —— 读 debugfs 那条 neverallow
   （`domain.te:1527`）**没有 userdebug 豁免**；而它还要读所有域的 `/proc`。
   它本质上是 `dumpstate` 那一类工具。
   **出路**：binder-debugfs 换成 `dumpsys`，或做成只在 userdebug 启用的诊断件。
2. **`gaokun3_smmustall`** —— 要 `sys_rawio` + `/dev/mem`。写得进去
   （那条 neverallow 有 userdebug 豁免），但会让 enforcing 与否取决于构建变体。
   ★ **正解是先做 B6**，脚本整个消失，这道坎一起没了。

⬜ 另有一处未解：genfscon 是**前缀匹配**，我给 UCSI 的 `power_supply` 打标签
时连带盖住了它下面的 `wakeup23`（本该是 `sysfs_wakeup`）。`wakeupN` 编号动态，
逐条 genfscon 不现实。**⚠️ 症状是 denial 的类型变了而不是消失 —— 别误读成进展。**

### B2. 真温控 HAL
现在是 AOSP mock（温度恒定 30.1/30.2），框架完全没有真实温控感知。
⚠️★ **换成读 `/sys/class/thermal` 的真 HAL 时必须同时改阈值** ——
mock 报的 skin/battery SHUTDOWN 阈值只有 **36 °C**，而
`ThermalManagerService.shutdownIfNeeded()` 到 SHUTDOWN 会直接
`powerManager.shutdown()`。现在因为 mock 值恒定打不到，**换真 HAL 会开机
几分钟就自动关机**。

### B3. 自研 EFI 加载器（规范化的最后一段）
读 `misc` 的 `bootloader_control` 选槽 + 解析 Android boot 镜像 +
装 initrd/DTB 协议。做完之后：
* postinstall 钩子与 ESP 上的派生文件**全部可以退役**
* BCB 能被消费 → `adb reboot recovery` 与恢复出厂设置才有可能工作
* 是 AVB/verified boot 的前提

**安全阀已实测可用**（[#73](stage4-findings.md)，2026-08-23）：systemd-boot 的
`efi` 指令在这台机器上确实能 LoadImage + StartImage 另一个 EFI 应用，
三轮实验全部自动回到 Android，**没有人碰过机器**。所以"固件不支持 chainload"
这个顾虑不成立，开发这个部件不需要有人守在机器旁。
⚠️ 但 `efi` 条目**拿不到 initrd**（`boot.c:2428` 直接按类型返回），
`devicetree` 倒是照装 —— 拿它引内核必须自带 `panic=10`。

⚠️ **真正的物理约束是 ESP 只剩 28 MB**（296M 用了 268M）。
里面有 70 MB 的 `Persisted_Capsules.bin` 和 31 MB 的 `EFI/`
（含已抹除的 Windows 整棵树）。要往 ESP 加东西，先腾地方。

### B4. LiveCD 图形安装器 —— ⏸ **用户决定暂时搁置**（2026-08-23）
接手说明见 [stage7-installer-roadmap.md](stage7-installer-roadmap.md) 末尾。

**已验过**：后端 probe/plan/apply/shrink 全部端到端测过（apply 在 loop 设备上、
shrink 在真 NTFS+ext 上验了文件 md5 与 PARTUUID）；20 屏离线渲染；
DRM 输出 + 触摸 + 键盘 + 软键盘在真机上跑通。

**还欠**：WiFi 页重做成两步式、apply 接网络安装、专业分区页的值回灌 plan、
**端到端真装一次**（从来没有真的装过一台机器）。

★ 回来时记住：**设备自己能编译**（Alpine aarch64 + 网），
改一行到真机看见效果约 30 秒，不用重建镜像。

### B6. GPU SMMU 中断根治
实际 DT 是全局 672/673、context bank 从 678 起；而硬件拉的是 675/680，
其中 680 被分给 CB2、675 整张表里根本没有。很像 CB 起始偏移就错了。
⚠️ 但只凭"675/680 挂起"推不出正确映射，而且**改错了没有任何征兆**
（只是继续收不到 fault）。做成之后可以丢掉常驻的 `smmu-nostall.sh` 轮询。

### B7. 用轻量系统替掉救援 Ubuntu（★ 与 B4 是同一件事）
**★ M0 完成（2026-08-23）**：Alpine 救援系统已在硬件上跑起来 ——
ssh 可达、WiFi 自动连上、全套分区工具就位、`lsblk` 看得见内置盘 8 个分区。
案卷 [stage7-live-installer.md](stage7-live-installer.md)。

产物：squashfs **55 MiB** + initramfs **2.7 MiB**（含 WCN6855 固件）。
它要替掉的是 **24.6 GiB** 的 Ubuntu 分区。

⬜ **还没删 p3**，因为现在 squashfs 就放在 p3 上（验证期故意不动分区表）。
下一步：按设计建 1 GiB 的 `gk3rescue` 分区、把 squashfs 挪过去、
确认能启动之后再回收那 24.6 GiB。

### B8. `invalid volume index range in the curve` ×12（既有，非回归）
每次 audioserver 启动都吐 12 条 `E APM_AudioPolicyManager: invalid volume index
range in the curve:`（后面是空的，连哪条曲线都没说）。
**确认与耳机改动无关**：干净 A/B，旧策略 12 条、新策略 12 条。
来源应在 `audio_policy_volumes.xml` / `default_volume_tables.xml`（两份都是从
`frameworks/av/services/audiopolicy/config/` 原样拷的）与本机 `devicePorts`
的交集上。目前没有可观测的功能损害，故只记不修。

**第一步**：给那条日志找出打印点（`EngineBase`/`VolumeCurve`），看它校验的是
哪个字段，再对照我们装进去的两份 XML。

---

### B9. SLPI 每 200 ms 一条 handover 噪声（根因未查）
详见 [#59](stage4-findings.md)。**它无害但有代价** —— 正是它把 [#58](stage4-findings.md)
那两次 panic 的调用栈从 pstore 里挤掉了（45 条记录里有用的不到 10 条）。
`patches/0014` 已把打印改成 ratelimited（治症状，正确且值得上游），
但**远端为什么每 200 ms 翻一次 smp2p 位仍然不知道**。

硬证据：SLPI 的 `q6v5 ready` 与 `q6v5 handover` 两条中断计数**完全相同、
同步增长**（5 秒 5475→5502），ADSP/CDSP 各只有 2。

**第一步**：5 Hz 很像一个采样节拍 —— 停掉 sensors HAL / `hexagonrpcd`
看频率变不变。⚠️ **别在没人看着时做**：M12 记过停/重启 HAL 会污染 SSC 会话，
自动旋转当场失效、要重启 `hexagonrpcd` 并等约 20 秒才恢复。

### B11. root（ReSukiSU）—— ✅ **已随 ROM 常驻**
2026-08-23 夜装机验收（构建戳 `1787436126`，slot_a）：
`scripts/verify-root.sh` **8/8**，`/data/adb/ksud` 就位，管理器已装。
★ postinstall 写进 slot_a 的内核与手工验过的那个 **sha256 逐字节相同**。

⬜ 还剩两个**产品决定**（都不是技术问题）：
* 要不要预装管理器 APK。★ ROM 侧其实什么都不用加 —— `ksud` 就在 APK 的
  `lib/arm64-v8a/libksud.so` 里，装 App 即到位。
* 要不要在 cmdline 给 `kernelsu.allow_shell=1`（adb shell 直接拿 `su`）。
  **建议不给**：那等于任何能连 adb 的人无确认拿 root。

⚠️ 发版说明里应当写明：**root 会影响 Play Integrity 和部分带反作弊的手游**。

### D5. PR #3 待回复（已审完，等你定措辞）
线上那个 PR 动的正是内核预编译这一块。我把要问的整理好了，**没有发到 GitHub**
—— 对外发言等你。三个问题：`dr_mode=host` 是不是有意为之（那正是我们 #52 的
取舍另一半）；他们刷完之后 USB adb 还通不通；能不能公开那份内核 `.config`
（我们这边的断言是 52 条 MUST_Y + `VIDEO_QCOM_IRIS` MUST_N，可以对一遍）。

### D6. 把 `drm_crtc` 那个 `BUG_ON` 报到 dri-devel（对外动作，等你定）
`patches/0013` 修的是**上游 mainline master 现存**的缺陷：普通应用查一次
present fence 的名字就能把整机 panic 掉。稿子照那个补丁的 commit message
改一改就能发。我不代发对外邮件。

### D7. v0.2.0-alpha 的 R2 产物要不要删（2.1 GiB，等你定）
`install/v0.2.0-alpha/` + `builds/…20260820….zip`。**它们正被 v0.2.0-alpha 的
GitHub 发布页链接着**，删了那个页面的下载链接会 404。桶现在 6.6 GiB，
免费额度 10 GB，不删也还撑得住。

---

## E. 明确搁置（记录理由，不是忘了）

* **recovery** —— 启动即复位循环（[#39](stage4-findings.md)）。
  现阶段意义不大：sideload 被系统内 OTA 覆盖，而调试它需要人反复到机器旁
  （本机没有串口、recovery 没有网络栈、pstore 对这类失败无效）。
  真要做，**第一步是把 USB adb 在 recovery 里弄通**，那是唯一能看见内部的通道。
* **fastboot** —— bootloader 级**不可能**（固件是 UEFI，不是 fastboot 设备）。
  用户态的 `fastbootd` 住在 recovery 的 ramdisk 里，所以随 recovery 一起搁置。
* **GMS / Play 商店** —— 用户未提出需求。
* **突破原神 1080×1728 的渲染上限** —— 那是游戏按**设备白名单**给的档位，
  不是本机的技术限制。要突破只能伪装机型，**有账号风险**，留给用户决定。
