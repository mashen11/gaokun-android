#!/usr/bin/env python3
"""Quantify stroke sampling / fragmentation from a getevent capture, split by
input tool (finger vs pen).

Why gesture-level, not contact-level
------------------------------------
The Himax node speaks MT protocol B and, for a single physical touch, is known
to (a) emit a *stray lead frame* carrying only `ABS_MT_TOOL_TYPE=MT_TOOL_FINGER`
+ SYN just before a pen contact, and (b) intermittently juggle one physical
touch between two slots (e.g. slot0 = MT_TOOL_PEN, slot1 = no tool type, both
tracing the same path).  A naive per-contact classifier therefore (a) inherits
the lead frame's FINGER and never lets the same-frame PEN overwrite it, and
(b) counts the ghost slot as a separate finger contact.

This tool instead:
  * parses SYN-bounded frames,
  * segments the stream into *gestures* on BTN_TOUCH DOWN..UP,
  * classifies a gesture by the strongest tool type seen in it (PEN >
    FINGER > unknown),
  * emits at most ONE position point per frame, which collapses the two-slot
    juggling back into the single physical touch.

Metrics reported per tool: contacts/s, per-hand gesture duration, frame
interval (=> sample rate), per-frame step in pixels and grid cells (vs the
driver's pen_jump_max=4 / pen_max_step=6), and stroke fragmentation.

Usage:
    python3 pen-line-analyze.py <capture.txt> [--cols N] [--xmax N]
"""
import argparse
import re
import statistics as st
import sys
from collections import Counter

LINE = re.compile(
    r"\[\s*(?P<t>\d+\.\d+)\]\s+(?:(?P<dev>\S+):\s+)?"
    r"(?P<type>EV_\S+)\s+(?P<code>\S+)\s+(?P<val>\S+)"
)

TOOL_SYM = {"MT_TOOL_FINGER": "finger", "MT_TOOL_PEN": "pen", "MT_TOOL_PALM": "palm"}
TOOL_RANK = {"pen": 3, "palm": 2, "finger": 1}


def pct(vals, p):
    if not vals:
        return 0.0
    s = sorted(vals)
    k = min(len(s) - 1, max(0, int(round((p / 100.0) * (len(s) - 1)))))
    return s[k]


def parse(path):
    ev = []
    with open(path, errors="replace") as f:
        for ln in f:
            m = LINE.match(ln.strip())
            if not m:
                continue
            code = m.group("code")
            raw = m.group("val")
            if code == "ABS_MT_TOOL_TYPE":
                v = raw
            elif raw in ("DOWN", "UP"):
                v = 1 if raw == "DOWN" else 0
            else:
                try:
                    v = int(raw, 16)
                except ValueError:
                    continue
            ev.append((float(m.group("t")), code, v))
    return ev


def build_frames(ev):
    """SYN-bounded frames.  Each frame:
       {t, xy:[(slot,x,y)...], tools:[(slot,name)...], opens:[(slot,id)...],
        closes:[slot...], btn:None|0|1, touch_down:bool}
    x/y carried over per slot so an X-only or Y-only update still yields a
    complete point."""
    slot_x, slot_y = {}, {}
    cur = 0
    frames = []
    acc = None

    def newacc(t):
        return {"t": t, "xy": [], "tools": [], "opens": [], "closes": [],
                "btn": None}

    for t, code, v in ev:
        if code in ("SYN_REPORT", "SYN_MT_REPORT"):
            if acc is not None:
                frames.append(acc)
                acc = None
            continue
        if acc is None:
            acc = newacc(t)
        if code == "ABS_MT_SLOT":
            cur = v
        elif code == "ABS_MT_TOOL_TYPE":
            acc["tools"].append((cur, TOOL_SYM.get(v, v)))
        elif code == "ABS_MT_TRACKING_ID":
            if v == 0xFFFFFFFF:
                acc["closes"].append(cur)
            else:
                acc["opens"].append((cur, v))
        elif code == "ABS_MT_POSITION_X":
            slot_x[cur] = v
            if v not in [p[0] for p in acc["xy"]]:
                pass  # placeholder; assembled at finalize
            acc.setdefault("_upd", set()).add(cur)
        elif code == "ABS_MT_POSITION_Y":
            slot_y[cur] = v
            acc.setdefault("_upd", set()).add(cur)
        elif code == "BTN_TOUCH":
            acc["btn"] = v
        # finalize xy for updated slots
        if acc is not None and "_upd" in acc:
            acc["xy"] = [(s, slot_x.get(s), slot_y.get(s))
                         for s in sorted(acc["_upd"])
                         if s in slot_x and s in slot_y]
    if acc is not None:
        frames.append(acc)
    for f in frames:
        f.pop("_upd", None)
    return frames


def gestures(frames):
    """Segment into BTN_TOUCH DOWN..UP gestures."""
    out = []
    cur = None
    for i, f in enumerate(frames):
        if f["btn"] == 1 and cur is None:
            cur = {"i0": i, "frames": [], "tools": []}
        if cur is not None:
            cur["frames"].append(f)
            cur["tools"].extend(name for _, name in f["tools"])
            if f["btn"] == 0:
                out.append(cur)
                cur = None
    if cur is not None:
        out.append(cur)
    return out


def classify(g):
    """Strongest tool type seen in the gesture."""
    c = Counter(g["tools"])
    for name in ("pen", "palm", "finger"):
        if c.get(name):
            return name
    return "unknown"


