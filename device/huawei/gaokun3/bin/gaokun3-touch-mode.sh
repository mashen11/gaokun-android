#!/vendor/bin/sh
# 触摸手感模式：game（更跟手）/ daily（驱动默认，更稳）
#
# 后端是 himax hx83121a 驱动自己暴露的算法参数
# （/sys/bus/spi/devices/spi0.0/algo/，共 27 项），运行期可改、立刻生效。
# 这里只动三项，取自上游 EGoTouchRev 的 game_preset：
#   track_smoothing      基于速度预测的坐标平滑。开着更稳，代价是多一层延迟。
#                        实现是 x = (3*旧 + 新)/4，稳态滞后 3 帧 ≈ 25 ms —— 不小。
#   track_start_debounce touch_active 要连续几帧才确认。2 帧 ≈ 按下多 1 帧（8 ms）延迟，
#                        换来的是噪声不那么容易变成一次"按下"。
#   track_jump_dist2     跳点检测阈值的平方。⚠️★★ **两版预设一律给 0**，见下。
#
# ⚠️★★★ 2026-09-14：`track_jump_dist2=6400` 是本仓自己制造的重大缺陷，已从两版预设清掉。
#    驱动的跳点检测拿【原始位移】跟它比，于是它等于一条 **1.0 m/s 的限速线**；越过之后
#    轨迹被复位、新槽的 debounce 永远回不到 0 ⇒ **手指只要持续快过它，驱动一个点都不上报**。
#    实测（同一个人、同样的甩动、录 event7 原始流）：
#        jump=6400  手指在屏 8.0 s / 路径 3.99 m → 系统收到 **87 条**轨迹，37% 是 ≤5 帧碎片
#        jump=0     手指在屏 10.3 s / 路径 4.07 m → 系统收到 **6 条**，0 碎片
#    一次连续滑动被切成十几次独立触摸：滑动惯性反复清零（列表甩不动）、手势被判取消，
#    而沿途多出来的那些"按下"**就是用户报的"幽灵触摸"**。两个症状同一个根因。
#    内核 `patches/0038` 把判据改成【与预测位置的偏差】之后这个功能才真正可用；
#    在带 0038 的内核上实机验过之前，**这里保持 0**。案卷 `docs/stage4-findings.md` #114。
#
# ⚠️ 只动这三项是【故意的】。其余 24 项（CMF 共模滤波 / IIR / 掌压 / 边缘补偿）
#    是按这块面板标定出来的，乱改会带来幽灵触点或断触，属于要实机逐项验的活。

MODE="$1"
case "$MODE" in
    # game 与 daily 现在只差 track_smoothing —— 那才是"跟手 vs 稳"的真实取舍。
    game)  SMOOTH=0; DEB=2; JUMP=0 ;;
    daily) SMOOTH=1; DEB=2; JUMP=0 ;;
    *) log -t gaokun3-touch "用法: $0 game|daily"; exit 2 ;;
esac

# ★ 不写死那条很长的 DT 路径（/sys/devices/platform/soc@0/9c0000.geniqup/...）：
#   换内核或 SPI 编号变了它就失效，而且失效时【毫无征兆】。按 algo 目录找。
ALGO=""
for d in /sys/bus/spi/devices/*/algo; do
    [ -d "$d" ] && ALGO="$d" && break
done
[ -n "$ALGO" ] || { log -t gaokun3-touch "找不到 algo 目录 —— 触摸驱动没起来？"; exit 1; }

fail=0
for kv in "track_smoothing=$SMOOTH" "track_start_debounce=$DEB" "track_jump_dist2=$JUMP"; do
    k="${kv%%=*}"; v="${kv##*=}"
    if ! echo "$v" > "$ALGO/$k" 2>/dev/null; then
        log -t gaokun3-touch "写 $k 失败"; fail=1; continue
    fi
    # ★ 回读校验：sysfs 写入成功不代表值被接受（驱动可能钳位或拒绝）
    got="$(cat "$ALGO/$k" 2>/dev/null)"
    [ "$got" = "$v" ] || { log -t gaokun3-touch "$k 回读 $got != 期望 $v"; fail=1; }
done

if [ "$fail" = 0 ]; then
    log -t gaokun3-touch "触摸模式 = ${MODE}（${ALGO}）"
else
    log -t gaokun3-touch "触摸模式 $MODE 未完全应用"
fi
exit $fail
