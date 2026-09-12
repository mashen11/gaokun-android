#!/usr/bin/env bash
# 实测标定 hi846 的模拟增益模型（给 libcamera 的 CameraSensorHelper 用）。
#
#   SER=<序列号> bash scripts/camera/gain-calib.sh [曝光] [步长]
#
# ★ 为什么必须【测】不能查：内核驱动 drivers/media/i2c/hi846.c 的
#   hi846_set_ctrl() 里 V4L2_CID_ANALOGUE_GAIN 分支只是
#   `hi846_write_reg(hi846, HI846_REG_ANALOG_GAIN, ctrl->val)` ——
#   把值原样写进寄存器 0x0077，**没有公式也没有注释**。
#   libcamera 要的是 gain(code) 的解析形式（AnalogueGainLinear 或
#   AnalogueGainExp，见 src/ipa/libipa/camera_sensor_helper.h:35-46）。
#
# ★ 为什么用 libcamera 的 RAW 通路来抓帧：
#   RAW role **不启用软件 ISP，因此没有 AGC**，没人跟我们抢增益控件 ——
#   实测设好 gain=240 跑完回读仍是 240。（ABGR 通路下 AGC 会覆盖。）
#   ⚠️ 但 TestPattern 会被 libcamera 写回 0（见 #90），别指望预设它 ——
#   **不是所有控件都同等对待**。
#   ⚠️★ 更要命的是 **VBLANK**：任何工具在 S_FMT 时都会让驱动把它打回默认
#   （`hi846_set_fmt()` 里的 `__v4l2_ctrl_modify_range(vblank,...)`），
#   于是曝光上限也退回 840 ⇒ **想用长曝光就必须"设完格式之后、开流之前"设**，
#   预设是设不住的（libcamera 与 camtest 实测都会清掉）。
#
# ⚠️ 两条必须遵守的实验纪律：
#  1. ⚠️★ **工作点要【同时】满足两端**：高端不饱和、低端信噪比够。
#     第一轮我只按"别饱和"选，把曝光压到 60 —— 结果 code=0 时信号只比黑电平
#     高约 1.9 计数，分母是噪声，整条比值曲线全废（见 #93）。
#     **认真地只做对一半，和没做一样。**
#     实测标定点：曝光 60/增益 0 时信号≈1.87 ⇒ 曝光 400 时≈12.5，
#     满增益(约12×)后≈150，离饱和(239)有余量 ⇒ **曝光取 400**。
#  2. **首尾各测一次 gain=0**：光源（手电筒/环境光）只要动过，整组数据就废了。
#     两次差异过大就重做。
set -uo pipefail
SER=${SER:?请设置 SER=<adb序列号>}
EXPO=${1:-60}
STEP=${2:-16}
SD=/dev/v4l-subdev44          # hi846 2-0020，用 /sys/class/video4linux/*/name 找
ID_EXPO=0x00980911
ID_AGAIN=0x009e0903
OUT=${OUT:-/tmp/gain-calib.csv}
S() { adb -s "$SER" shell "$@"; }

set_ctl() { S "/data/local/tmp/yavta-static --no-query -w '$1 $2' $SD >/dev/null 2>&1"; }

REPS=${REPS:-3}

one_shot() {  # $1=增益码；打印 "中位数 均值 饱和%"
    set_ctl $ID_EXPO $EXPO
    set_ctl $ID_AGAIN $1
    SER=$SER bash "$(dirname "$0")/lc-run.sh" -r RAW -n 4 -o /data/local/tmp/gc >/dev/null 2>&1
    adb -s "$SER" pull /data/local/tmp/gc-last.SGBRG10_CSI2P /tmp/gc.bin >/dev/null 2>&1
    python3 - <<'PY'
d=open("/tmp/gc.bin","rb").read()
msb=[]
for i in range(0,len(d),5): msb.extend(d[i:i+4])   # CSI2P：每 5 字节 4 像素，取高 8 位
msb.sort(); n=len(msb)
print(f"{msb[n//2]} {sum(msb)/n:.4f} {sum(1 for v in msb if v>=250)/n*100:.3f}")
PY
}

measure() {   # $1 = 增益码；重复 REPS 次取中位数
    local meds="" means="" sats=""
    for _ in $(seq 1 "$REPS"); do
        set -- "$1"
        out=$(one_shot "$1")
        meds="$meds $(echo "$out" | cut -d" " -f1)"
        means="$means $(echo "$out" | cut -d" " -f2)"
        sats="$sats $(echo "$out" | cut -d" " -f3)"
    done
    python3 - "$1" "$meds" "$means" "$sats" <<'PY'
import sys, statistics
code, meds, means, sats = sys.argv[1], sys.argv[2].split(), sys.argv[3].split(), sys.argv[4].split()
m  = statistics.median(list(map(float, meds)))
mn = statistics.median(list(map(float, means)))
st = statistics.median(list(map(float, sats)))
print(f"{code},{m:.1f},{mn:.2f},{st:.3f}," + "|".join(meds))
PY
}

echo "code,median,mean,sat_pct,reps" | tee "$OUT"
for g in $(seq 0 "$STEP" 240); do measure "$g" | tee -a "$OUT"; done
echo "# 收尾对照：再测一次 code=0，与开头那行比" | tee -a "$OUT"
measure 0 | tee -a "$OUT"
echo "→ 结果在 $OUT"