def points_of(g):
    """One (t,x,y) per frame (first updated slot), collapsing 2-slot juggling."""
    pts = []
    for f in g["frames"]:
        if not f["xy"]:
            continue
        s, x, y = f["xy"][0]
        if x is None or y is None:
            continue
        pts.append((f["t"], x, y))
    return pts


def analyze(name, gs, cols, xmax, max_show):
    if not gs:
        print("-- %s --  no gestures" % name)
        return
    durs = []
    all_gaps, all_steps, rates = [], [], []
    strokes = []
    for g in gs:
        pts = points_of(g)
        if len(pts) < 2:
            continue
        dur = pts[-1][0] - pts[0][0]
        if dur <= 0:
            continue
        durs.append(dur * 1000.0)
        strokes.append((g, pts))
        rates.append(len(pts) / dur)
        for j in range(1, len(pts)):
            all_gaps.append((pts[j][0] - pts[j - 1][0]) * 1000.0)
            dx = pts[j][1] - pts[j - 1][1]
            dy = pts[j][2] - pts[j - 1][2]
            all_steps.append((dx * dx + dy * dy) ** 0.5)

    print("-- tool=%s --  gestures %d   drawable strokes %d"
          % (name, len(gs), len(strokes)))
    if durs:
        print("  stroke duration ms : min %.0f  median %.0f  max %.0f"
              % (min(durs), st.median(durs), max(durs)))
    if all_gaps:
        med = st.median(all_gaps)
        print("  frame interval ms  : median %.2f  p90 %.2f  max %.2f   => %.1f Hz"
              % (med, pct(all_gaps, 90), max(all_gaps), 1000.0 / med))
    if rates:
        print("  points/s in stroke : median %.1f" % st.median(rates))
    if all_steps:
        print("  step px            : median %.0f  p90 %.0f  p99 %.0f  max %.0f"
              % (st.median(all_steps), pct(all_steps, 90), pct(all_steps, 99),
                 max(all_steps)))
        if cols and xmax:
            cell = xmax / float(cols - 1)
            cs = [s / cell for s in all_steps]
            print("  step cells         : median %.2f  p90 %.2f  max %.2f  (1 cell=%.1f px)"
                  % (st.median(cs), pct(cs, 90), max(cs), cell))
            over = sum(1 for s in cs if s > 4)
            over6 = sum(1 for s in cs if s > 6)
            print("  > pen_jump_max(4c) : %d / %d (%.1f%%)   > pen_max_step(6c): %d / %d"
                  % (over, len(cs), 100.0 * over / len(cs), over6, len(cs)))
    print()
    for g, pts in sorted(strokes, key=lambda k: len(k[1]), reverse=True)[:max_show]:
        gaps = [pts[j][0] - pts[j - 1][0] for j in range(1, len(pts))]
        gm = [x * 1000.0 for x in gaps if x > 0]
        steps = []
        for j in range(1, len(pts)):
            dx = pts[j][1] - pts[j - 1][1]
            dy = pts[j][2] - pts[j - 1][2]
            steps.append((dx * dx + dy * dy) ** 0.5)
        print("    %-8s dur %6.0f ms  pts %3d  gap %5.1f ms  step med %3.0f max %3.0f px"
              % (classify(g), (pts[-1][0] - pts[0][0]) * 1000.0, len(pts),
                 st.median(gm) if gm else 0, st.median(steps) if steps else 0,
                 max(steps) if steps else 0))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--cols", type=int, default=0)
    ap.add_argument("--xmax", type=int, default=0)
    ap.add_argument("--max-contacts", dest="max_show", type=int, default=5)
    ap.add_argument("--timeline", action="store_true",
                    help="print each gesture's start offset / tool / length")
    args = ap.parse_args(argv[1:])

    ev = parse(args.capture)
    if not ev:
        print("no parseable events in %s" % args.capture)
        return 1
    frames = build_frames(ev)
    gs = gestures(frames)

    t0, t1 = ev[0][0], ev[-1][0]
    print("=" * 70)
    print("stroke sampling / fragmentation analysis   (gesture-level, by tool)")
    print("=" * 70)
    print("file     : %s" % args.capture)
    print("events   : %d   frames: %d" % (len(ev), len(frames)))
    print("window   : %.2f s  (t %.3f -> %.3f)" % (t1 - t0, t0, t1))
    by_tool = Counter(classify(g) for g in gs)
    print("gestures : %d  ->  %s" % (
        len(gs), ", ".join("%s %d" % (k, v) for k, v in sorted(by_tool.items()))))
    print()

    if args.timeline:
        print("-- timeline (offset s | tool | pts | dur ms) --")
        for g in gs:
            pts = points_of(g)
            t_start = g["frames"][0]["t"] - t0
            dur = (pts[-1][0] - pts[0][0]) * 1000.0 if len(pts) >= 2 else 0.0
            print("  %7.1f | %-8s | %3d | %6.0f" % (t_start, classify(g), len(pts), dur))
        print()

    for tool in ("finger", "pen", "palm", "unknown"):
        sub = [g for g in gs if classify(g) == tool]
        if sub:
            analyze(tool, sub, args.cols, args.xmax, args.max_show)
    print("-- combined (all tools) --")
    analyze("all", gs, args.cols, args.xmax, 0)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
