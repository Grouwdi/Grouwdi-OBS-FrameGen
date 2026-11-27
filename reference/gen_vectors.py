"""Record engine inputs/decisions from the reference runs, for the C port test."""
import sys, numpy as np, cv2
sys.path.insert(0, '.')
import ref_smoother as R
import eval_edges as E  # reuses edge scenes (prints its own results on import)

def record(path, scene_render, posfn_ticks):
    eng = R.Engine(); cd = R.CutDetector(); smalls = {}; prev_thumb = prev_id = None
    rows = []
    for t, (x, tc) in enumerate(posfn_ticks):
        img = scene_render(x, tc)
        th = cv2.resize(img, R.THUMB, interpolation=cv2.INTER_AREA).astype(np.int16)
        diff = 0.0 if prev_thumb is None else float(np.abs(th - prev_thumb).mean()); new = prev_thumb is None or diff >= R.DUP_THRESH
        gx = gy = 0.0; conf = True; cut = False
        if new:
            smalls[t] = R.gray_small(img)
            if prev_id is not None:
                f01 = R.flow(smalls[prev_id], smalls[t]); f10 = R.flow(smalls[t], smalls[prev_id]); gx, gy, ok = R.global_motion(f01); conf = ok > 0.5; cut = cd.check(R.fb_consistency(f01, f10))
            prev_id = t
        prev_thumb = th; eng.push(t, new, gx, gy, conf, cut)
        k = t - R.LOOKAHEAD
        d = eng.decide(k, t) if k >= 0 else None
        if d is None: kind, a, b, al = 0, 0, 0, 0.0
        elif d[0] == 'pass': kind, a, b, al = 1, d[1], 0, 0.0
        else: kind, a, b, al = 2, d[1], d[2], d[3]
        rows.append(f"{t},{int(new)},{gx:.17g},{gy:.17g},{int(conf)},{int(cut)},{kind},{a},{b},{al:.17g}")
    open(path, 'w').write("\n".join(rows) + "\n")
    return len(rows)

def ticks_for(posfn, fps, seconds=6, jitter=0.0, card=60.0):
    ct = R.content_times(fps, seconds, jitter); out = []
    for k in range(int(seconds * 60)):
        card_idx = int(np.floor(k / 60 * card + 1e-9)); tcard = card_idx / card
        gi = int(np.searchsorted(ct, tcard + 1e-9) - 1); out.append((posfn(ct[gi]), ct[gi]))
    return out

plain = R.Scene()
def plain_render(x, tc): return plain.render(x)
cases = {
    "v_119": (plain_render, ticks_for(lambda t: 300 * t, 119)),
    "v_59": (plain_render, ticks_for(lambda t: 300 * t, 59)),
    "v_165j": (plain_render, ticks_for(lambda t: 300 * t, 165, jitter=0.25)),
    "v_card59": (plain_render, ticks_for(lambda t: 300 * t, 60, card=59.0)),
}
for name, fn in [("v_stop", E.stopstart), ("v_rev", E.reverse)]:
    s = E.EdgeScene(fn); cases[name] = (s.render_t, ticks_for(fn, 119))
cut = E.EdgeScene(lambda t: 300 * t, cut_at=3.0); cases["v_cut"] = (cut.render_t, ticks_for(cut.posfn, 119))
spr = E.EdgeScene(lambda t: 0.0, sprite=True); cases["v_sprite"] = (spr.render_t, ticks_for(spr.posfn, 119))
import eval_cadence as C
import math
def cad_ticks(fps_fn, kind, seconds=6, cut_at=None):
    t, ct = 0.0, []
    while t < seconds + 1:
        ct.append(t); t += 1.0 / fps_fn(t)
    ct = np.array(ct); out = []
    for k in range(int(seconds * 60)):
        gi = int(np.searchsorted(ct, k / 60 + 1e-9) - 1); out.append((ct[gi], ct[gi]))
    return out
def cad_render(kind, cut_at=None):
    def r(x, tc):
        img = C.render(tc, kind)
        if cut_at is not None and tc >= cut_at:
            img = 255 - img
        return img
    return r
cases["v_43par"] = (cad_render("parallax"), cad_ticks(lambda t: 43, "parallax"))
cases["v_varpar"] = (cad_render("parallax"), cad_ticks(lambda t: 47.5 + 7.5 * math.sin(t * 1.7), "parallax"))
cases["v_43spr"] = (cad_render("sprite"), cad_ticks(lambda t: 43, "sprite"))
cases["v_43cut"] = (cad_render("parallax", cut_at=3.0), cad_ticks(lambda t: 43, "parallax"))
import os; os.makedirs('../test/vectors', exist_ok=True)
for name, (render, ticks) in cases.items():
    n = record(f'../test/vectors/{name}.csv', render, ticks); print(name, n)
