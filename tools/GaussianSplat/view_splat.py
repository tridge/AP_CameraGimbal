#!/usr/bin/env python3
"""Interactive browser viewer for a thermal splat from train.py.

Renders on this machine's GPU with gsplat (full view-dependent spherical harmonics,
exact colormapping of the rendered temperature) and streams frames to a browser via
viser. Open the printed URL; drag to orbit, right-drag to pan, scroll to zoom.
"""
import argparse
import json
import math
from pathlib import Path
import sys
import threading
import time

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from colormaps import NAMES, colormap_lut
from splat_common import setup_cuda_env

setup_cuda_env()
import torch  # noqa: E402
import torch.nn.functional as F  # noqa: E402
from splat_model import view_values  # noqa: E402


def rotmat_to_wxyz(R):
    w = np.sqrt(max(0.0, 1 + R[0, 0] + R[1, 1] + R[2, 2])) / 2
    x = np.sqrt(max(0.0, 1 + R[0, 0] - R[1, 1] - R[2, 2])) / 2
    y = np.sqrt(max(0.0, 1 - R[0, 0] + R[1, 1] - R[2, 2])) / 2
    z = np.sqrt(max(0.0, 1 - R[0, 0] - R[1, 1] + R[2, 2])) / 2
    x = np.copysign(x, R[2, 1] - R[1, 2])
    y = np.copysign(y, R[0, 2] - R[2, 0])
    z = np.copysign(z, R[1, 0] - R[0, 1])
    return np.array([w, x, y, z])


