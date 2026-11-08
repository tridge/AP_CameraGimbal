#!/usr/bin/env python3
"""G1 criteria 7 and 8 from the dumps of the Brush fork's thermal_loss test
(THERMAL_LOSS_DUMP=DIR G1_DATASET=DATASET cargo test -p brush-train --test thermal_loss):
the Brush loss and gradient against train.py's loss (its own ssim(), autograd), and the
targets as read by the Brush kernel against the prepared float32 targets."""
import argparse
import json
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('dump', type=Path)
    ap.add_argument('dataset', type=Path)
    a = ap.parse_args()
    import torch
    from train import ssim

    r = lambda n: np.fromfile(a.dump / n, '<f4')  # noqa: E731
    loss_b, loss0_b, thr, hw, ssim_w, H, W = [float(v) for v in r('loss.f32')]
    H, W = int(H), int(W)
    gt = torch.from_numpy(r('gt.f32').reshape(H, W)).float()
    pred = torch.from_numpy(r('pred.f32').reshape(H, W)).float().requires_grad_()
    grad_b = r('grad.f32').reshape(H, W, 3).sum(2)

    def train_py_loss(hot_weight):  # train.py's training loss, hot_tolerance 0
        hot = (gt > float(thr)).float()
        l1 = (pred - gt).abs().mean() + hot_weight * ((pred - gt).abs() * hot).sum() / pred.numel()
        return (1 - ssim_w) * l1 + ssim_w * (1 - ssim(pred[None, None], gt[None, None]))
    ref = train_py_loss(float(hw))
    ref.backward()
    ref0 = float(train_py_loss(0.0).detach())
    grad_r = pred.grad.numpy()
    # Brush omits train.py's constant +ssim_weight (it adds -w * SSIM instead of w * (1 - SSIM))
    out = {}
    for name, b, t in (('loss', float(loss_b) + ssim_w, float(ref.detach())), ('loss_no_hot', float(loss0_b) + ssim_w, ref0)):
        out[name] = dict(brush=b, train_py=t, rel_err=abs(b - t) / abs(t))
    out['grad_rel_err'] = float(np.linalg.norm(grad_b - grad_r) / np.linalg.norm(grad_r))
    out['hot_pixels'] = int((gt > float(thr)).sum())

    gt32 = np.load(a.dataset / 'gt.npy', mmap_mode='r')
    meta = json.loads((a.dataset / 'g1.json').read_text())
    idx = {n: i for i, n in enumerate(meta['names'])}
    worst = 0.0
    for name in meta['test']:
        read = np.fromfile(a.dump / ('gtread_%s.f32' % name), '<f4').reshape(meta['height'], meta['width'])
        worst = max(worst, float(np.abs(read - gt32[idx[name]]).max()))
    out['target_read_max_abs_err'] = worst
    out['target_read_views'] = len(meta['test'])
    ok = dict(c7=worst <= 2e-5, c8=out['loss']['rel_err'] <= 1e-4 and out['loss_no_hot']['rel_err'] <= 1e-4
              and out['grad_rel_err'] <= 1e-3)
    out['pass'] = ok
    print(json.dumps(out, indent=1))


if __name__ == '__main__':
    main()
