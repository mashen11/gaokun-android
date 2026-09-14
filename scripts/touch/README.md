# 触摸手感的测量工具

★ **先读 [`docs/stage4-findings.md` #114](../../docs/stage4-findings.md)。**
这三个工具是那一仗留下的，解决的是同一个问题：**"手感不好"没法调试，除非先变成数字。**

| 文件 | 干什么 |
|---|---|
| `capture-ab.sh` | 在设备上跑。录 `/dev/input/event7` 原始流，**按收到多少数据**切换 A/B 预设 |
| `evdev-strokes.py` | 在本机跑。把原始流解码成轨迹，输出轨迹数 / 碎片率 / 每条帧数 / 速度分布 |
| `tracker-sim.c` | 在本机跑。`hx_track_contacts()` 的逐行复刻，不用硬件就能验证跟踪器的改动 |

## 为什么不用 `getevent`

`timeout N getevent > 文件` **会因块缓冲丢光全部输出**（本仓 #26 的老坑）。
一律 `cat /dev/input/eventX` 录二进制、离线解码。

## 典型用法

```sh
adb push scripts/touch/capture-ab.sh /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/capture-ab.sh
adb shell 'nohup /data/local/tmp/capture-ab.sh >/dev/null 2>&1 &'
# ……让用户照常滑动，收够自动切换、自动结束……
adb pull /data/local/tmp/ts_A.bin . && adb pull /data/local/tmp/ts_B.bin .
python3 scripts/touch/evdev-strokes.py ts_A.bin ts_B.bin
```

⚠️ `capture-ab.sh` 里的 `event7` 与算法路径 `/sys/bus/spi/devices/spi0.0/algo/` 都可能变 ——
先 `grep -A9 -i himax /proc/bus/input/devices` 确认。

## 判据（#114 实测到的量级）

一次连续滑动**应该只产生一条轨迹**。如果 8 秒的甩动产生了 87 条轨迹、其中 37% 不到 5 帧，
那就是跟踪器在自毁，而不是"手感需要微调"。
