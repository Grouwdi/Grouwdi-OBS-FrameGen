import sys, math, numpy as np, cv2
sys.path.insert(0, '.')
import ref_smoother as R

class EdgeScene(R.Scene):
    def __init__(self, posfn, cut_at=None, sprite=False):
        super().__init__()
        self.posfn = posfn; self.cut_at = cut_at; self.sprite = sprite
        self.tex2 = R.make_texture(self.w * 14, self.h, seed=7)
        self.t_now = 0.0
    def pos(self, t):
        self.t_now = t
        return self.posfn(t)
    def render_t(self, x, t):
        tex = self.tex2 if (self.cut_at is not None and t >= self.cut_at) else self.tex
        m = np.float32([[1, 0, -(x % (tex.shape[1] - self.w - 2))], [0, 1, 0]])
        img = cv2.warpAffine(tex, m, (self.w, self.h), flags=cv2.INTER_LINEAR)
        if self.sprite:
            sx = int(100 + 300 * t) % (self.w - 60)
            cv2.rectangle(img, (sx, 150), (sx + 50, 200), 255, -1)
        return img

def run_edge(name, scene, game_fps=119, seconds=6):
    ct = R.content_times(game_fps, seconds)
    eng = R.Engine(); cd = R.CutDetector(); poss = {}; times = {}; smalls = {}
    prev_thumb = prev_id = None
    shown_in, shown_out, crossings, interp_n = [], [], 0, 0
    seq = []
    for k in range(int(seconds * 60)):
        gi = int(np.searchsorted(ct, k / 60 + 1e-9) - 1)
        seq.append(ct[gi])
    for t, tc in enumerate(seq):
        x = scene.posfn(tc); img = scene.render_t(x, tc)
        th = cv2.resize(img, R.THUMB, interpolation=cv2.INTER_AREA).astype(np.int16)
        diff = 0.0 if prev_thumb is None else float(np.abs(th - prev_thumb).mean()); new = prev_thumb is None or diff >= R.DUP_THRESH
        gx = gy = 0.0; conf = True; cut = False
        if new:
            poss[t], times[t], smalls[t] = x, tc, R.gray_small(img)
            if prev_id is not None:
                f01 = R.flow(smalls[prev_id], smalls[t]); f10 = R.flow(smalls[t], smalls[prev_id]); gx, gy, ok = R.global_motion(f01); conf = ok > 0.5; cut = cd.check(R.fb_consistency(f01, f10))
            prev_id = t
        prev_thumb = th; eng.push(t, new, gx, gy, conf, cut)
        k = t - R.LOOKAHEAD
        if k < 0: continue
        d = eng.decide(k, t)
        shown_in.append(scene.posfn(seq[k]))
        if d[0] == 'pass':
            shown_out.append(poss[d[1]])
        else:
            _, a, b, al = d; interp_n += 1
            shown_out.append((1 - al) * poss[a] + al * poss[b])
            if scene.cut_at is not None and times[a] < scene.cut_at <= times[b]:
                crossings += 1
    si = np.abs(np.diff(shown_in)); so = np.abs(np.diff(shown_out))
    # "worse" = output has a bigger single jump than the input ever had near it
    worst_in, worst_out = si.max(), so.max()
    print(f"{name:40s} interpolated {interp_n:4d}  cut-crossing blends {crossings}  "
          f"largest jump in {worst_in:7.1f}px out {worst_out:7.1f}px")
    return crossings, worst_in, worst_out

def stopstart(t):  # pan, stop dead for 1 s, pan again
    if t < 2: return 300 * t
    if t < 3: return 600
    return 600 + 300 * (t - 3)
def reverse(t):  # pan right then snap back left
    return 300 * t if t < 3 else 900 - 300 * (t - 3)
def accel(t):  # sudden speed doubling
    return 200 * t if t < 3 else 600 + 600 * (t - 3)

r = []
r.append(run_edge("pan stops dead then restarts", EdgeScene(stopstart)))
r.append(run_edge("direction reversal", EdgeScene(reverse)))
r.append(run_edge("sudden speed doubling", EdgeScene(accel)))
r.append(run_edge("scene cut mid-pan", EdgeScene(lambda t: 300 * t, cut_at=3.0)))
r.append(run_edge("still camera, moving character", EdgeScene(lambda t: 0.0, sprite=True)))
bad = [x for x in r if x[0] > 0 or x[2] > x[1] * 1.05]
print("RESULT:", "PASS" if not bad else f"FAIL {bad}")
