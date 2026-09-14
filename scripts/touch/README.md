# 触摸手感的测量工具

★ **先读 [`docs/stage4-findings.md` #114](../../docs/stage4-findings.md)。**
这三个工具是那一仗留下的，解决的是同一个问题：**"手感不好"没法调试，除非先变成数字。**

| 文件 | 干什么 |
|---|---|
| `dump-state.sh` | **一键取证**。把计数器、触点出生记录、两张原始网格、27 个旋钮、轴信息、IRQ 计数一次抓全 |
| `grid.py` | 把 40×60 电容网格画成看得懂的图；给两个文件就并排比较 |
| `capture-ab.sh` | 在设备上跑。录 `/dev/input/event7` 原始流，**按收到多少数据**切换 A/B 预设 |
| `evdev-strokes.py` | 在本机跑。解码成轨迹：轨迹数 / 每条帧数 / 速度分布 / **相邻轨迹间隔**（见下） |
| `tracker-sim.c` | 在本机跑。`hx_track_contacts()` 的逐行复刻，不用硬件就能验证跟踪器的改动 |

## 驱动侧的可观测接口（内核 **#22** 起，`patches/0043`）

| 路径 | 内容 |
|---|---|
| `/sys/bus/spi/devices/spi0.0/algo/stats` | 22 个逐级计数器。**写任意值清零** |
| `…/algo/contacts_log` | 最近 16 个触点**出生时**的 `seq x y area signal edge` |
| `/sys/kernel/debug/himax-hx83121a/frame_raw` | 面板**产出**的 40×60 s16 网格（仅去基线）|
| `/sys/kernel/debug/himax-hx83121a/frame` | 流水线**判定**的同一张网格（CMF/边缘增强/IIR 之后）|

★ 并排看两张网格是**区分"信号问题"与"算法问题"**的唯一办法。
★ 空载读 `frame_raw` 是回答**"固件到底做不做逐像素基线跟踪"**的唯一办法。

⚠️ 整帧要用 `adb exec-out`，**不能用 `adb shell`** —— 后者会把 `\n` 变成 `\r\n`，
把二进制帧毁掉。`dump-state.sh` 已经处理了，并且会校验是不是 4800 字节。

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

## 判据

**别用"短轨迹的比例"。** 它只在用户【连续滑动】时才有意义 —— 正常使用里点按本身就是
短轨迹，会得出假的回归结论（2026-09-14 我栽过一次，#114 §7bis）。

要用**相邻轨迹之间的间隔**（`evdev-strokes.py` 默认就跟着算）：手指若真抬起再按下，
跨过间隔的**隐含速度应当接近 0**；若是一条滑动被切开，手指在"隐形"期间仍在移动，
隐含速度就等于它当时的滑行速度。

| | 间隔 <100 ms 的占比 | 跨间隔隐含速度 |
|---|---|---|
| #114 修复前 | 35% | **1.01 m/s**（= 那条限速线本身）|
| 修复后 | 0/45，最小间隔 124 ms | — |

另一个可用的量级参考：同样的甩动，修复前 8 秒产生 **87 条**轨迹，修复后 **6 条**。
