"""Thermal splat appearance: SH plus an optional per-Gaussian directional emission lobe.

The lobe, amp * exp(kappa * (cos(angle to axis) - 1)), represents sharp directional
sources (e.g. a sector emitter) that low-order SH can only blur. Values are in the
scaled temperature space of train.TempScale.
"""
import math

import torch
import torch.nn.functional as F

SH_C0 = 0.28209479177387814
LOBE_KEYS = ('lobe_amp', 'lobe_dir', 'lobe_kappa')


def has_lobe(params):
    return 'lobe_amp' in params


def init_lobe(n, device, kappa=4.0):
    return dict(lobe_amp=torch.full((n,), -6.0, device=device),  # softplus -> ~0.0025
                lobe_dir=F.normalize(torch.randn(n, 3, device=device), dim=1),
                lobe_kappa=torch.full((n,), math.log(kappa), device=device))


def lobe_value(params, out_dirs):
    """out_dirs: [..., N, 3] unit vectors from the Gaussian towards the camera."""
    axis = F.normalize(params['lobe_dir'], dim=-1)
    cos = (out_dirs * axis).sum(-1)
    return F.softplus(params['lobe_amp']) * torch.exp(torch.exp(params['lobe_kappa']) * (cos - 1))


def view_values(params, campos, sh_degree, lobe_gain=None, source=None):
    """Scalar scaled temperature of each Gaussian seen from camera centres campos [C, 3] -> [C, N].
    lobe_gain [C] is a per-view source strength: with source=(centre, radius) it scales the emission of
    Gaussians within radius of centre, without it every lobe (older checkpoints)."""
    from gsplat import spherical_harmonics
    sh = torch.cat([params['sh0'], params['shN']], 1)
    dirs = params['means'][None] - campos[:, None]  # camera -> Gaussian, as gsplat uses
    v = spherical_harmonics(sh_degree, dirs, sh[None].expand(len(campos), -1, -1, -1)).mean(-1) + 0.5
    if has_lobe(params):
        lobe = lobe_value(params, F.normalize(-dirs, dim=-1))
        if lobe_gain is not None and source is None:
            lobe = lobe * lobe_gain[:, None]  # legacy: scale every lobe
        v = v + lobe
    if lobe_gain is not None and source is not None:
        # time-varying source: scale the whole emission of Gaussians near it
        near = (params['means'] - source[0]).norm(dim=-1) <= source[1]
        v = torch.where(near[None], v * lobe_gain[:, None], v)
    return v.clamp(min=0)


def sh_basis(degree, dirs):
    """[M, K] real SH basis matching gsplat for directions dirs [M, 3]."""
    from gsplat import spherical_harmonics
    K = (degree + 1) ** 2
    M = len(dirs)
    eye = torch.eye(K, device=dirs.device)[:, None, :, None].expand(K, M, K, 3)
    out = spherical_harmonics(degree, dirs[None].expand(K, M, 3).reshape(-1, 3), eye.reshape(-1, K, 3))
    return out[:, 0].reshape(K, M).T


def bake_lobe_into_sh(params, degree, samples=2000, chunk=8192):
    """Least-squares project the lobe onto SH of the given degree, for generic PLY viewers."""
    if not has_lobe(params):
        return params['sh0'], params['shN']
    device = params['means'].device
    # Fibonacci sphere
    i = torch.arange(samples, device=device, dtype=torch.float32) + 0.5
    phi = torch.acos(1 - 2 * i / samples)
    theta = math.pi * (1 + 5 ** 0.5) * i
    d = torch.stack([torch.cos(theta) * torch.sin(phi), torch.sin(theta) * torch.sin(phi), torch.cos(phi)], 1)
    B = sh_basis(degree, d)  # basis at gsplat dirs (camera -> Gaussian)
    P = torch.linalg.pinv(B)  # [K, M]
    sh = torch.cat([params['sh0'], params['shN']], 1).clone()
    for s in range(0, len(sh), chunk):
        sub = {k: params[k][s:s + chunk] for k in LOBE_KEYS}
        L = lobe_value({k: v[:, None] if v.dim() == 1 else v[:, None, :] for k, v in sub.items()},
                       -d[None])  # outgoing direction is -dirs
        sh[s:s + chunk] += (L @ P.T)[:, :, None]
    return sh[:, :1].contiguous(), sh[:, 1:].contiguous()
