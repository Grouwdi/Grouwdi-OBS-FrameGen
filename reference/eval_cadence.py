"""Cadence-mode tests: irregular frame arrival (repeats) with motion the pan
check cannot trust (parallax), plus regression on the original cases.
Smoothness is measured on true content TIME, which works for any motion."""
import sys, math, numpy as np, cv2
sys.path.insert(0, '.')
import ref_smoother as R

W, H = 640, 360
TEX = R.make_texture(W * 14, H, 1)
TEX2 = R.make_texture(W * 14, H, 5)

def render(t, kind):
    if kind == "pan":
        x = 300 * t
        m = np.float32([[1, 0, -(x % (TEX.shape[1] - W - 2))], [0, 1, 0]])
        return cv2.warpAffine(TEX, m, (W, H), flags=cv2.INTER_LINEAR)
    if kind == "parallax":  # top half moves 3x faster than bottom half (3D-like depth)
        a = render_x(300 * t * 3, TEX); b = render_x(300 * t, TEX2)
        img = b.copy(); img[: H // 2] = a[: H // 2]; return img
    if kind == "sprite":  # still camera, character moving
        img = TEX[:, :W].copy()
        sx = int(50 + 250 * t) % (W - 80)
        cv2.circle(img, (sx + 40, 180), 40, 255, -1); return img

def render_x(x, tex):
    m = np.float32([[1, 0, -(x % (tex.shape[1] - W - 2))], [0, 1, 0]])
    return cv2.warpAffine(tex, m, (W, H), flags=cv2.INTER_LINEAR)

def run(name, fps_fn, kind, seconds=8):
    # game frame times from a (possibly varying) fps
    t, ct = 0.0, []
    while t < seconds + 1:
        ct.append(t); t += 1.0 / fps_fn(t)
    ct = np.array(ct)
    eng = R.Engine(); cd = R.CutDetector(); times = {}; smalls = {}; prev_thumb = prev_id = None
    seq_in, seq_out, interp = [], [], 0
    shown = []
    for k in range(int(seconds * 60)):
        gi = int(np.searchsorted(ct, k / 60 + 1e-9) - 1); tc = ct[gi]; shown.append(tc)
        img = render(tc, kind)
        th = cv2.resize(img, R.THUMB, interpolation=cv2.INTER_AREA).astype(np.int16)
        diff = 0.0 if prev_thumb is None else float(np.abs(th - prev_thumb).mean())
        new = prev_thumb is None or diff >= 0.1
        gx = gy = 0.0; conf = True; cut = False
        if new:
            times[k] = tc; smalls[k] = R.gray_small(img)
            if prev_id is not None:
                f01 = R.flow(smalls[prev_id], smalls[k]); f10 = R.flow(smalls[k], smalls[prev_id]); gx, gy, ok = R.global_motion(f01); conf = ok > 0.5; cut = cd.check(R.fb_consistency(f01, f10))
            prev_id = k
        prev_thumb = th; eng.push(k, new, gx, gy, conf, cut)
        o = k - R.LOOKAHEAD
        if o < 0: continue
        d = eng.decide(o, k)
        seq_in.append(shown[o])
        if d[0] == "pass": seq_out.append(times[d[1]])
        else:
            _, a, b, al = d; interp += 1
            seq_out.append((1 - al) * times[a] + al * times[b])
    def worst(p):
        s = np.diff(np.array(p[40:])); ker = np.ones(9) / 9
        local = np.convolve(s, ker, mode="same"); dev = np.abs(s - local)[8:-8]
        m = np.mean(np.abs(local[8:-8])); return float(np.max(dev) / m), float(np.mean(dev) / m)
    wi, mi = worst(seq_in); wo, mo = worst(seq_out)
    print(f"{name:44s} interp {interp:4d}  worst {wi*100:6.1f}% -> {wo*100:5.1f}%   mean {mi*100:5.1f}% -> {mo*100:5.1f}%")
    return wi, wo, mi, mo

res = {}
res["43pan"] = run("43 fps into 60, pan", lambda t: 43, "pan")
res["43par"] = run("43 fps into 60, parallax (pan check fails)", lambda t: 43, "parallax")
res["50par"] = run("50 fps into 60, parallax", lambda t: 50, "parallax")
res["var"] = run("40-55 fps fluctuating, parallax", lambda t: 47.5 + 7.5 * math.sin(t * 1.7), "parallax")
res["43spr"] = run("43 fps into 60, still camera + character", lambda t: 43, "sprite")
res["59par"] = run("59 fps into 60, parallax", lambda t: 59, "parallax")
res["60par"] = run("clean 60 into 60, parallax", lambda t: 60, "parallax")
res["119par"] = run("119 fps into 60, parallax", lambda t: 119, "parallax")
bad = []
for k in ["43pan", "43par", "50par", "var", "43spr", "59par"]:
    wi, wo, mi, mo = res[k]
    if not (mo < mi * 0.35): bad.append(k)
if res["60par"][3] > 0.001: bad.append("60par changed")
if res["119par"][1] > res["119par"][0] * 1.05: bad.append("119par worse")
print("RESULT:", "PASS" if not bad else f"FAIL {bad}")
