# 上游投稿草稿（未发送）

三份英文补丁稿，由本仓 `patches/0031`、`0027`、`0034`（只取 OF 匹配表那一块）改写提交说明而来。
**发出去是对外动作，等用户点头。** 发前要在干净的 linux-next 上 `git apply --check` 一次，
并补 `dt-bindings` 里 `ovti,ov13b10` 的 binding（0003 需要，目前主线没有这个 binding 文件）。

| 文件 | 收件人（get_maintainer） |
|---|---|
| 0001 camcc RCG shared | linux-clk, linux-arm-msm, Bjorn Andersson, Konrad Dybcio |
| 0002 GDSC wait values | 同上 |
| 0003 ov13b10 OF match | linux-media, Sakari Ailus, Arec Kao |

0003 的 hunk 是从 0034 里机械抽取的，**发前必须重新 `git apply --check`**。
