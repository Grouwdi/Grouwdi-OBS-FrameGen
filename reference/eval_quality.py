import sys, math, numpy as np, cv2
sys.path.insert(0, '.')
import ref_smoother as R

def psnr(a, b):
    m = np.mean((a.astype(np.float32) - b.astype(np.float32)) ** 2)
    return 99.0 if m == 0 else 10 * math.log10(255 * 255 / m)

def quality(name, game_fps, **kw):
    scene = R.Scene(wobble=kw.pop('wobble', 0.0))
    seq = R.capture_sequence(scene, game_fps, 6, kw.get('card_fps', 60.0), 60.0, kw.get('jitter', 0.0))
    eng = R.Engine(); cd = R.CutDetector(); frames = {}; smalls = {}; poss = {}; flows = {}
    prev_thumb = prev_id = None; ps_interp = []; ps_blend = []
    for t, (x, _) in enumerate(seq):
        img = scene.render(x)
        th = cv2.resize(img, R.THUMB, interpolation=cv2.INTER_AREA).astype(np.int16)
        diff = 0.0 if prev_thumb is None else float(np.abs(th - prev_thumb).mean()); new = prev_thumb is None or diff >= R.DUP_THRESH
        gx = gy = 0.0; conf = True; cut = False
        if new:
            frames[t], poss[t], smalls[t] = img, x, R.gray_small(img)
            if prev_id is not None:
                f01 = R.flow(smalls[prev_id], smalls[t]); f10 = R.flow(smalls[t], smalls[prev_id])
                flows[(prev_id, t)] = (f01, f10); gx, gy, ok = R.global_motion(f01); conf = ok > 0.5; cut = cd.check(R.fb_consistency(f01, f10))
            prev_id = t
        prev_thumb = th; eng.push(t, new, gx, gy, conf, cut)
        k = t - R.LOOKAHEAD
        if k < 0: continue
        d = eng.decide(k, t)
        if d[0] == 'interp':
            _, a, b, al = d
            truth = scene.render((1 - al) * poss[a] + al * poss[b])
            f01, f10 = flows[(a, b)]
            out = R.interpolate(frames[a], frames[b], f01, f10, al)
            blend = ((1 - al) * frames[a].astype(np.float32) + al * frames[b].astype(np.float32)).astype(np.uint8)
            m = 20  # ignore borders (content enters the frame)
            ps_interp.append(psnr(out[m:-m, m:-m], truth[m:-m, m:-m]))
            ps_blend.append(psnr(blend[m:-m, m:-m], truth[m:-m, m:-m]))
    print(f"{name:30s} interpolated frames: {len(ps_interp):4d}   flow-warp PSNR {np.median(ps_interp):5.1f} dB "
          f"(worst {np.min(ps_interp):5.1f})   vs plain blend {np.median(ps_blend):5.1f} dB")

quality("game 119 into 60", 119)
quality("uncapped ~165 jittery", 165, jitter=0.25)
quality("game 59 into 60", 59)
