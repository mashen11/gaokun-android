#!/usr/bin/env python3
"""Find ghost / duplicate contacts in a getevent capture.

Hypothesis under test: a single physical finger touch is sometimes reported
across TWO MT slots at once, so Android sees two pointers -- which would make
a one-finger drag trigger pinch-zoom, and would cancel the recents swipe-up.

Reports, per BTN_TOUCH DOWN..UP gesture:
  * how many ABS_MT_TRACKING_ID opens happened,
  * which slots carried coordinates,
  * the per-frame active-slot count distribution,
  * for every multi-slot frame, the distance between the points (a ghost that
    rides on top of the real touch is < ~60 px away; two real fingers are far).

Usage:  python3 pen-multitouch-analyze.py <capture.txt> [--cols 60] [--xmax 1599]
"""
import argparse
import re
import statistics as st
import sys
from collections import Counter, defaultdict

LINE = re.compile(
    r"\[\s*(?P<t>\d+\.\d+)\]\s+(?:(?P<dev>\S+):\s+)?"
    r"(?P<type>EV_\S+)\s+(?P<code>\S+)\s+(?P<val>\S+)"
)
TOOL_SYM = {"MT_TOOL_FINGER": "finger", "MT_TOOL_PEN": "pen", "MT_TOOL_PALM": "palm"}


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


def build(ev):
    """SYN-bounded frames carrying a full per-slot snapshot.

    slot state is persistent across frames (MT protocol B), so a slot that is
    not touched in a frame still holds its last tracking id and coordinates.
    The live snapshot MUST be taken at SYN_REPORT time -- taking it after the
    event loop yields the *final* (all-closed) state for every frame, which
    silently hides every concurrent-slot frame (maxslots always 0).  That bug
    was real and produced a false "no ghost" result once; do not reintroduce.
    """
    cur = 0
    tid = {}          # slot -> tracking id (None when closed)
    tool = {}         # slot -> tool name
    sx, sy = {}, {}   # slot -> last x / y
    frames = []
    acc = None

    def newacc(t):
        return {"t": t, "btn": None, "opens": [], "closes": [], "touched": set()}

    def snapshot():
        live = {}
        for s, i in tid.items():
            if i is not None and s in sx and s in sy:
                live[s] = (sx[s], sy[s], tool.get(s, "?"), i)
        return live

    for t, code, v in ev:
        if acc is None:
            acc = newacc(t)
        if code in ("SYN_REPORT", "SYN_MT_REPORT"):
            acc["live"] = snapshot()      # state as of this SYN
            frames.append(acc)
            acc = None
            continue
        if code == "ABS_MT_SLOT":
            cur = v
        elif code == "ABS_MT_TOOL_TYPE":
            tool[cur] = TOOL_SYM.get(v, v)
            acc["touched"].add(cur)
        elif code == "ABS_MT_TRACKING_ID":
            if v == 0xFFFFFFFF:
                tid[cur] = None
                acc["closes"].append(cur)
                acc["touched"].add(cur)
            else:
                tid[cur] = v
                acc["opens"].append((cur, v))
                acc["touched"].add(cur)
        elif code == "ABS_MT_POSITION_X":
            sx[cur] = v
            acc["touched"].add(cur)
        elif code == "ABS_MT_POSITION_Y":
            sy[cur] = v
            acc["touched"].add(cur)
        elif code == "BTN_TOUCH":
            acc["btn"] = v
    if acc is not None:
        acc["live"] = snapshot()
        frames.append(acc)
    return frames


def gestures(frames):
    out, cur = [], None
    for i, f in enumerate(frames):
        if f["btn"] == 1 and cur is None:
            cur = {"i0": i, "frames": []}
        if cur is not None:
            cur["frames"].append(f)
            if f["btn"] == 0:
                out.append(cur)
                cur = None
    if cur is not None:
        out.append(cur)
    return out


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--cols", type=int, default=60)
    ap.add_argument("--xmax", type=int, default=1599)
    ap.add_argument("--min-show", dest="min_show", type=int, default=3,
                    help="only show gestures with >= this many open frames")
    args = ap.parse_args(argv[1:])

    ev = parse(args.capture)
    frames = build(ev)
    gs = gestures(frames)

    t0 = ev[0][0]
    multi_total = 0
    multi_close = 0
    ghost_gestures = []
    slot_hist = Counter()
    opens_hist = Counter()
    toolpair_hist = Counter()     # only for <60px multi-slot frames
    toolsolo_hist = Counter()     # tool of the lone slot, for contrast

    for gi, g in enumerate(gs):
        opens = []
        slots = set()
        for f in g["frames"]:
            opens.extend(f["opens"])
            for s in f["live"]:
                slots.add(s)
        ns_max = max((len(f["live"]) for f in g["frames"]), default=0)

        # per-gesture multi-slot frames
        n_ms = 0
        close_ms = 0
        dmin = None
        for f in g["frames"]:
            if len(f["live"]) == 1:
                toolsolo_hist[next(iter(f["live"].values()))[2]] += 1
            if len(f["live"]) >= 2:
                n_ms += 1
                pts = list(f["live"].values())
                fmin = None
                for a in range(len(pts)):
                    for b in range(a + 1, len(pts)):
                        d = ((pts[a][0] - pts[b][0]) ** 2
                             + (pts[a][1] - pts[b][1]) ** 2) ** 0.5
                        if fmin is None or d < fmin:
                            fmin = d
                if dmin is None or fmin < dmin:
                    dmin = fmin
                if fmin is not None and fmin < 60:
                    close_ms += 1
                    tools = tuple(sorted(p[2] for p in pts))
                    toolpair_hist[tools] += 1
        multi_total += n_ms
        multi_close += close_ms
        opens_hist[len(opens)] += 1
        if ns_max > 1:
            slot_hist[ns_max] += 1
        if ns_max > 1 or len(opens) > 2:
            ghost_gestures.append((gi, g, len(opens), ns_max, n_ms, close_ms, dmin))

    print("=" * 72)
    print("ghost / duplicate contact analysis")
    print("=" * 72)
    print("file   : %s" % args.capture)
    print("events : %d   frames: %d   gestures: %d" % (len(ev), len(frames), len(gs)))
    print()
    print("gestures by # of TRACKING_ID opens : %s"
          % dict(sorted(opens_hist.items())))
    print("gestures by max live slots in a frame: %s"
          % dict(sorted(slot_hist.items())))
    print()
    print("frames with >=2 simultaneous live slots : %d" % multi_total)
    print("   of which the two points are < 60 px apart : %d  <== GHOST" % multi_close)
    print()
    print("tool of the LONE slot in single-slot frames : %s"
          % dict(sorted(toolsolo_hist.items())))
    print("tool multiset of <60px multi-slot frames    : %s"
          % {"/".join(k): v for k, v in sorted(toolpair_hist.items())})
    print("   (finger/pen pair => a pen track riding a live finger = GHOST;"
          " finger/finger => two real fingers)")
    print()

    if ghost_gestures:
        print("-- suspicious gestures (>=2 slots or >2 opens) --")
        print("  #   t_rel   opens  maxslots  multi  close  dmin px")
        for gi, g, no, ns, nms, cms, dmin in ghost_gestures:
            tr = g["frames"][0]["t"] - t0
            flag = "  <== GHOST" if cms else ""
            print("  %-3d %7.1f  %5d  %8d  %5d  %5d  %s%s"
                  % (gi, tr, no, ns, nms, cms,
                     "%.0f" % dmin if dmin is not None else "-", flag))
    else:
        print("-- no multi-slot gesture found --")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
