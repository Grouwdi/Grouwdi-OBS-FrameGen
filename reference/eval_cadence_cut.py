import sys, numpy as np, cv2
sys.path.insert(0, '.')
import ref_smoother as R
import eval_cadence as E  # prints its own table on import
W, H = E.W, E.H
def render(t):
    tex_a, tex_b = (E.TEX, E.TEX2) if t < 4.0 else (E.TEX2, E.TEX)  # scene cut at 4.0 s
    a = E.render_x(300 * t * 3, tex_a); b = E.render_x(300 * t, tex_b)
    img = b.copy(); img[: H // 2] = a[: H // 2]; return img
ct = np.arange(0, 9, 1 / 43.0)
eng = R.Engine(); cd = R.CutDetector(); times = {}; smalls = {}; prev_thumb = prev_id = None
cross = interp = cuts = 0
for k in range(8 * 60):
    tc = ct[int(np.searchsorted(ct, k / 60 + 1e-9) - 1)]; img = render(tc)
    th = cv2.resize(img, R.THUMB, interpolation=cv2.INTER_AREA).astype(np.int16)
    diff = 0.0 if prev_thumb is None else float(np.abs(th - prev_thumb).mean())
    new = prev_thumb is None or diff >= R.DUP_THRESH
    gx = gy = 0.0; conf = True; cut = False
    if new:
        times[k] = tc; smalls[k] = R.gray_small(img)
        if prev_id is not None:
            f01 = R.flow(smalls[prev_id], smalls[k]); f10 = R.flow(smalls[k], smalls[prev_id]); gx, gy, ok = R.global_motion(f01); conf = ok > 0.5; cut = cd.check(R.fb_consistency(f01, f10))
        prev_id = k
    cuts += cut
    if new and 235 <= k <= 245 and prev_id is not None: print(f'  tick {k} consistency {R.fb_consistency(f01, f10):.2f} cut={cut}')
    prev_thumb = th; eng.push(k, new, gx, gy, conf, cut)
    o = k - R.LOOKAHEAD
    if o < 0: continue
    d = eng.decide(o, k)
    if d[0] == "interp":
        interp += 1
        if times[d[1]] < 4.0 <= times[d[2]]: cross += 1
print(f"\nscene cut during 43 fps cadence: cuts detected {cuts}, interpolated {interp}, blends across the cut {cross}")
print("RESULT:", "PASS" if cuts == 1 and cross == 0 and interp > 300 else "FAIL")
