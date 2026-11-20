#!/usr/bin/env python3
"""Check terrain camera geometry, threaded loading, H.264, and optional live tiles."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import math
from pathlib import Path
import tempfile
import threading
import time
from unittest import mock

import terrain_video as video
from target_properties import TARGETS


def record():
    return {'pts90k': 0, 'utc_us': 1700000000000000, 'position': {'lat_e7': -353632610, 'lon_e7': 1491652300,
                                    'alt_amsl_m': 650, 'alt_relative_m': 70},
            'vehicle_attitude': {'roll_rad': 0, 'pitch_rad': 0, 'yaw_rad': 0},
            'gimbal_attitude': {'roll_rad': 0, 'pitch_rad': -math.pi / 4, 'yaw_rad': 0},
            'fov': [88, 24.2], 'thermal': [False, True], 'swap': False}


def geometry():
    np = video.np
    # The renderer must use the 16:9 crop of the MT11's 84-degree 4:3 sensor.
    camera = mock.Mock()
    video.set_camera_fov(camera, TARGETS['mt11']['lens1_fov_h'], 16 / 9)
    assert abs(camera.SetViewAngle.call_args.args[0] - 44.113793) < 1e-5
    forward, up = video.camera_vectors(0, 0, 0)
    np.testing.assert_allclose(forward, (0, 1, 0))  # north
    np.testing.assert_allclose(up, (0, 0, 1))
    forward, up = video.camera_vectors(0, -math.pi / 2, math.pi / 2)
    np.testing.assert_allclose(forward, (0, 0, -1), atol=1e-12)
    np.testing.assert_allclose(up, (1, 0, 0), atol=1e-12)
    for roll, pitch, yaw in ((0.3, -0.8, 2), (-1.2, 0.5, -2.8)):
        forward, up = video.camera_vectors(roll, pitch, yaw)
        assert abs(np.dot(forward, up)) < 1e-12
        assert abs(np.linalg.norm(up) - 1) < 1e-12
    pose = video.camera_pose(record())
    assert pose[4] == -45
    print('PASS N/E/U directions, nadir, roll and gimbal pose convention')


def prediction():
    import copy
    np = video.np
    predictor = video.PosePredictor()
    base = record()
    base['velocity'] = {'vn_m_s': 25, 've_m_s': 10, 'vd_m_s': -2, 'age_ms': 0}
    base['vehicle_attitude']['yaw_rate_rad_s'] = 0.2
    poses = []
    # A 4 Hz position source must still move on every 20 Hz video frame.
    for i in range(20):
        t = i * 0.05
        sampled = (i // 5) * 0.25
        state = copy.deepcopy(base)
        state['position']['lat_e7'] += math.degrees(25 * sampled / video.terrain.R) * 1e7
        state['position']['lon_e7'] += math.degrees(10 * sampled / (video.terrain.R * math.cos(math.radians(-35.363261)))) * 1e7
        state['position']['alt_amsl_m'] += 2 * sampled
        state['vehicle_attitude']['yaw_rad'] = 0.2 * sampled
        for key in ('position', 'velocity', 'vehicle_attitude', 'gimbal_attitude'):
            state[key]['age_ms'] = round((t - sampled) * 1000)
        poses.append(predictor.pose(state, now=100 + t))
    north_steps = np.diff([p[0] for p in poses]) * math.pi / 180 * video.terrain.R
    np.testing.assert_allclose(north_steps, 1.25, atol=1e-6)
    np.testing.assert_allclose(np.diff([p[2] for p in poses]), 0.1, atol=1e-6)
    np.testing.assert_allclose(np.diff([p[5] for p in poses]), math.degrees(0.01), atol=1e-6)
    # Loss freezes prediction at 250 ms, rather than drifting or jumping back.
    for key in ('position', 'velocity', 'vehicle_attitude', 'gimbal_attitude'):
        state[key]['age_ms'] = 250
    stopped = predictor.pose(state, now=101)
    for key in ('position', 'velocity', 'vehicle_attitude', 'gimbal_attitude'):
        state[key]['age_ms'] = 1500
    np.testing.assert_allclose(predictor.pose(state, now=102.25), stopped)
    state['position']['age_ms'] = 10001
    assert predictor.pose(state) is None
    # Estimate relative gimbal yaw across the +/- pi boundary by the short arc.
    predictor = video.PosePredictor()
    sample = {'roll_rad': 0, 'pitch_rad': 0, 'yaw_rad': math.radians(179), 'age_ms': 0}
    predictor.angles('gimbal', sample, 1)
    sample['yaw_rad'] = math.radians(-179)
    predictor.angles('gimbal', sample, 1.1)
    sample['age_ms'] = 50
    angle = predictor.angles('gimbal', sample, 1.15)[2]
    assert abs(math.degrees(angle) + 178) < 1e-6
    base['velocity']['vn_m_s'] = float('nan')
    base['position']['age_ms'] = 100
    assert video.PosePredictor().pose(base)[0] == video.camera_pose(base)[0]
    # Packet arrival jitter must not become alternating speed corrections.
    observer = video.MotionFilter([100])
    raw, filtered = [], []
    for i in range(100):
        now = i * 0.05
        target = 25 * now + (0.4 if (i // 2) % 2 else -0.4)
        raw.append(target)
        filtered.append(observer.update([target], [25], now, now, 0.25)[0])
    assert np.std(np.diff(filtered[20:])) < 0.3 * np.std(np.diff(raw[20:]))
    future = record()
    future['velocity'] = {'vn_m_s': 25, 've_m_s': 0, 'vd_m_s': 0}
    future['prediction_ms'] = 50
    pose = video.PosePredictor().pose(future, now=1)
    distance = math.radians(pose[0] - video.camera_pose(future)[0]) * video.terrain.R
    assert abs(distance - 1.25) < 1e-6
    predictor = video.PosePredictor()
    poses = []
    for i in range(40):
        now, sampled = i * 0.05, (i // 5) * 0.25
        state = copy.deepcopy(base)
        state['prediction_ms'] = 150
        state['velocity'] = {'vn_m_s': 25, 've_m_s': 0, 'vd_m_s': 0}
        state['position']['lat_e7'] += math.degrees(25 * sampled / video.terrain.R) * 1e7
        state['vehicle_attitude']['yaw_rad'] = 0.2 * sampled
        for key in ('position', 'vehicle_attitude', 'gimbal_attitude'):
            state[key]['age_ms'] = round((now - sampled) * 1000)
        poses.append(predictor.pose(state, now=100 + now + 0.15))
    north_steps = np.diff([p[0] for p in poses]) * math.pi / 180 * video.terrain.R
    np.testing.assert_allclose(north_steps, 1.25, atol=1e-6)
    np.testing.assert_allclose(np.diff([p[5] for p in poses]), math.degrees(0.01), atol=1e-6)
    # Still freeze after 250 ms of actual link staleness, plus the render lead.
    for key in ('position', 'vehicle_attitude', 'gimbal_attitude'):
        state[key]['age_ms'] = 250
    frozen = predictor.pose(state, now=102.15)
    for key in ('position', 'vehicle_attitude', 'gimbal_attitude'):
        state[key]['age_ms'] = 500
    np.testing.assert_allclose(predictor.pose(state, now=102.4), frozen)
    print('PASS 4 Hz -> 20 Hz prediction, bounded loss, yaw wrap, jitter correction and presentation lead')


def quantised_gimbal_prediction():
    np = video.np
    errors = []
    # MT11 rounds angles to 0.1 degree. Sparse rendering with a variable lead
    # amplifies the noise if yaw rate is differentiated from those angles.
    for measured_rate in (False, True):
        predictor = video.PosePredictor()
        yaw_errors = []
        for i in range(300):
            now = i * .147
            sampled = math.floor(now / .058) * .058
            lead = .15 if i % 3 else .08
            sample = {'roll_rad': 0, 'pitch_rad': 0,
                      'yaw_rad': math.radians(round(11 * sampled, 1)),
                      'age_ms': round((now - sampled + lead) * 1000)}
            if measured_rate:
                sample['yaw_rate_rad_s'] = math.radians(11)
            yaw = predictor.angles('gimbal', sample, now + lead, .25 + lead)[2]
            yaw_errors.append((math.degrees(yaw) - 11 * (now + lead) + 180) % 360 - 180)
        errors.append(np.array(yaw_errors[20:]))
    assert np.std(errors[1]) < .03
    assert max(abs(errors[1])) < .05
    assert np.std(np.diff(errors[1])) < .3 * np.std(np.diff(errors[0]))
    print('PASS measured gimbal yaw rate reduces quantisation-induced prediction jitter')


def stable_lod():
    from types import SimpleNamespace
    tile = SimpleNamespace(bbox=(149, -35.01, 149.01, -35),
                           center=(0, 0, 0), tex_key=None)
    imagery = SimpleNamespace(min_zoom=4, max_zoom=19)
    first = video.stable_texture_request(tile, True, (0, 0, 3000), 360, imagery)
    assert first is not None
    tile.tex_key = first[0]
    # Small camera movement used to rebuild differently sized textures.
    for distance in range(2980, 3021):
        assert video.stable_texture_request(tile, True, (0, 0, distance), 360, imagery) is None
    zoomed = video.stable_texture_request(tile, True, (0, 0, 3000), 360, imagery, 5)
    assert zoomed is not None and zoomed[0][1] > first[0][1]
    assert zoomed[0][1] in (256, 512, 1024, 2048)
    print('PASS stable texture LOD during flight and higher detail on zoom')


def synthetic():
    np = video.np
    active = peak = 0
    lock = threading.Lock()

    def decode(z, x, y):
        nonlocal active, peak
        with lock:
            active += 1
            peak = max(peak, active)
        try:
            time.sleep(0.02)  # deliberate network delay in each terrain worker
            w, s, e, n = video.terrain.GlobalGeodetic(True).TileBounds(x, y, z)
            return {'bbox': (w, s, e, n),
                    'verts': np.array([[w, s, 580], [e, s, 580], [w, n, 600], [e, n, 600]]),
                    'idx': np.array([[0, 1, 2], [1, 3, 2]])}
        finally:
            with lock:
                active -= 1

    def texture(mt, lock, w, s, e, n, zoom, width, priority=0):
        # Small, high-contrast synthetic texture avoids all network access.
        image = np.zeros((64, 64, 3), dtype=np.uint8)
        image[:] = (30, 100, 40)
        image[::4, :] = (180, 130, 30)
        return image

    with mock.patch.object(video.terrain, 'decode_terrain', decode), \
            mock.patch.object(video.terrain, 'build_texture_image', texture):
        scene = video.Scene([(320, 180), (160, 90)], downloads=16, radius=1)
        try:
            state = record()
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                scene.update(state)
                if scene.manager.tiles and not scene.manager.inflight:
                    break
                time.sleep(0.02)
            assert len(scene.manager.tiles) > 1
            assert peak >= 4, f'only {peak} parallel terrain workers'
            assert scene.tiles.download_threads == 16
            # Cached prefetch rings must still change when a tile boundary is
            # crossed, and callers must not mutate a cached ring through it.
            manager = scene.manager
            manager.vehicle = (-35.36, 149.16)
            manager.ahead = (-35.35, 149.17)
            for lat, lon in ((-35.36, 149.16), (-35.35999, 149.16), (-35.3, 149.3)):
                expected = set()
                for center in ((lat, lon), manager.vehicle, manager.ahead):
                    expected.update(video.terrain.TerrainManager.desired_set(manager, *center))
                assert manager.desired_set(lat, lon) == expected
                manager.desired_set(lat, lon).clear()
                assert manager.desired_set(lat, lon) == expected
            rgb = scene.render(0, state, True)
            ir = scene.render(1, state, True)
            assert rgb.shape == (180, 320, 3) and ir.shape == (90, 160, 3)
            assert scene.window.GetSize() == (640, 512)
            assert rgb.std() > 10
            np.testing.assert_array_equal(ir[:, :, 0], ir[:, :, 1])
            raw = scene.raw_thermal(state, True).copy()
            assert raw.shape == (512, 640) and raw.dtype == np.uint16
            assert raw.std() > 1 and raw.min() >= round(288.15*64) and raw.max() <= round(318.15*64)
            import cv2
            np.testing.assert_array_equal(ir, cv2.resize(scene.controls.thermal_display(raw, {}), (160, 90)))
            metadata = scene.thermal_metadata(state)
            np.testing.assert_allclose(metadata['gimbal_attitude']['pitch_rad'], math.radians(scene.pose[4]))
            assert metadata['utc_us'] == state['utc_us']
            # Controls are applied to terrain pixels as well as fixture video.
            state['image'] = {'brightness': 80, 'thermal_palette': 3}
            scene.update(state)
            brighter = scene.render(0, state, True)
            assert brighter.mean() > rgb.mean() + 30
            iron = scene.render(1, state, True)
            assert np.abs(iron[:, :, 0].astype(float) - iron[:, :, 2]).mean() > 10
            np.testing.assert_array_equal(scene.raw_thermal(state, True), raw)
            state['image'] = {'thermal_gain': 0, 'thermal_palette': 1}
            scene.update(state)
            scene.render(1, state, True)
            np.testing.assert_array_equal(scene.raw_thermal(state, True), raw)
            state['image'] = {'saturation': 0}
            scene.update(state)
            gray = scene.render(0, state, True)
            np.testing.assert_array_equal(gray[:, :, 0], gray[:, :, 2])
            state.pop('image')
            before = rgb.copy()
            state['gimbal_attitude']['yaw_rad'] = math.pi / 2
            scene.update(state)
            after = scene.render(0, state, True)
            assert np.mean(np.abs(before.astype(float) - after)) > 1
            assert np.mean(np.abs(scene.raw_thermal(state, True).astype(float) - raw)) > 1
            assert metadata['gimbal_attitude']['yaw_rad'] == 0  # snapshot stays frozen
            state['prediction_ms'] = 150
            state['velocity'] = {'vn_m_s': 10, 've_m_s': 0, 'vd_m_s': -2, 'age_ms': 0}
            state['vehicle_attitude']['yaw_rate_rad_s'] = 0.1
            scene.update(state)
            predicted = scene.thermal_metadata(state)
            assert predicted['utc_us'] == state['utc_us'] + 150000
            assert predicted['position']['lat_e7'] == round(scene.pose[0]*1e7)
            assert predicted['position']['alt_amsl_m'] == scene.pose[2]
            np.testing.assert_allclose(predicted['gimbal_attitude']['yaw_rad'] +
                                       predicted['vehicle_attitude']['yaw_rad'], math.radians(scene.pose[5]))
            state = record()
            ground = manager.height_at(state['position']['lat_e7'] / 1e7,
                                       state['position']['lon_e7'] / 1e7)
            assert ground is not None
            state['position']['alt_amsl_m'] = ground - 10
            # Relative-to-home altitude may remain positive below the mesh.
            state['image'] = {'brightness': 0, 'thermal_palette': 3}
            assert scene.update(state) and scene.below_ground
            with mock.patch.object(scene.window, 'Render', side_effect=AssertionError('rendered underground')):
                for stream, (width, height) in enumerate(scene.sizes):
                    underground = scene.render(stream, state, True)
                    assert underground.shape == (height, width, 3)
                    brown = np.all(underground == (139, 90, 43), axis=2)
                    assert brown.mean() > 0.9
                    assert np.any(np.all(underground > 200, axis=2))  # legible light text
                # The raw thermal view must not show the underside either.
                raw_below = scene.raw_thermal(state, True)
                assert raw_below.shape == (512, 640) and raw_below.dtype == np.uint16
                assert raw_below.std() > 1  # warning text survives conversion
            state['position']['alt_amsl_m'] = ground + 60
            state['position']['alt_relative_m'] = -10
            assert scene.update(state) and not scene.below_ground
            with mock.patch.object(manager, 'height_at', return_value=None):
                assert scene.update(state) and not scene.below_ground
            print('PASS below-ground warning, raw thermal, ascent recovery and terrain-height detection')
            state['position'] = None
            assert not scene.update(state)
            assert not scene.below_ground
            missing = scene.render(0, state, False)
            assert np.mean(missing) < 15  # stale terrain is replaced by waiting screen
            assert scene.thermal_metadata(state)['position'] is None
            assert scene.raw_thermal(state, False).std() == 0
            encoder = video.Encoder((320, 180), 10)
            decoder = video.av.CodecContext.create('h264', 'r')
            frames = []
            for i in range(5):
                data = encoder.encode(rgb, i * 9000, force_key=i == 3)
                size, key = video.struct.unpack('!II', data[:8])
                assert size == len(data) - 8
                if i in (0, 3):
                    assert key
                frames.extend(decoder.decode(video.av.Packet(data[8:])))
            assert len(frames) == 5
            print(f'PASS terrain loading ({peak} workers), camera motion, grayscale, '
                  'image controls, stale telemetry, H.264 and forced source-switch keyframes')
        finally:
            scene.close()
            for worker in scene.manager.workers:
                worker.join(2)


def network(output, seconds, fps, width, height, a8):
    """Real imagery/mesh smoke test and paced two-stream render/encode benchmark."""
    import cv2
    np = video.np
    scene = video.Scene([(width, height)] * 2)
    encoders = [video.Encoder((width, height), fps) for _ in range(2)]
    output.mkdir(parents=True, exist_ok=True)
    frames, times, intervals = 0, [], []
    started = previous = time.monotonic()
    state = record()
    if a8:
        state['thermal'] = [False, False]
        state['fov'] = [88, 88]
    with ThreadPoolExecutor(max_workers=2) as pool:
        try:
            while time.monotonic() - started < seconds:
                begin = time.monotonic()
                state['pts90k'] = frames * (90000 // fps)
                # Move north at 10 m/s while looking down over the terrain.
                state['position']['lat_e7'] = -353632610 + int((begin - started) * 900)
                valid = scene.update(state)
                images = [scene.render(i, state, valid) for i in range(2)]
                futures = [pool.submit(encoders[i].encode, images[i], state['pts90k']) for i in range(2)]
                for f in futures:
                    assert f.result()
                if begin - started > 10:
                    times.append(time.monotonic() - begin)
                    intervals.append(begin - previous)
                previous = begin
                frames += 1
                time.sleep(max(0, 1 / fps - (time.monotonic() - begin)))
            for i, image in enumerate(images):
                cv2.imwrite(str(output / f'video{i + 1}.png'), image[:, :, ::-1])
            assert scene.manager.tiles, 'no terrain meshes downloaded'
            assert any(t.actor.GetTexture() for t in scene.manager.tiles.values()), 'no imagery draped'
            stats = {'frames': frames, 'seconds': time.monotonic() - started,
                     'steady_fps': 1 / float(np.mean(intervals)),
                     'render_encode_ms_p50_p95_max': (np.percentile(times, [50, 95, 100]) * 1000).tolist(),
                     'meshes': len(scene.manager.tiles), 'imagery_pending': scene.tiles.tiles_pending()}
            (output / 'timing.json').write_text(json.dumps(stats, indent=2) + '\n')
            print(json.dumps(stats), flush=True)
        finally:
            scene.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--network', action='store_true')
    parser.add_argument('--output', type=Path, default=Path(tempfile.gettempdir()) / 'sitl-terrain-test')
    parser.add_argument('--seconds', type=int, default=30)
    parser.add_argument('--fps', type=int, default=20)
    parser.add_argument('--width', type=int, default=640)
    parser.add_argument('--height', type=int, default=360)
    parser.add_argument('--a8', action='store_true', help='both encodings use the visible sensor')
    args = parser.parse_args()
    video.dependencies()
    video.socket.setdefaulttimeout(10)
    geometry()
    prediction()
    quantised_gimbal_prediction()
    stable_lod()
    synthetic()
    if args.network:
        network(args.output, max(15, args.seconds), args.fps, args.width, args.height, args.a8)
