#!/system/bin/sh
# check-pen-android.sh -- Android 侧「笔到底认不认」的判定脚本
#
# 放在设备上跑。回答一个问题：
#
#     内核已经报上 MT_TOOL_PEN 之后，Android 到底把笔当笔了吗？
#
# ------------------------------------------------------------------
# 为什么需要这个脚本（而不是照抄一份 .idc 就完事）
# ------------------------------------------------------------------
# 2026-10-05 核实 AOSP 官方文档，推翻了「必须加 .idc 才能认笔」这个说法：
#
#   * touch.deviceType 的合法取值只有
#     touchScreen / touchPad / pointer / default -- 没有 stylus。
#     => 写 touch.deviceType = stylus 不会让设备变成 stylus，只会被忽略。
#   * MotionEvent.getToolType() 返回 TOOL_TYPE_STYLUS 的唯一来源是内核事件：
#     MT_TOOL_PEN / BTN_TOOL_PEN（MT_TOOL_BRUSH/PENCIL/AIRBRUSH 也映射成 STYLUS）。
#     => .idc 对「这是不是笔」没有任何影响。
#
# 所以真正的关键全在内核，而本脚本把「内核报了」和「Android 认了」分开判定
# -- 两者可能不一致（内核报了但 Android 没认，或反过来）。
#
# ------------------------------------------------------------------
# 三层判定（从硬到软）
# ------------------------------------------------------------------
#   L1 设备分类  dumpsys input            设备 sources 含 SOURCE_STYLUS
#   L2 能力位    getevent -pl 该事件节点   ABS_MT_TOOL_TYPE 存在
#   L3 实际事件  getevent -lt 抓一段      看到 ABS_MT_TOOL_TYPE = 1（MT_TOOL_PEN）
#
# L3 才是终判：前两层是静态能力，L3 才是「这笔真的被标成笔了」。
# 三层全过 => App 的 getToolType() == TOOL_TYPE_STYLUS 就成立。
#
# ------------------------------------------------------------------
# ★★★ v1.1 修掉的三个 bug（2026-10-07，都在真机上抓到的）
# ------------------------------------------------------------------
# 这一版之前，本脚本在一台**笔通路完全正常**的设备上报了 2 个假 FAIL。
# 三个 bug 都属于同一族：**"什么都没查到" 被当成了 "查到了坏结果"**。
#
#  B1  L1 用 `grep -A 4 "<设备名>"` 取 dumpsys 的块，但 `Sources:` 在块里
#      第 9 行左右 -- 4 行窗口永远够不着 => 恒报 NO SOURCE_STYLUS。
#      修：用 awk 取**整个** `Device N: <名>` 块。
#
#  B2  L2 用 `grep ABS_MT_TOOL_TYPE /proc/bus/input/devices`。
#      但那个文件**只有 bitmask，没有轴名字**（形如 `B: ABS=2e0800000000003`）
#      => 这个 grep **在物理上不可能命中** => 恒报 NO ABS_MT_TOOL_TYPE。
#      修：改用 `getevent -pl <eventN>`（能力位的事实源）。
#      参考：0x2e0800000000003 = 2^0|2^1|2^47|2^53|2^54|2^55|2^57
#            = ABS_X|ABS_Y|ABS_MT_SLOT|MT_POSITION_X|MT_POSITION_Y|
#              MT_TOOL_TYPE|MT_TRACKING_ID  -- bit55 就是 ABS_MT_TOOL_TYPE。
#
#  B3  FAILED 计数器写成了 `row PASS ...; FAILED=$((FAILED+1))` ⇒
#      PASS/WARN/SKIP 也 +1，末尾恒 `exit 1`，门禁失效。
#      修：把计数收进 row()，只在 FAIL 时 +1。
#      注：文件里原本写着「函数体内的赋值在 mksh 里不传播」——**这是错的**。
#      2026-10-07 在设备上实测（/system/bin/sh = mksh）：
#          FAILED=0; row(){ if [ "$1" = FAIL ]; then FAILED=$((FAILED+1)); fi; }
#          row PASS; row FAIL; row FAIL; echo $FAILED   ->  2   正常传播
#
# ------------------------------------------------------------------
# 硬件限制（先说清楚，免得白忙）
# ------------------------------------------------------------------
# 面板是无源电容耦合，测不出笔到屏的距离 => 没有 ABS_DISTANCE
# => 悬停预览（hover）在硬件上就不可能，不是软件能补的。
# 压感同理：除非 IC 侧上报真实压力域，否则拿不到压力。
#
# ------------------------------------------------------------------
# 用法
# ------------------------------------------------------------------
#   adb push check-pen-android.sh /data/local/tmp/
#   adb shell sh /data/local/tmp/check-pen-android.sh
#
#   # 带抓事件（需要 root，录 8 秒；期间用笔划两下）
#   adb shell su -c 'sh /data/local/tmp/check-pen-android.sh --capture 8'
#
# 退出码：0 = 无硬阻塞；1 = 有 FAIL 项。

