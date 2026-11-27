"""Ghosting test: camera pan + static HUD + a character moving the other way
(occlusion), game 43 fps into 60. Compares the current blend with an
artifact-aware blend against the true in-between image."""
import math, sys, numpy as np, cv2
sys.path.insert(0, '.')
import ref_smoother as R

W, H = 640, 360
BG = R.make_texture(W * 14, H, 1)

def render(t):
    x = 300 * t
    m = np.float32([[1, 0, -(x % (BG.shape[1] - W - 2))], [0, 1, 0]])
    img = cv2.warpAffine(BG, m, (W, H), flags=cv2.INTER_LINEAR).astype(np.float32)
    # character: bright textured block moving left fast (against the pan)
    cx = int(W - 80 - (420 * t) % (W - 100))
    blk = (R.make_texture(70, 120, 9).astype(np.float32) * 0.5 + 120)
    img[180:300, cx:cx + 70] = blk
    img = np.clip(img, 0, 255).astype(np.uint8)
    # HUD: static bar + crosshair, never moves
    img[8:40, 8:240] = 30
    cv2.putText(img, "HP 100  AMMO 30", (14, 32), cv2.FONT_HERSHEY_SIMPLEX, 0.6, 235, 2)
    cv2.circle(img, (W // 2, H // 2), 6, 255, 2)
    return img

def warp(img, fx, fy):
    h, w = img.shape[:2]
    gx, gy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    return cv2.remap(img.astype(np.float32), gx + fx, gy + fy, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)

def flows_full(i0, i1):
    s0, s1 = R.gray_small(i0), R.gray_small(i1)
    f01, f10 = R.flow(s0, s1), R.flow(s1, s0)
    h, w = i0.shape[:2]; sx = w / f01.shape[1]; sy = h / f01.shape[0]
    up = lambda f: cv2.resize(f, (w, h), interpolation=cv2.INTER_LINEAR) * np.array([sx, sy], np.float32)
    return up(f01), up(f10)

def blend_current(i0, i1, F01, F10, a):
    fa0 = -(1 - a) * a * F01 + a * a * F10
    fa1 = (1 - a) ** 2 * F01 - a * (1 - a) * F10
    return (1 - a) * warp(i0, fa0[..., 0], fa0[..., 1]) + a * warp(i1, fa1[..., 0], fa1[..., 1])

def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1); return t * t * (3 - 2 * t)

def blend_aware(i0, i1, F01, F10, a, ks_lo=0.0, ks_hi=0.04, bad_lo=0.06, bad_hi=0.16):
    """Same math as the new shader (values in 0..1)."""
    fa0 = -(1 - a) * a * F01 + a * a * F10
    fa1 = (1 - a) ** 2 * F01 - a * (1 - a) * F10
    w0 = warp(i0, fa0[..., 0], fa0[..., 1]) / 255; w1 = warp(i1, fa1[..., 0], fa1[..., 1]) / 255
    z0 = i0.astype(np.float32) / 255; z1 = i1.astype(np.float32) / 255
    ew = np.abs(w0 - w1); ez = np.abs(z0 - z1)
    warped = (1 - a) * w0 + a * w1
    still = (1 - a) * z0 + a * z1
    ks = smoothstep(ks_lo, ks_hi, ew - ez)  # unwarped clearly matches better: static content (HUD)
    col = warped * (1 - ks) + still * ks
    near = w0 if a < 0.5 else w1
    bad = smoothstep(bad_lo, bad_hi, np.minimum(ew, ez))  # neither matches: occlusion -> nearest frame
    col = col * (1 - bad) + near * bad
    return np.clip(col * 255, 0, 255)

def psnr(a, b, mask=None):
    d = (a.astype(np.float32) - b.astype(np.float32)) ** 2
    m = d[mask].mean() if mask is not None else d.mean()
    return 99.0 if m == 0 else 10 * math.log10(255 * 255 / m)

if __name__ == "__main__":
    rng = np.random.default_rng(4)
    hud = np.zeros((H, W), bool); hud[8:40, 8:240] = True; hud[H//2-9:H//2+9, W//2-9:W//2+9] = True
    res = {"cur": [], "new": [], "cur_hud": [], "new_hud": [], "cur_char": [], "new_char": [], "near": []}
    for i in range(40):
        t0 = 0.5 + i * 0.09; t1 = t0 + 1 / 43; a = float(rng.uniform(0.2, 0.8))
        i0, i1 = render(t0), render(t1); truth = render(t0 + a / 43)
        F01, F10 = flows_full(i0, i1)
        cur = blend_current(i0, i1, F01, F10, a); new = blend_aware(i0, i1, F01, F10, a)
        near = (i0 if a < 0.5 else i1)
        # ghost-prone region: around the character
        cx = int(W - 80 - (420 * (t0 + a / 43)) % (W - 100)); ch = np.zeros((H, W), bool); ch[170:310, max(0, cx - 40):cx + 110] = True
        m = np.zeros((H, W), bool); m[10:-10, 10:-10] = True
        res["cur"].append(psnr(cur, truth, m)); res["new"].append(psnr(new, truth, m)); res["near"].append(psnr(near, truth, m))
        res["cur_hud"].append(psnr(cur, truth, hud)); res["new_hud"].append(psnr(new, truth, hud))
        res["cur_char"].append(psnr(cur, truth, ch)); res["new_char"].append(psnr(new, truth, ch))
    f = lambda k: f"{np.median(res[k]):5.1f} dB (worst {np.min(res[k]):5.1f})"
    print(f"whole frame : current blend {f('cur')}   new blend {f('new')}   (just repeating nearest frame {f('near')})")
    print(f"HUD         : current blend {f('cur_hud')}   new blend {f('new_hud')}")
    print(f"character   : current blend {f('cur_char')}   new blend {f('new_char')}")


def sample_flow(F, fx, fy):
    return np.dstack([warp(F[..., 0], fx, fy), warp(F[..., 1], fx, fy)])


def blend_visibility(i0, i1, F01, F10, a, tol=2.0, ks_lo=0.0, ks_hi=0.04):
    """Bidirectional visibility: trust a frame's sample only where that frame's
    forward/backward motion is consistent, plus the static check for HUD."""
    fa0 = -(1 - a) * a * F01 + a * a * F10
    fa1 = (1 - a) ** 2 * F01 - a * (1 - a) * F10
    w0 = warp(i0, fa0[..., 0], fa0[..., 1]) / 255; w1 = warp(i1, fa1[..., 0], fa1[..., 1]) / 255
    # consistency of frame 0's motion where we sample it, and of frame 1's
    f01_at = sample_flow(F01, fa0[..., 0], fa0[..., 1])
    back = sample_flow(F10, fa0[..., 0] + f01_at[..., 0], fa0[..., 1] + f01_at[..., 1])
    e0 = np.hypot(f01_at[..., 0] + back[..., 0], f01_at[..., 1] + back[..., 1])
    f10_at = sample_flow(F10, fa1[..., 0], fa1[..., 1])
    fwd = sample_flow(F01, fa1[..., 0] + f10_at[..., 0], fa1[..., 1] + f10_at[..., 1])
    e1 = np.hypot(f10_at[..., 0] + fwd[..., 0], f10_at[..., 1] + fwd[..., 1])
    v0 = 1.0 / (1.0 + (e0 / tol) ** 2); v1 = 1.0 / (1.0 + (e1 / tol) ** 2)
    k0 = (1 - a) * v0 + 1e-4; k1 = a * v1 + 1e-4
    warped = (k0[..., None] * w0 + k1[..., None] * w1) / (k0 + k1)[..., None] if w0.ndim == 3 else (k0 * w0 + k1 * w1) / (k0 + k1)
    z0 = i0.astype(np.float32) / 255; z1 = i1.astype(np.float32) / 255
    still = (1 - a) * z0 + a * z1
    ew = np.abs(w0 - w1); ez = np.abs(z0 - z1)
    ks = smoothstep(ks_lo, ks_hi, ew - ez)
    return np.clip((warped * (1 - ks) + still * ks) * 255, 0, 255)


def compare(variants):
    rng = np.random.default_rng(4)
    hud = np.zeros((H, W), bool); hud[8:40, 8:240] = True; hud[H//2-9:H//2+9, W//2-9:W//2+9] = True
    cases = []
    for i in range(40):
        t0 = 0.5 + i * 0.09; t1 = t0 + 1 / 43; a = float(rng.uniform(0.2, 0.8))
        i0, i1 = render(t0), render(t1); truth = render(t0 + a / 43)
        F01, F10 = flows_full(i0, i1)
        cx = int(W - 80 - (420 * (t0 + a / 43)) % (W - 100)); ch = np.zeros((H, W), bool); ch[170:310, max(0, cx - 40):cx + 110] = True
        cases.append((i0, i1, F01, F10, a, truth, ch))
    m = np.zeros((H, W), bool); m[10:-10, 10:-10] = True
    for name, fn in variants:
        whole, hd, chr_ = [], [], []
        for (i0, i1, F01, F10, a, truth, ch) in cases:
            out = fn(i0, i1, F01, F10, a)
            whole.append(psnr(out, truth, m)); hd.append(psnr(out, truth, hud)); chr_.append(psnr(out, truth, ch))
        g = lambda v: f"{np.median(v):5.1f} (worst {np.min(v):5.1f})"
        print(f"{name:34s} whole {g(whole)}  HUD {g(hd)}  character {g(chr_)}")


def blend_static(i0, i1, F01, F10, a, lo=0.0, hi=0.04, still_max=0.03, still_soft=0.03):
    """Static check: use the unwarped pixels where (1) not warping matches better
    and (2) the two frames are nearly identical there (truly static: HUD, overlays)."""
    fa0 = -(1 - a) * a * F01 + a * a * F10
    fa1 = (1 - a) ** 2 * F01 - a * (1 - a) * F10
    w0 = warp(i0, fa0[..., 0], fa0[..., 1]) / 255; w1 = warp(i1, fa1[..., 0], fa1[..., 1]) / 255
    z0 = i0.astype(np.float32) / 255; z1 = i1.astype(np.float32) / 255
    ew = np.abs(w0 - w1); ez = np.abs(z0 - z1)
    ks = smoothstep(lo, hi, ew - ez) * (1 - smoothstep(still_max, still_max + still_soft, ez))
    col = ((1 - a) * w0 + a * w1) * (1 - ks) + ((1 - a) * z0 + a * z1) * ks
    return np.clip(col * 255, 0, 255)
