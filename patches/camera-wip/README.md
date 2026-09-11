# 相机半成品（**只归档，不应用**）

⚠️ **这里的补丁不在 `scripts/kernel-apply-patches.sh` 的 KPATCHES 里，
也不应该被加进去。** 它们是 2026-08-30/31 那次相机实验的残骸，
**从未验证过**，结果如何当时也没人记录。

## 为什么要入库

2026-09-11 查启动失败时发现：这些源码此前**只活在构建机的内核树里**
（`~/gaokun/mainline-linux`），而同一棵树上的 `ashmem` 与 `xt_quota2`
已经因为树被重置而险些丢失，并直接导致了一次上机失败
（见 [`docs/stage4-findings.md`](../../docs/stage4-findings.md) #79）。

**入库是为了防止第三次丢失，不代表它能用。**

## 内容

| 文件 | 是什么 |
|---|---|
| `media-i2c-s5k3l6xx.patch` | Samsung S5K3L6XX 后摄驱动（48 KB 新文件）+ `drivers/media/i2c/` 的 Kconfig/Makefile 接线 |
| `camera-dtsi-only.patch` | `sc8280xp-huawei-gaokun3-camera.dtsi` 的改动 |

⚠️ **`sc8280xp.dtsi` 的改动没有收进来** —— 那份 diff 里 335 行**全是本仓
已入库的 venus（`upstream-venus/0019`）与 cooling maps（`0009`）**，
不含任何相机内容。别被"改过 sc8280xp.dtsi"误导。

## ★ 里面有一条比驱动本身值钱的实测发现

`camera-dtsi-only.patch` 的注释里记着（原文在补丁里）：

> 后摄 `port@0`（csiphy0 → s5k3l6）被**删掉**了，因为 `vdda`(l2b) 被 DSI 的
> `vddi` 钉在 **1.8 V**，而 S5K3L6 要 **2.8 V** —— sensor 在 CCI 上直接 NAK
> （i2c `-6`）。而 v7.2 的 camss 用 `fwnode_graph_for_each_endpoint` 遍历端点、
> **不检查可用性**，所以一个"接了但永远绑不上"的 sensor 会**卡死整个
> v4l2-async notifier** —— 连前摄也拿不到 `/dev/v4l-subdev*`。
> 共享电源轨的冲突需要板级证据才能定。

⇒ 也就是说：**后摄不是"驱动没写"，是供电轨被显示占了。**
真要做相机，第一步是查 l2b 这条轨到底能不能独立，而不是继续调驱动。

⚠️ `docs/TODO.md` A7 此前写着相机"完全没碰"，与这里的事实不符，已更正。