VERSION="check-pen-android/1.1"

# 内核 input_dev->name 的确切值（驱动 probe 里就是这一串，别猜）
DEV_NAME="Himax Capacitive TouchScreen"
# 驱动 algo 的 sysfs 位置：spi 编号随内核版本变过（spi0.0 / spi1.0 都出现过）
ALGO_GLOB="/sys/bus/spi/devices/*/algo"

FAILED=0

say() { echo "$*"; }

# ★ 计数收在这里，调用点不用管。只在 FAIL 时 +1（B3）。
#   实测 mksh 下函数体赋值会传播回调用者，所以这是安全的。
row() {
    # $1=状态 $2=名字 $3=详情
    printf "  [%-8s] %-16s %s\n" "$1" "$2" "$3"
    if [ "$1" = "FAIL" ]; then
        FAILED=$((FAILED + 1))
    fi
    return 0
}

# 取 /proc/bus/input/devices 里某个设备名的**整段**（N: 开头，到下一段 N: 为止）。
proc_block() {
    awk -v n="$1" '
        /^N: Name=/ { f = index($0, n) ? 1 : 0 }
        f
    ' /proc/bus/input/devices 2>/dev/null
}

say "=============================================================="
say "$VERSION"
say "=============================================================="
say "kernel: $(uname -r 2>/dev/null)"
say "time  : $(date 2>/dev/null)"
say ""

# ---------------------------------------------------------------- L0
say "-- L0: driver present, pen path on --"
ALGO=$(ls -d $ALGO_GLOB 2>/dev/null | head -1)
if [ -z "$ALGO" ]; then
    row FAIL L0-driver "no $ALGO_GLOB -- driver not built in"
elif [ ! -e "$ALGO/pen_enabled" ]; then
    row FAIL L0-driver "$ALGO has NO pen_enabled -- module is an old build, rebuild it"
else
    PE=$(cat "$ALGO/pen_enabled" 2>/dev/null)
    if [ "$PE" = "1" ]; then
        row PASS L0-driver "$ALGO, pen_enabled=1 (pen path on)"
    else
        row WARN L0-driver "$ALGO, pen_enabled=$PE (pen path OFF => L3 sees nothing)"
        say "      fix: echo 1 > $ALGO/pen_enabled"
    fi
fi
say ""

# ---------------------------------------------------------------- L1
say "-- L1: how Android classifies the device --"
DS=$(dumpsys input 2>/dev/null)
if [ -z "$DS" ]; then
    row FAIL L1-class "dumpsys input gave no output (not Android? needs root?)"