def wxyz_to_rotmat(q):
    w, x, y, z = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('splat', type=Path, help='splat directory (with splat.pt and scale.json) or WORKDIR')
    ap.add_argument('--host', default='127.0.0.1', help='bind address (0.0.0.0 to allow other machines)')
    ap.add_argument('--port', type=int, default=8080)
    ap.add_argument('--max-clients', type=int, default=0, help='refuse viewers beyond this many (0: no limit)')
    ap.add_argument('--max-width', type=int, default=1920, help='largest render width a viewer may choose')
    ap.add_argument('--title', default='', help='markdown shown at the top of the control panel')
    args = ap.parse_args()

    import viser
    from gsplat import rasterization

    d = args.splat
    if not (d / 'splat.pt').exists() and (d / 'splat' / 'splat.pt').exists():
        d = d / 'splat'
    ckpt = torch.load(d / 'splat.pt', map_location='cpu')
    meta = json.loads((d / 'scale.json').read_text())
    p = {k: v.cuda() for k, v in ckpt['params'].items()}
    means, quats = p['means'], F.normalize(p['quats'], dim=1)
    scales, opac = torch.exp(p['scales']), torch.sigmoid(p['opacities'])
    sh_degree = ckpt['sh_degree']
    rgb_mode = meta.get('kind') == 'rgb'  # train_rgb.py: true colour, no temperature scale
    ts = meta.get('temperature_scale') or dict(t0_c=0.0, t1_c=1.0, k_c=1.0)
    norm = np.log1p((ts['t1_c'] - ts['t0_c']) / ts['k_c'])

    def to_celsius(v):
        return ts['t0_c'] + ts['k_c'] * torch.expm1(v * norm)

    source = None
    if meta.get('source_region'):
        source = (torch.tensor(meta['source_region']['enu'], dtype=torch.float32, device='cuda'),
                  meta['source_region']['radius_m'])
    no_lobe = {k: v for k, v in p.items() if not k.startswith('lobe_')}
    viewmats = ckpt.get('viewmats_refined', ckpt['viewmats']).numpy()
    K_train = ckpt['K'].numpy()
    target = np.array(meta['hotspot']['enu'] if meta.get('hotspot') else meta['target_enu'])
    luts = {n: torch.tensor(colormap_lut(n), dtype=torch.float32, device='cuda') for n in NAMES}

    server = viser.ViserServer(host=args.host, port=args.port, verbose=False)
    server.scene.set_up_direction('+z')

    names = ckpt['names']
    hot_set = set(ckpt.get('hot', []))
    hot_ids = [i for i, n in enumerate(names) if n in hot_set]
    gains = ckpt.get('source_gain')
    fov_train = float(2 * np.arctan(ckpt['height'] / 2 / K_train[1, 1]))
    tmin0, tmax0 = float(np.floor(ts['t0_c'])), float(np.ceil(ts['t1_c']))
    max_width = args.max_width

    def look_at_training(client, i):
        c2w = np.linalg.inv(viewmats[i])
        # orbit centre on the optical axis at the target's depth, so the view is exactly the training pose
        depth = max(1.0, float((target - c2w[:3, 3]) @ c2w[:3, 2]))
        with client.atomic():
            client.camera.fov = fov_train
            client.camera.position = c2w[:3, 3]
            client.camera.look_at = c2w[:3, 3] + c2w[:3, 2] * depth
            client.camera.wxyz = rotmat_to_wxyz(c2w[:3, :3])

    def start_view(client, i):
        """Training view i, or for a near-vertical one an oblique view of the target from the same side:
        looking straight down puts the orbit controls at their pole, where dragging only spins the image."""
        c2w = np.linalg.inv(viewmats[i])
        axis = c2w[:3, 2]
        if axis[2] > -0.94:  # more than ~20 deg off vertical: the training view orbits fine
            look_at_training(client, i)
            return
        dist = float(np.linalg.norm(c2w[:3, 3] - target))
        side = -c2w[:3, 1]  # image up, i.e. the side the camera's top edge faces
        side = side[:2] / max(np.linalg.norm(side[:2]), 1e-6)
        el = np.radians(50)
        pos = target + dist * np.array([-side[0] * np.cos(el), -side[1] * np.cos(el), np.sin(el)])
        fwd = (target - pos) / np.linalg.norm(target - pos)
        right = np.cross(fwd, [0, 0, 1.0])
        right /= np.linalg.norm(right)
        down = np.cross(fwd, right)
        with client.atomic():
            client.camera.fov = fov_train
            client.camera.position = pos
            client.camera.look_at = target
            client.camera.wxyz = rotmat_to_wxyz(np.stack([right, down, fwd], axis=1))

    sessions = {}  # client_id -> per-viewer GUI state; each viewer has its own controls
    lock = threading.Lock()
    admit = threading.Lock()

    def make_session(client):
        gui, scene = client.gui, client.scene
        st = {'dirty': True, 'frustums': []}
        if args.title:
            gui.add_markdown(args.title)
        with gui.add_folder('Display', visible=rgb_mode):
            st['rgb_vd'] = gui.add_checkbox('View dependent colour', True)
            st['rgb_res'] = gui.add_slider('Render width', 320, max_width, 32, min(960, max_width))
            st['rgb_cams'] = gui.add_checkbox('Show training cameras', False)
        with gui.add_folder('Display', visible=not rgb_mode):
            st['cmap'] = gui.add_dropdown('Colormap', NAMES, initial_value='ironbow')
            st['lo'] = gui.add_slider('Min C', tmin0, tmax0, 0.5, tmin0)
            st['hi'] = gui.add_slider('Max C', tmin0, tmax0, 0.5, min(tmax0, tmin0 + 40))
            st['auto'] = gui.add_checkbox('Auto range', True)
            st['vd'] = gui.add_checkbox('View dependent', True)
            st['gain'] = gui.add_slider('Source strength', 0.0, 3.0, 0.05, 1.0, visible='lobe_amp' in p or source is not None)
            st['res'] = gui.add_slider('Render width', 320, max_width, 32, min(960, max_width))
            st['cams'] = gui.add_checkbox('Show training cameras', False)
            st['show_marker'] = gui.add_checkbox('Show hot spot marker', False)
            st['stats'] = gui.add_text('Apparent temp', '', disabled=True)
        with gui.add_folder('Training views'):
            st['view'] = gui.add_slider('View', 0, len(viewmats) - 1, 1, 0)
            st['hot_only'] = gui.add_checkbox('Hot views only', False)
            st['view_name'] = gui.add_text('Frame', '', disabled=True)
            go = gui.add_button('Go to view')
        gui.add_markdown('Drag to orbit, right-drag to pan, scroll to zoom. Coordinates: ENU metres about '
                         'the target, z up from takeoff.' + ('' if rgb_mode else ' Values are apparent temperature '
                         'decoded from the splat, not calibrated surface temperature.'))
        if rgb_mode:  # the RGB controls stand in for the thermal ones
            st['vd'], st['res'], st['cams'] = st['rgb_vd'], st['rgb_res'], st['rgb_cams']
        marker = scene.add_icosphere('/target', radius=0.4, color=(0, 255, 0), position=tuple(target),
                                     visible=False)

        def mark(_=None):
            st['dirty'] = True
        for k in ('cmap', 'lo', 'hi', 'auto', 'vd', 'res', 'gain'):
            st[k].on_update(mark)

        @st['show_marker'].on_update
        def _(_):
            marker.visible = st['show_marker'].value

        @st['cams'].on_update
        def _(_):
            for fr in st['frustums']:
                fr.remove()
            st['frustums'].clear()
            if not st['cams'].value:
                return
            for i in range(0, len(viewmats), max(1, len(viewmats) // 300)):
                c2w = np.linalg.inv(viewmats[i])
                st['frustums'].append(scene.add_camera_frustum(
                    '/cams/%d' % i, fov=fov_train, aspect=ckpt['width'] / ckpt['height'], scale=4.0,
                    wxyz=rotmat_to_wxyz(c2w[:3, :3]), position=c2w[:3, 3], color=(80, 160, 255)))

        def chosen_view():
            if st['hot_only'].value and hot_ids:
                return hot_ids[min(len(hot_ids) - 1, int(st['view'].value * len(hot_ids) / len(names)))]
            return int(st['view'].value)

        def show_name(_=None):
            i = chosen_view()
            st['view_name'].value = '%s%s' % (names[i], ' (hot)' if i in hot_ids else '')
        st['view'].on_update(show_name)
        st['hot_only'].on_update(show_name)
        show_name()

        @go.on_click
        def _(_):
            i = chosen_view()
            if gains is not None:
                st['gain'].value = round(float(gains[i]), 2)  # that frame's learned source strength
            look_at_training(client, i)

        @client.camera.on_update
        def _(_cam):
            st['dirty'] = True
        return st

    @server.on_client_connect
    def _(client):
        with admit:  # callbacks run on worker threads: reserve the slot atomically
            if client.client_id not in server.get_clients():  # already disconnected
                return
            busy = args.max_clients and len(sessions) >= args.max_clients
            if not busy:
                sessions[client.client_id] = None
        if busy:
            client.gui.add_markdown('**Sorry, the viewer is busy. Please try again in a few minutes.**')
            return
        st = make_session(client)
        with admit:
            if client.client_id not in sessions or client.client_id not in server.get_clients():
                sessions.pop(client.client_id, None)  # left while the controls were being built
                return
            sessions[client.client_id] = st
        print('client %d connected (%d active)' % (client.client_id, len(sessions)), flush=True)
        start_view(client, hot_ids[len(hot_ids) // 2] if hot_ids else len(viewmats) // 2)

    @server.on_client_disconnect
    def _(client):
        with admit:
            st = sessions.pop(client.client_id, None)
        if st is not None:
            print('client %d left (%d active)' % (client.client_id, len(sessions)), flush=True)

    def render(client, st):
        cam = client.camera
        # camera aspect and FOV come from the browser: bound them so no client can request a huge render
        aspect = min(max(float(cam.aspect), 0.25), 4.0)
        fov = min(max(float(cam.fov), 0.05), 2.5)
        W = min(int(st['res'].value), max_width)
        H = W / aspect
        if H > max_width:  # scale both to keep the browser's aspect
            W, H = W * max_width / H, max_width
        W, H = max(32, int(W)), max(32, int(H))
        f = H / 2 / np.tan(fov / 2)
        K = torch.tensor([[f, 0, W / 2], [0, f, H / 2], [0, 0, 1]], dtype=torch.float32, device='cuda')
        c2w = np.eye(4)
        c2w[:3, :3] = wxyz_to_rotmat(cam.wxyz)
        c2w[:3, 3] = cam.position
        vm = torch.tensor(np.linalg.inv(c2w), dtype=torch.float32, device='cuda')
        vd = st['vd'].value
        if rgb_mode:
            with torch.no_grad():
                colours = torch.cat([p['sh0'], p['shN']], 1)
                img, alpha, _ = rasterization(means, quats, scales, opac, colours, vm[None], K[None], W, H,
                                              sh_degree=sh_degree if vd else 0, near_plane=0.5)
                rgb = img[0].clamp(0, 1)
            client.scene.set_background_image((rgb.cpu().numpy() * 255).astype(np.uint8), format='jpeg',
                                              jpeg_quality=90)
            return
        with torch.no_grad():
            values = view_values(p if vd else no_lobe, vm.inverse()[None, :3, 3], sh_degree if vd else 0,
                                 torch.tensor([st['gain'].value], device='cuda'), source)[..., None]
            img, alpha, _ = rasterization(means, quats, scales, opac, values, vm[None], K[None], W, H,
                                          near_plane=0.5)
            v = img[0, ..., 0].clamp(0, 1)
            temp = to_celsius(v)
            valid = alpha[0, ..., 0] > 0.5
            if valid.any():
                tv = temp[valid]
                q = torch.quantile(tv[::max(1, tv.numel() // 100000)], torch.tensor([0.01, 0.995], device='cuda'))
                lo_c, bg_c, hi_c = float(q[0]), float(q[1]), float(tv.max())
            else:
                lo_c, bg_c, hi_c = st['lo'].value, st['hi'].value, st['hi'].value
            if st['auto'].value:
                # as the web viewer: 1%..99.5% over the bottom 70% of the palette, hotter up to the
                # peak log-compressed into the top 30%, so heat sources stand out instead of saturating
                lo = lo_c
                bg = max(bg_c, lo + 1)
                hi = max(hi_c, bg + 1)
                x = torch.where(temp <= bg, 0.7 * (temp - lo) / (bg - lo),
                                0.7 + 0.3 * torch.log1p((temp - bg).clamp(min=0) / 8) / math.log1p((hi - bg) / 8))
                info = 'display %.1f..%.1f C, hotter to %.1f C in the top of the scale' % (lo, bg, hi)
            else:
                lo, hi = st['lo'].value, max(st['hi'].value, st['lo'].value + 0.1)
                x = (temp - lo) / (hi - lo)
                info = 'display %.1f..%.1f C' % (lo, hi)
            rgb = luts[st['cmap'].value][(x.clamp(0, 1) * 255).long()] * alpha[0]
        st['stats'].value = 'peak %.1f C, %s' % (hi_c, info)
        client.scene.set_background_image((rgb.cpu().numpy() * 255).astype(np.uint8), format='jpeg',
                                          jpeg_quality=85)

    port = server.get_port()  # viser moves to a free port if the requested one is busy
    print('viewer at http://%s:%d' % ('localhost' if args.host in ('127.0.0.1', '0.0.0.0') else args.host, port),
          flush=True)
    while True:
        busy = False
        for client in list(server.get_clients().values()):
            st = sessions.get(client.client_id)
            if st is not None and st['dirty']:
                st['dirty'] = False
                with lock:
                    try:
                        render(client, st)
                    except Exception as e:  # keep serving other clients
                        print('render failed: %s' % e, flush=True)
                busy = True
        if not busy:
            time.sleep(0.02)


if __name__ == '__main__':
    main()
