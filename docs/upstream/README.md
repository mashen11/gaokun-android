# 上游投稿草稿（未发送）

五份英文补丁稿，由本仓 `patches/0031`、`0027`、`0034`（拆成 OF 匹配表与 get_selection 两份）、`0035` 改写提交说明而来。
**发出去是对外动作，等用户点头。** 发前要在干净的 linux-next 上 `git apply --check` 一次，
并补 `dt-bindings` 里 `ovti,ov13b10` 的 binding（0003 需要，目前主线没有这个 binding 文件）。

| 文件 | 收件人（get_maintainer） |
|---|---|
| 0001 camcc RCG shared | linux-clk, linux-arm-msm, Bjorn Andersson, Konrad Dybcio |
| 0002 GDSC wait values | 同上 |
| 0003 ov13b10 OF match | linux-media, Sakari Ailus, Arec Kao |
| 0004 camss：等不到的传感器不再拖死 notifier（**RFC**） | linux-media, linux-arm-msm, Robert Foss, Bryan O'Donoghue, Hans Verkuil |
| 0005 ov13b10 get_selection | linux-media, Sakari Ailus, Arec Kao |

0003 的 hunk 是从 0034 里机械抽取的，**发前必须重新 `git apply --check`**。

0004 是 **RFC**：它用超时做决定（模块参数 `sensor_wait_ms`），上游可能更想在 v4l2-async 层解决或让 DT 表达
"二选一"——提交说明里已经把这个问题抛出来了。0005 的 hunk 行号已归一到旧文件侧，同样发前 `git apply --check`。
两份都来自 2026-09-14 内核 `#19` 实机验过的代码（#110）。
