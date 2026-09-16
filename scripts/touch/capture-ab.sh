#!/system/bin/sh
# 事件驱动的 A/B 采集：不按时间切，按【收到多少触摸数据】切。用户什么时候开始滑都行。
echo $$ > /data/local/tmp/tsab.pid
A=/sys/bus/spi/devices/spi0.0/algo
# ★ 节点号会变（2026-09-16 一次重启后 event7 变成了键盘，触摸挪到 event8）——
#   按名字找，别写死。
EV=$(grep -A8 'Name="Himax' /proc/bus/input/devices | grep -o 'event[0-9]*' | head -1)
[ -n "$EV" ] || { echo '找不到 Himax 触摸节点' >&2; exit 1; }
DEV=/dev/input/$EV
NEED=120000          # 每段要收够的字节数（约 7 秒实际触摸）
LIMIT=240            # 每段最多等 240 秒
set_preset() { echo "$1" > $A/track_jump_dist2; echo "$2" > $A/track_smoothing; echo "$3" > $A/track_start_debounce; }

phase() {   # $1=标签 $2=jump $3=smooth $4=deb
    set_preset "$2" "$3" "$4"
    f=/data/local/tmp/ts_$1.bin
    rm -f "$f"; : > "$f"
    cat "$DEV" > "$f" &
    CAT=$!
    i=0
    while [ $i -lt $LIMIT ]; do
        sz=$(stat -c %s "$f" 2>/dev/null || echo 0)
        [ "$sz" -ge "$NEED" ] && break
        echo "$1 $sz" > /data/local/tmp/tsab.progress
        sleep 1; i=$((i+1))
    done
    kill $CAT 2>/dev/null
    echo "$1 done $(stat -c %s "$f")" >> /data/local/tmp/tsab.log
}

rm -f /data/local/tmp/tsab.log /data/local/tmp/tsab.done
phase A 6400 0 0          # 出厂 game 预设
echo "SWITCHED" > /data/local/tmp/tsab.progress
sleep 2
phase B 0 0 2             # 修正预设
echo DONE > /data/local/tmp/tsab.done
