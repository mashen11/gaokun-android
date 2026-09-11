#!/vendor/bin/sh
# 触摸手感模式：game（更跟手）/ daily（驱动默认，更稳）
#
# 后端是 himax hx83121a 驱动自己暴露的算法参数
# （/sys/bus/spi/devices/spi0.0/algo/，共 27 项），运行期可改、立刻生效。
# 这里只动三项，取自上游 EGoTouchRev 的 game_preset：
#   track_smoothing      基于速度预测的坐标平滑。开着更稳，代价是多一层延迟。
#   track_start_debounce touch_active 要连续几帧才确认。2 帧 ≈ 按下多 2 帧延迟。
#   track_jump_dist2     跳点检测阈值的平方（6400 = 80 像素）。0 = 不检测。
#
# ⚠️ 只动这三项是【故意的】。其余 24 项（CMF 共模滤波 / IIR / 掌压 / 边缘补偿）
#    是按这块面板标定出来的，乱改会带来幽灵触点或断触，属于要实机逐项验的活。

MODE="$1"
case "$MODE" in
    game)  SMOOTH=0; DEB=0; JUMP=6400 ;;
    daily) SMOOTH=1; DEB=2; JUMP=0    ;;
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
    log -t gaokun3-touch "触摸模式 = $MODE（$ALGO）"
else
    log -t gaokun3-touch "触摸模式 $MODE 未完全应用"
fi
exit $fail