else
    # ★ B1 修复：取整个 Device 块，不要用 -A N 的窗口
    DBLK=$(printf '%s\n' "$DS" | awk -v n="$DEV_NAME" '
        /^[ \t]*Device [0-9]+:/ { f = index($0, n) ? 1 : 0 }
        f
    ')
    if [ -z "$DBLK" ]; then
        row FAIL L1-class "device name '$DEV_NAME' not found in dumpsys input"
        say "      devices present:"
        printf '%s\n' "$DS" | grep -iE "^ *Device [0-9]+:" | head -8 | sed 's/^/        /'
    else
        # 只打印到 Sources: 那一行为止；整块有上百行（含 Input Controller
        # 的灯列表），全打出来会把真正的结论淹掉。
        printf '%s\n' "$DBLK" | sed -n '1,/^ *Sources:/p' | sed 's/^/      /'
        say ""
        SRC=$(printf '%s\n' "$DBLK" | grep -i "^ *Sources:" | head -1)
        if printf '%s' "$SRC" | grep -qi "stylus"; then
            row PASS L1-class "has SOURCE_STYLUS: $SRC"
        else
            row FAIL L1-class "NO SOURCE_STYLUS => apps will not treat it as a pen"
            say "      => root cause is framework side, not a missing kernel event."
        fi
    fi
fi
say ""

# ---------------------------------------------------------------- L2
say "-- L2: which ABS axes the device registered --"
HXBLK=$(proc_block "$DEV_NAME")
if [ -z "$HXBLK" ]; then
    row WARN L2-abs "device block not found in /proc/bus/input/devices"
else
    # 从这个设备的 Handlers= 里拿事件节点
    EVD=$(printf '%s\n' "$HXBLK" | sed -n 's/^H: Handlers=\(.*\)$/\1/p' \
          | tr ' ' '\n' | grep -x 'event[0-9]*' | head -1)

    # ★ B2 修复：能力位从 getevent -pl 读（/proc 里只有 bitmask、没有轴名字）
    CAPS=""
    if [ -n "$EVD" ] && command -v getevent >/dev/null 2>&1; then
        CAPS=$(getevent -pl "/dev/input/$EVD" 2>/dev/null)
    fi

    if [ -z "$CAPS" ]; then
        row WARN L2-abs "cannot read capabilities (node='${EVD:-?}'); getevent unavailable?"
        say "      /proc/bus/input/devices only carries a bitmask, e.g."
        printf '%s\n' "$HXBLK" | grep '^B: ABS=' | sed 's/^/        /'
        say "      decode bit55 = ABS_MT_TOOL_TYPE yourself if you need a verdict."
    else
        say "      node: /dev/input/${EVD}   (from Handlers=)"
        if printf '%s' "$CAPS" | grep -q "ABS_MT_TOOL_TYPE"; then
            row PASS L2-abs "ABS_MT_TOOL_TYPE registered (pen can be tagged)"
        else
            row FAIL L2-abs "NO ABS_MT_TOOL_TYPE => pen cannot be told from finger"
        fi
        if printf '%s' "$CAPS" | grep -q "ABS_MT_PRESSURE"; then
            row PASS L2-press "ABS_MT_PRESSURE present"
        else
            row WARN L2-press "no ABS_MT_PRESSURE => pressure unavailable to apps"
        fi
        if printf '%s' "$CAPS" | grep -q "ABS_MT_DISTANCE"; then
            row PASS L2-hover "ABS_MT_DISTANCE present (hover possible)"
        else
            row WARN L2-hover "no ABS_MT_DISTANCE => hover IMPOSSIBLE (passive panel)"
        fi
        if printf '%s' "$CAPS" | grep -q "ABS_TILT_X"; then
            row PASS L2-tilt "ABS_TILT_X present (side writing possible)"
        else
            row WARN L2-tilt "no ABS_TILT_X => no tilt / no side-edge writing"
        fi
    fi
fi
say ""

# ---------------------------------------------------------------- L3
say "-- L3: does the live event stream carry MT_TOOL_PEN --"
CAPTURE=0
if [ "$1" = "--capture" ]; then
    CAPTURE=${2:-8}
fi

if [ "$CAPTURE" -gt 0 ]; then
    say "  recording ${CAPTURE}s -- STROKE WITH THE PEN TWICE NOW"
    if command -v getevent >/dev/null 2>&1; then
        TMP=/data/local/tmp/.pen-evt.$$
        getevent -lt /dev/input/event* > "$TMP" 2>/dev/null &
        GPID=$!
        sleep "$CAPTURE"
        kill $GPID 2>/dev/null
        wait $GPID 2>/dev/null

        if [ ! -s "$TMP" ]; then
            row FAIL L3-event "no events captured (wrong node? driver not running?)"
        else
            say "      captured $(wc -l < "$TMP") lines"
            if grep -q "ABS_MT_TOOL_TYPE" "$TMP"; then
                # ★ 只取数字：原来的 `case "$V" in *1)` 会把 "value 0" 也误判
                V=$(grep "ABS_MT_TOOL_TYPE" "$TMP" | sed 's/.*value //' | head -1)
                if [ "$V" = "1" ]; then
                    row PASS L3-event "ABS_MT_TOOL_TYPE = 1 (MT_TOOL_PEN) -- pen is tagged"
                else
                    row FAIL L3-event "ABS_MT_TOOL_TYPE present but value != 1: '$V'"
                fi
            else
                row FAIL L3-event "no ABS_MT_TOOL_TYPE in events => kernel is not tagging the pen"
            fi
            if grep -q "ABS_MT_PRESSURE" "$TMP"; then
                row PASS L3-press "pressure events present"
            else
                row WARN L3-press "no ABS_MT_PRESSURE events"
            fi
            say ""
            say "  raw excerpt (TOOL / PEN / TRACKING_ID):"
            grep -E "TOOL|PEN|TRACKING_ID" "$TMP" | head -15 | sed 's/^/      /'
            rm -f "$TMP"
        fi
    else
        row WARN L3-event "no getevent binary (toybox should have it)"
    fi
else
    row SKIP L3-event "no --capture; run with --capture 8 for the final verdict"
fi
say ""

# ---------------------------------------------------------------- verdict
say "-- verdict --"
if [ "$FAILED" -eq 0 ]; then
    say "  no hard blockers."
    if [ "$CAPTURE" -gt 0 ]; then
        say "  => all layers passed; apps should see TOOL_TYPE_STYLUS."
    else
        say "  => run with --capture 8 to get the final verdict (L3)."
    fi
else
    say "  $FAILED FAIL item(s). Triage in this order:"
    say ""
    say "   1) L2 FAIL + L3 FAIL => kernel patches not applied."
    say "      check: bash tests/verify-patches-pack.sh <tree>"
    say "   2) L2 PASS + L3 FAIL => driver built but pen path off."
    say "      check: cat <algo>/pen_enabled   (expect 1)"
    say "      if that file does not exist => old module, rebuild."
    say "   3) L3 PASS + L1 FAIL => kernel tags the pen but the framework"
    say "      does not classify it. .idc CANNOT fix this: the official"
    say "      touch.deviceType enum has no stylus member."
fi
say ""
say "REMEMBER (AOSP docs, verified 2026-10-05):"
say "  whether this is a pen is decided by the kernel's MT_TOOL_PEN /"
say "  BTN_TOOL_PEN. touch.deviceType only picks touchscreen/touchpad/pointer;"
say "  writing 'stylus' there is a no-op."
say ""
say "HARDWARE LIMIT: the panel is passively coupled and cannot measure"
say "  pen-to-glass distance => no ABS_DISTANCE => hover preview is"
say "  impossible on this hardware, not a software gap."
# ★ 不用 [ $((FAILED > 0)) ] 或 exit $((FAILED > 0))：
#   在 mksh/ksh 系（Android 的 /system/bin/sh 就是 mksh）里
#   算术扩展里的 > 会被当成重定向符，整条表达式退化成 0
#   => 永远 exit 0，门禁失效。显式 if 没有这个歧义。
if [ "$FAILED" -gt 0 ]; then
    exit 1
fi
exit 0
