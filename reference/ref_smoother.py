#!/usr/bin/env python3
"""
CPU reference for the Grouwdi-OBS-FrameGen motion pipeline.

Mirrors what the GPU filter does, so the algorithm can be measured exactly:
  1. each OBS tick, a frame is sampled from the capture source
  2. repeats are detected from a small thumbnail
  3. optical flow (forward + backward) between consecutive distinct frames
  4. global camera motion -> scalar "content position"
  5. retiming engine picks, for output tick k = t - L, the content position the
     motion SHOULD be at (local fit) and which two frames + blend factor give it
  6. bidirectional flow warp renders that in-between frame

Synthetic footage has a known true camera position for every frame, so the
smoothness of the output can be measured directly (step irregularity).
"""
import math
import sys

import cv2
import numpy as np

# ---------------- engine constants (the C port must match) ----------------
LOOKAHEAD = 4  # output delay in ticks
HISTORY = 12  # ticks of history used for the fit
MIN_SPEED = 0.6  # px/tick (flow resolution) below this = not a pan, pass through
MAX_RESID = 0.45  # fit RMS residual allowed, as a fraction of speed
MAX_BRACKET_DIST = 3  # bracket frames must be within this many ticks of k
DUP_THRESH = 0.1  # thumbnail mean abs diff below this = repeated frame (repeats are identical)
MIN_FRAMES = 5  # distinct frames needed in the window
FILL_ONLY = False  # only replace repeated frames; never retime real frames
CAD_MIN_TPF = 1.0  # cadence mode only when frames arrive no faster than 1 per tick
CAD_MAX_TPF = 3.0  # ... and no slower than 1 per 3 ticks
CAD_MAX_RESID = 1.2  # fit RMS residual allowed, in ticks (tuned on a real trace: bursty delivery)
CUT_CONSISTENCY = 0.25  # scene cut: under this fraction of motion vectors agree forward vs backward
CUT_NORMAL_MIN = 0.5  # ... while normally at least this fraction agree


def fb_consistency(f01, f10, tol=1.0):
    """
    Fraction of the center region where forward and backward motion agree:
    f01(p) + f10(p + f01(p)) ~ 0. Real motion of any kind (pans, depth, objects)
    stays consistent; across a scene cut the estimates are meaningless and don't.
    """
    h, w = f01.shape[:2]
    ys, xs = np.mgrid[int(h * 0.2):int(h * 0.8), int(w * 0.2):int(w * 0.8)]
    fx = f01[ys, xs, 0]
    fy = f01[ys, xs, 1]
    tx = np.clip(np.rint(xs + fx).astype(int), 0, w - 1)
    ty = np.clip(np.rint(ys + fy).astype(int), 0, h - 1)
    ex = fx + f10[ty, tx, 0]
    ey = fy + f10[ty, tx, 1]
    return float(np.mean(ex * ex + ey * ey < tol * tol))


class CutDetector:
    """Scene cut = forward/backward motion suddenly stops agreeing."""

    def __init__(self):
        self.hist = []

    def check(self, consistency):
        normal = float(np.median(self.hist)) if len(self.hist) >= 3 else None
        cut = normal is not None and normal >= CUT_NORMAL_MIN and consistency < CUT_CONSISTENCY
        if not cut:
            self.hist = (self.hist + [consistency])[-15:]
        return cut


