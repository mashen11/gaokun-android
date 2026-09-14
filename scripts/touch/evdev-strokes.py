#!/usr/bin/env python3
"""解码 cat /dev/input/eventN 录下的原始 evdev 流，把触摸手感量化。

为什么不用 getevent：CLAUDE.md 记过，`timeout N getevent > file` 会因块缓冲丢光输出。
录二进制、离线解码是本仓既定方法（#26）。

aarch64 的 struct input_event = timeval(16) + type(2) + code(2) + value(4) = 24 字节。
"""
import struct, sys, math

EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID = 0x2f, 0x35, 0x36, 0x39

# 单位 -> 毫米：X 轴 1600 单位横跨 167 mm，Y 轴 2560 单位横跨 267 mm（两者都是 0.1043 mm/单位）
MM_PER_UNIT = 0.1043

def decode(path):
    data = open(path, 'rb').read()
    n = len(data) // 24
    slot, slots = 0, {}          # slot -> dict(id, x, y)
    strokes = {}                 # tracking_id -> list[(t, x, y)]
    order = []
    frames = 0
    for i in range(n):
        sec, usec, typ, code, val = struct.unpack_from('<qqHHi', data, i*24)
        t = sec + usec/1e6
        if typ == EV_SYN:
            frames += 1
            for s in slots.values():
                if s['id'] is not None and s['x'] is not None and s['y'] is not None:
                    strokes[s['id']].append((t, s['x'], s['y']))
            continue
        if typ != EV_ABS:
            continue
        if code == ABS_MT_SLOT:
            slot = val
        else:
            s = slots.setdefault(slot, {'id': None, 'x': None, 'y': None})
            if code == ABS_MT_TRACKING_ID:
                if val == -1:
                    s['id'] = None
                else:
                    s['id'] = val
                    if val not in strokes:
                        strokes[val] = []
                        order.append(val)
            elif code == ABS_MT_POSITION_X: s['x'] = val
            elif code == ABS_MT_POSITION_Y: s['y'] = val
    return frames, [(tid, strokes[tid]) for tid in order if strokes.get(tid)]

def report(path):
    frames, strokes = decode(path)
    print(f"{path}: {frames} 个 SYN 帧, {len(strokes)} 条轨迹")
    if not strokes:
        print("  （没有任何触摸事件）")
        return
    for tid, pts in strokes:
        if len(pts) < 2:
            print(f"  id={tid:<6} 只有 {len(pts)} 帧 —— 点按或碎片")
            continue
        dur = pts[-1][0] - pts[0][0]
        dist = sum(math.hypot(pts[k+1][1]-pts[k][1], pts[k+1][2]-pts[k][2])
                   for k in range(len(pts)-1)) * MM_PER_UNIT
        # 逐帧速度
        vs = []
        for k in range(len(pts)-1):
            dt = pts[k+1][0] - pts[k][0]
            if dt <= 0: continue
            d = math.hypot(pts[k+1][1]-pts[k][1], pts[k+1][2]-pts[k][2]) * MM_PER_UNIT
            vs.append(d/dt/1000.0)          # m/s
        # 帧间隔：> 2 倍中位数算一次“断帧”
        gaps = [pts[k+1][0]-pts[k][0] for k in range(len(pts)-1)]
        med = sorted(gaps)[len(gaps)//2] if gaps else 0
        stalls = sum(1 for g in gaps if med > 0 and g > 2.5*med)
        # 静止帧：位移为 0（fuzz 死区的直接指纹）
        frozen = sum(1 for k in range(len(pts)-1)
                     if pts[k+1][1] == pts[k][1] and pts[k+1][2] == pts[k][2])
        print(f"  id={tid:<6} {len(pts):>4} 帧 {dur*1000:>6.0f} ms "
              f"路径 {dist:>6.1f} mm  峰值 {max(vs) if vs else 0:>4.2f} m/s "
              f"中位帧距 {med*1000:>4.1f} ms  断帧 {stalls}  零位移帧 {frozen}"
              f" ({100*frozen/max(1,len(pts)-1):.0f}%)")
    allv = []
    for _, pts in strokes:
        for k in range(len(pts)-1):
            dt = pts[k+1][0]-pts[k][0]
            if dt > 0:
                allv.append(math.hypot(pts[k+1][1]-pts[k][1], pts[k+1][2]-pts[k][2])*MM_PER_UNIT/dt/1000)
    if allv:
        allv.sort()
        print(f"  >>> 全部帧速度：中位 {allv[len(allv)//2]:.2f} m/s  "
              f"p95 {allv[int(len(allv)*0.95)]:.2f} m/s  峰值 {allv[-1]:.2f} m/s  "
              f"超过 1.00 m/s 的帧 {100*sum(1 for v in allv if v>1.0)/len(allv):.0f}%")

if __name__ == '__main__':
    for p in sys.argv[1:]:
        report(p)
