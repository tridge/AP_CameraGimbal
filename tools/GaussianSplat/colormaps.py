"""Thermal colormaps as 256x3 float lookup tables in 0..1."""
import numpy as np

_STOPS = {
    'ironbow': [(0.0, (0, 0, 0)), (0.15, (32, 0, 140)), (0.35, (145, 0, 155)), (0.55, (225, 60, 30)),
                (0.75, (250, 160, 0)), (0.9, (255, 225, 60)), (1.0, (255, 255, 255))],
    'whitehot': [(0.0, (0, 0, 0)), (1.0, (255, 255, 255))],
    'blackhot': [(0.0, (255, 255, 255)), (1.0, (0, 0, 0))],
    'inferno': [(0.0, (0, 0, 4)), (0.25, (87, 16, 110)), (0.5, (188, 55, 84)), (0.75, (249, 142, 9)),
                (1.0, (252, 255, 164))],
    'rainbow': [(0.0, (0, 0, 128)), (0.2, (0, 0, 255)), (0.4, (0, 255, 255)), (0.6, (255, 255, 0)),
                (0.8, (255, 0, 0)), (1.0, (255, 255, 255))],
}

NAMES = list(_STOPS)


def colormap_lut(name):
    if name not in _STOPS:
        raise ValueError('unknown colormap %s (choose from %s)' % (name, ', '.join(NAMES)))
    stops = _STOPS[name]
    x = np.linspace(0, 1, 256)
    pos = [s[0] for s in stops]
    return np.stack([np.interp(x, pos, [s[1][c] / 255 for s in stops]) for c in range(3)], axis=1)