class Engine:
    """Retiming engine. Works in ticks and 'content position' units."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.frames = []  # list of dict(tick, pos, id)
        self.ticks = {}  # tick -> id of frame visible at that tick
        self.dirx = 0.0
        self.diry = 0.0
        self.break_tick = -10**9
        self.last_target = None
        self.last_cad = None
        self.seq = 0  # running count of distinct frames (cadence mode)
        self.pan_break_tick = -10**9  # pan mode only: direction reversal / unreliable motion

    def push(self, tick, new_content, gx, gy, confident, cut=False):
        """
        Register the sample for tick. gx,gy = motion from previous distinct frame.
        confident: the motion is one uniform camera move (pan mode may use it).
        cut: the picture changed completely (scene cut): reset everything.
        """
        if not new_content and self.frames:
            self.ticks[tick] = self.frames[-1]["id"]
            return
        self.seq += 1
        if not self.frames:
            self.frames.append({"tick": tick, "pos": 0.0, "id": tick, "seq": self.seq})
            self.ticks[tick] = tick
            return
        if cut:
            self.break_tick = tick
            self.pan_break_tick = tick
            self.last_target = None
            self.last_cad = None
        if not confident:
            # not a uniform pan: pan mode stands down, timing mode keeps working
            self.pan_break_tick = tick
            self.last_target = None
            gx = gy = 0.0
        mag = math.hypot(gx, gy)
        if mag > 1e-6:
            # direction: running average of motion vectors
            if self.dirx * gx + self.diry * gy < -0.5 * mag * math.hypot(self.dirx, self.diry):
                # direction reversed: start a new pan segment
                self.pan_break_tick = tick
                self.last_target = None
                self.dirx, self.diry = gx, gy
            else:
                self.dirx = 0.8 * self.dirx + 0.2 * gx
                self.diry = 0.8 * self.diry + 0.2 * gy
        dn = math.hypot(self.dirx, self.diry)
        step = (gx * self.dirx + gy * self.diry) / dn if dn > 1e-9 else 0.0
        pos = self.frames[-1]["pos"] + step
        self.frames.append({"tick": tick, "pos": pos, "id": tick, "seq": self.seq})
        self.ticks[tick] = tick
        # forget old history
        cutoff = tick - HISTORY - LOOKAHEAD - 4
        self.frames = [f for f in self.frames if f["tick"] >= cutoff]
        for t in [t for t in self.ticks if t < cutoff]:
            del self.ticks[t]

    def decide(self, k, t):
        """
        Output for tick k (t = newest tick pushed).
        Returns ('pass', frame_id) or ('interp', id_a, id_b, alpha).
        """
        visible = self.ticks.get(k)
        if visible is None:
            return None
        if FILL_ONLY:
            return self._repeat_fill(k, visible)
        lo = max(k - HISTORY, self.break_tick)
        win = [f for f in self.frames if lo <= f["tick"] <= t]
        lo_pan = max(lo, self.pan_break_tick)
        pan_win = [f for f in win if f["tick"] >= lo_pan]
        fitted = self._fit(pan_win, k)
        if fitted is not None:
            target = fitted
            if self.last_target is not None and target < self.last_target:
                target = self.last_target
            br = self._bracket(pan_win, target, k)
            if br is not None:
                self.last_target = target
                return br
        self.last_target = None
        cad = self._cadence(win, k)
        if cad is not None:
            return cad
        self.last_cad = None
        return self._repeat_fill(k, visible)

    def _cadence(self, win, k):
        """
        Timing-based retiming for any motion: when new frames arrive slower than
        one per tick (e.g. 40-50 fps content in a 60 fps stream), fit when each
        frame should have appeared (evenly spaced) and show the point in between
        two frames that matches output tick k.
        """
        if len(win) < MIN_FRAMES:
            return None
        seqs = np.array([f["seq"] for f in win], dtype=float)
        ts = np.array([f["tick"] - k for f in win], dtype=float)
        if np.any(np.diff(seqs) != 1):
            return None  # a frame is missing from the window
        w = np.maximum(1.0, (HISTORY + 1) - np.abs(ts))
        sw = w.sum()
        ms = (w * seqs).sum() / sw
        mt = (w * ts).sum() / sw
        var = (w * (seqs - ms) ** 2).sum()
        if var <= 0:
            return None
        d = (w * (seqs - ms) * (ts - mt)).sum() / var  # ticks per frame
        if not (CAD_MIN_TPF <= d <= CAD_MAX_TPF):
            return None
        c = mt - d * ms
        fit = c + d * seqs  # where each frame "should" be, in ticks relative to k
        rms = math.sqrt((w * (ts - fit) ** 2).sum() / sw)
        if rms > CAD_MAX_RESID:
            return None
        u = (0.0 - c) / d  # fractional frame number that belongs at tick k
        if self.last_cad is not None and u < self.last_cad:
            u = self.last_cad
        for f0, f1 in zip(win, win[1:]):
            if f0["seq"] <= u <= f1["seq"]:
                if abs(f0["tick"] - k) > MAX_BRACKET_DIST or abs(f1["tick"] - k) > MAX_BRACKET_DIST:
                    return None
                # remember u in absolute frame numbers for the no-going-back guard
                self.last_cad = u  # output never goes back in time
                alpha = u - f0["seq"]
                if alpha < 0.03:
                    return ("pass", f0["id"])
                if alpha > 0.97:
                    return ("pass", f1["id"])
                return ("interp", f0["id"], f1["id"], alpha)
        return None

    def _fit(self, win, k):
        if len(win) < MIN_FRAMES:
            return None
        ts = np.array([f["tick"] - k for f in win], dtype=float)
        ps = np.array([f["pos"] for f in win], dtype=float)
        w = np.maximum(1.0, (HISTORY + 1) - np.abs(ts))  # triangular weights around k
        sw = w.sum()
        mt = (w * ts).sum() / sw
        mp = (w * ps).sum() / sw
        var = (w * (ts - mt) ** 2).sum()
        if var <= 0:
            return None
        b = (w * (ts - mt) * (ps - mp)).sum() / var
        a = mp - b * mt
        if b < MIN_SPEED:
            return None
        resid = ps - (a + b * ts)
        rms = math.sqrt((w * resid ** 2).sum() / sw)
        if rms > MAX_RESID * b:
            return None
        return a  # fitted position at tick k

    def _bracket(self, win, target, k):
        for f0, f1 in zip(win, win[1:]):
            if f0["pos"] <= target <= f1["pos"] and f1["pos"] > f0["pos"]:
                if abs(f0["tick"] - k) > MAX_BRACKET_DIST or abs(f1["tick"] - k) > MAX_BRACKET_DIST:
                    return None
                alpha = (target - f0["pos"]) / (f1["pos"] - f0["pos"])
                if alpha < 0.03:
                    return ("pass", f0["id"])
                if alpha > 0.97:
                    return ("pass", f1["id"])
                return ("interp", f0["id"], f1["id"], alpha)
        return None

    def _repeat_fill(self, k, visible):
        """Not a steady pan: only fill a repeated frame that sits between two distinct ones."""
        ids = [f["id"] for f in self.frames]
        if visible != k and visible in ids:  # tick k shows an older frame -> repeat
            i = ids.index(visible)
            if i + 1 < len(self.frames):
                a, b = self.frames[i], self.frames[i + 1]
                if b["tick"] - a["tick"] <= 3:
                    alpha = (k - a["tick"]) / (b["tick"] - a["tick"])
                    return ("interp", a["id"], b["id"], alpha)
        return ("pass", visible)


# ---------------- image side ----------------
FLOW_W = 320  # flow computed at this width (GPU uses 1280 with grid 4 -> 320 vectors)
THUMB = (128, 72)
DIS = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)


def gray_small(img):
    h, w = img.shape[:2]
    fh = int(round(h * FLOW_W / w))
    g = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY) if img.ndim == 3 else img
    return cv2.resize(g, (FLOW_W, fh), interpolation=cv2.INTER_AREA)


def flow(a, b):
    """Dense flow such that a(p) ~ b(p + f(p))."""
    return DIS.calc(a, b, None)


def global_motion(f):
    h, w = f.shape[:2]
    c = f[int(h * 0.2):int(h * 0.8), int(w * 0.2):int(w * 0.8)].reshape(-1, 2)
    med = np.median(c, axis=0)
    ok = np.mean(np.hypot(c[:, 0] - med[0], c[:, 1] - med[1]) < 1.0)
    return float(med[0]), float(med[1]), ok


def warp(img, fx, fy):
    """Sample img at p + (fx, fy) (full-res pixel offsets)."""
    h, w = img.shape[:2]
    gx, gy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    return cv2.remap(img, gx + fx, gy + fy, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)


def interpolate(i0, i1, f01, f10, alpha):
    """
    Bidirectional flow interpolation (same math as the GPU shader).
    f01: flow from frame 0 to 1 (i0(p) ~ i1(p + f01)), f10 the reverse.
    Flow at time alpha, approximated from both directions:
      F_a0 = -(1-a)*a*F01 + a^2*F10
      F_a1 = (1-a)^2*F01 - a*(1-a)*F10
    """
    h, w = i0.shape[:2]
    sx = w / f01.shape[1]
    sy = h / f01.shape[0]
    up01 = cv2.resize(f01, (w, h), interpolation=cv2.INTER_LINEAR) * np.array([sx, sy], np.float32)
    up10 = cv2.resize(f10, (w, h), interpolation=cv2.INTER_LINEAR) * np.array([sx, sy], np.float32)
    a = float(alpha)
    fa0 = -(1 - a) * a * up01 + a * a * up10
    fa1 = (1 - a) ** 2 * up01 - a * (1 - a) * up10
    s0 = warp(i0, fa0[..., 0], fa0[..., 1]).astype(np.float32)
    s1 = warp(i1, fa1[..., 0], fa1[..., 1]).astype(np.float32)
    out = (1 - a) * s0 + a * s1
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


# ---------------- synthetic test footage ----------------
def make_texture(w, h, seed=1):
    rng = np.random.default_rng(seed)
    t = rng.integers(0, 256, (h, w), dtype=np.uint8)
    t = cv2.GaussianBlur(t, (0, 0), 2.0)
    t = cv2.normalize(t, None, 0, 255, cv2.NORM_MINMAX)
    return t


class Scene:
    """A camera panning over a texture. pos(time) is known exactly."""

    def __init__(self, w=640, h=360, speed=300.0, wobble=0.0, seed=1):
        self.w, self.h = w, h
        self.tex = make_texture(w * 14, h, seed)
        self.speed, self.wobble = speed, wobble

    def pos(self, t):
        # px at time t; wobble varies speed smoothly (human-like pan)
        return self.speed * t + self.wobble * math.sin(t * 1.3) * 40.0

    def render(self, x):
        m = np.float32([[1, 0, -(x % (self.tex.shape[1] - self.w - 2))], [0, 1, 0]])
        return cv2.warpAffine(self.tex, m, (self.w, self.h), flags=cv2.INTER_LINEAR)


def content_times(game_fps, seconds, jitter=0.0, seed=2):
    """Times at which the game produced frames (uncapped -> jittered)."""
    rng = np.random.default_rng(seed)
    t, out = 0.0, []
    while t < seconds + 1:
        out.append(t)
        dt = 1.0 / game_fps
        if jitter:
            dt *= 1.0 + rng.uniform(-jitter, jitter)
        t += dt
    return np.array(out)


def capture_sequence(scene, game_fps, seconds, card_fps=60.0, obs_fps=60.0, jitter=0.0):
    """
    What OBS sees at each tick: the game frame on screen when the card sampled.
    Returns list of (true_position, frame_key) per OBS tick.
    """
    ct = content_times(game_fps, seconds, jitter)
    ticks = []
    for k in range(int(seconds * obs_fps)):
        t_tick = k / obs_fps
        # latest card frame at or before this tick
        card_idx = math.floor(t_tick * card_fps + 1e-9)
        t_card = card_idx / card_fps
        # latest game frame at or before the card sample
        gi = int(np.searchsorted(ct, t_card + 1e-9) - 1)
        ticks.append((scene.pos(ct[gi]), gi))
    return ticks


def run(name, game_fps, seconds=10, card_fps=60.0, jitter=0.0, wobble=0.0, render=False, out_path=None):
    scene = Scene(wobble=wobble)
    seq = capture_sequence(scene, game_fps, seconds, card_fps, 60.0, jitter)
    eng = Engine()
    cd = CutDetector()
    frames, smalls, poss = {}, {}, {}
    flows = {}  # (id_a, id_b) -> (f01, f10)
    prev_thumb, prev_id = None, None
    out_pos_in, out_pos_out = [], []
    writer = None
    if render and out_path:
        writer = cv2.VideoWriter(out_path, cv2.VideoWriter_fourcc(*"MJPG"), 60, (scene.w, scene.h), False)

    sign = 0  # flow sign calibration (+1 / -1), decided from data like the GPU path
    for t, (true_x, key) in enumerate(seq):
        img = scene.render(true_x)
        th = cv2.resize(img, THUMB, interpolation=cv2.INTER_AREA).astype(np.int16)
        diff = 0.0 if prev_thumb is None else float(np.abs(th - prev_thumb).mean())
        new = prev_thumb is None or diff >= DUP_THRESH
        gx = gy = 0.0
        conf = True
        cut = False
        if new:
            frames[t], poss[t] = img, true_x
            smalls[t] = gray_small(img)
            if prev_id is not None:
                f01 = flow(smalls[prev_id], smalls[t])
                f10 = flow(smalls[t], smalls[prev_id])
                flows[(prev_id, t)] = (f01, f10)
                mx, my, ok = global_motion(f01)
                conf = ok > 0.5
                cut = cd.check(fb_consistency(f01, f10))
                gx, gy = mx, my
            prev_id = t
        prev_thumb = th
        eng.push(t, new, gx, gy, conf, cut)

        k = t - LOOKAHEAD
        if k < 0:
            continue
        d = eng.decide(k, t)
        # true content position shown at k without smoothing
        out_pos_in.append(seq[k][0])
        if d[0] == "pass":
            out_pos_out.append(poss[d[1]])
            frame = frames[d[1]]
        else:
            _, a, b, al = d
            out_pos_out.append((1 - al) * poss[a] + al * poss[b])
            if writer is not None:
                f01, f10 = flows[(a, b)]
                frame = interpolate(frames[a], frames[b], f01, f10, al)
            else:
                frame = None
        if writer is not None:
            writer.write(frame)
        # drop old images
        for old in [i for i in frames if i < k - HISTORY - 8]:
            frames.pop(old)
            smalls.pop(old)
            poss.pop(old)
    if writer is not None:
        writer.release()

    def irregularity(p):
        s = np.diff(np.array(p[30:]))
        # compare each step with the local average speed (removes smooth speed changes)
        ker = np.ones(9) / 9
        local = np.convolve(s, ker, mode="same")
        dev = np.abs(s - local)[8:-8]
        return float(np.mean(dev) / np.mean(np.abs(local[8:-8]))), float(np.max(dev) / np.mean(np.abs(local[8:-8])))

    mi, xi = irregularity(out_pos_in)
    mo, xo = irregularity(out_pos_out)
    print(f"{name:34s} input  irregularity mean {mi*100:5.1f}%  worst {xi*100:5.1f}%   ->   "
          f"output mean {mo*100:5.1f}%  worst {xo*100:5.1f}%")
    return mi, xi, mo, xo


if __name__ == "__main__":
    cases = [
        ("clean 60 into 60", 60, {}),
        ("game 59 into 60 (repeat/s)", 59, {}),
        ("game 119 into 60 (short/s)", 119, {}),
        ("game 121 into 60 (long/s)", 121, {}),
        ("game 144 into 60", 144, {}),
        ("uncapped ~165 jittery into 60", 165, {"jitter": 0.25}),
        ("game 119 + human pan wobble", 119, {"wobble": 1.0}),
        ("card 59 into OBS 60 (re-show/s)", 60, {"card_fps": 59.0}),
    ]
    for name, fps, kw in cases:
        run(name, fps, **kw)
