#!/usr/bin/env python3
"""Live camera image processing for simple fixtures and optional map3d terrain.

The private socket carries one JSON pose/control request followed by four
length/keyframe headers (network-order uint32 pairs) and Annex-B access units.
Optional encodings have zero length when unused. Requested still images follow
as length-prefixed JPEGs. Only one request is outstanding at a time.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from functools import partial
from fractions import Fraction
import json
import math
import os
import queue
import socket
import struct
import sys
import time
from types import SimpleNamespace


def camera_vectors(roll, pitch, yaw):
    """Level-referenced gimbal roll/pitch, earth yaw, radians -> ENU forward/up."""
    sr, cr = math.sin(roll), math.cos(roll)
    sp, cp = math.sin(pitch), math.cos(pitch)
    sy, cy = math.sin(yaw), math.cos(yaw)
    return ((cp * sy, cp * cy, sp),
            (sr * cy - cr * sp * sy, -sr * sy - cr * sp * cy, cr * cp))


def set_camera_fov(camera, hfov, output_aspect, thermal_aspect=None):
    # The MT11 stretches its 5:4 thermal sensor to a 16:9 encoded image.
    # Preserve the sensor's rays and let the viewport stretch the pixels.
    aspect = thermal_aspect if thermal_aspect is not None else output_aspect
    camera.SetUseExplicitAspectRatio(thermal_aspect is not None)
    camera.SetExplicitAspectRatio(aspect)
    camera.SetViewAngle(math.degrees(2 * math.atan(math.tan(math.radians(hfov) / 2) / aspect)))


def env_int(name, default, minimum, maximum):
    value = int(os.environ.get(name, default))
    if not minimum <= value <= maximum:
        raise ValueError(f'{name} must be {minimum}..{maximum}')
    return value


def dependencies(terrain_mode=True):
    # Optional imports stay out of the ordinary SITL build and launcher.
    global av, np, vtk, numpy_support, terrain, mp_tile, camera_pose, ImageControls, apply_overlay
    import av
    import numpy as np
    import cv2
    if __package__:
        from .image_controls import ImageControls, apply_overlay
    else:
        from image_controls import ImageControls, apply_overlay
    cv2.setNumThreads(1)  # encoders already run in parallel; avoid CPU oversubscription
    if not terrain_mode:
        return
    import vtk
    from vtkmodules.util import numpy_support
    from MAVProxy.modules.mavproxy_map3d import terrain
    from MAVProxy.modules.mavproxy_map import mp_tile
    from MAVProxy.modules.lib.video_telemetry import camera_pose


class FrameResults(queue.Queue):
    """Limit VTK uploads per frame; downloads/decodes continue in the background."""
    def begin_frame(self):
        self.deadline = time.monotonic() + 0.008
        # VTK defers OpenGL uploads until Render(), so the CPU deadline alone
        # does not bound their cost. Spread new meshes/textures across frames.
        self.remaining = 1

    def get_nowait(self):
        if self.remaining <= 0 or time.monotonic() >= self.deadline:
            raise queue.Empty
        self.remaining -= 1
        return super().get_nowait()


def stable_texture_request(tile, fine, campos, screen_h, mt, fov_deg=30.0):
    """Use stable full-tile texture LODs instead of rebuilding each pixel size."""
    west, south, east, north = tile.bbox
    mid = (north + south) / 2
    distance = math.sqrt(sum((campos[i] - tile.center[i]) ** 2 for i in range(3)))
    gsd = max(0.3, distance * 2 * math.tan(math.radians(fov_deg) / 2) / screen_h)
    zoom_float = math.log2(2 * math.pi * terrain.R * math.cos(math.radians(mid)) / (gsd * 256))
    zoom = max(mt.min_zoom, min(mt.max_zoom, round(zoom_float)))
    wanted_width = math.radians(east - west) * terrain.R * math.cos(math.radians(mid)) / gsd
    maximum = 2048 if fine else 1024
    width = min(maximum, 2 ** math.ceil(math.log2(max(256, wanted_width))))
    if tile.tex_key is not None:
        old_zoom, old_width = tile.tex_key
        if old_width * 0.4 <= wanted_width <= old_width * 1.25:
            width = old_width
        if abs(zoom_float - old_zoom) < 0.75:
            zoom = old_zoom
    # Do not fetch detail that the output texture cannot represent.
    zoom = min(zoom, math.ceil(math.log2(360 * width / ((east - west) * 256))))
    key = (zoom, width)
    if key == tile.tex_key:
        return None
    return key, (west, south, east, north, zoom, width)


def make_manager(renderer, tiles, lat, lon, height, radius):
    class PrefetchTerrain(terrain.TerrainManager):
        vehicle = None
        ahead = None

        def update(self, tc):
            # map3d optimizes texture resolution for a settled view. Continuous
            # flight needs quantized LODs, not a new texture for each width.
            for key, tile in self.tiles.items():
                if 'texture_request' not in tile.__dict__:
                    tile.texture_request = partial(stable_texture_request, tile,
                                                   key[0] == self.zoom_fine)
            return super().update(tc)

        def _worker(self):
            # Keep map3d's network/mesh/cache pipeline, with larger close-range
            # drapes for video. Never fetch imagery finer than the output
            # texture: the map implementation would downsample it away.
            while not self.stop:
                try:
                    kind, jid, payload = self.jobs.get(timeout=0.2)
                except queue.Empty:
                    continue
                try:
                    if kind == 'decode':
                        self.results.put(('decode', jid, terrain.decode_terrain(*payload)))
                    else:
                        tilekey, tkey, params, priority = payload
                        west, south, east, north, zoom, width = params
                        useful_zoom = math.ceil(math.log2(360 * width / ((east - west) * 256)))
                        zoom = min(zoom, useful_zoom)
                        params = (west, south, east, north, zoom, width)
                        img = terrain.build_texture_image(self.mt, self.tex_lock, *params, priority)
                        self.results.put(('texture', jid, (img, (tilekey, tkey, params, priority))))
                except Exception as error:
                    self.results.put(('error', jid, error))

        def desired_set(self, lat, lon):
            # The LOD rings depend on tile coordinates, not each small pose
            # change. Rebuilding all three rings for every completed texture
            # used several milliseconds per frame at full resolution.
            if not hasattr(self, 'tile_sets'):
                self.tile_sets = {}
            want = set()
            for center in ((lat, lon), self.vehicle, self.ahead):
                if center is not None:
                    y, x = center
                    key = (self.g.LonLatToTile(x, y, self.zoom_fine),
                           self.zoom_fine, self.fine_radius, self.lod_min, self.ring)
                    if key not in self.tile_sets:
                        if len(self.tile_sets) >= 32:
                            self.tile_sets.clear()
                        self.tile_sets[key] = super().desired_set(*center)
                    want.update(self.tile_sets[key])
            return want

    manager = PrefetchTerrain(renderer, tiles, lat, lon, screen_h=height,
                              fine_radius=radius, lod_min=9, shading=False)
    # Workers start empty and use self.results at completion.
    manager.results = FrameResults()
    return manager


class MotionFilter:
    """Integrate velocity, gradually correcting small sample/arrival errors."""
    def __init__(self, limit, wrap=None):
        self.limit = np.asarray(limit)
        self.wrap = wrap
        self.value = None
        self.time = None

    def update(self, target, rates, now, sample_time, horizon):
        target = np.asarray(target, dtype=float)
        if self.value is None or now < self.time or now - self.time > 1:
            self.value, self.time = target.copy(), now
            return target.copy()
        # Stop both integration and corrections at the prediction horizon.
        dt = max(0, min(now, sample_time + horizon) - min(self.time, sample_time + horizon))
        predicted = self.value + np.asarray(rates) * dt
        error = target - predicted
        if self.wrap is not None:
            period = self.wrap
            error = (error + period / 2) % period - period / 2
        if np.any(np.abs(error) > self.limit):
            predicted = target.copy()  # teleport or deliberate large orientation change
        else:
            predicted += error * (-math.expm1(-dt / 0.15))
        self.value, self.time = predicted, now
        return predicted.copy()


class PosePredictor:
    """Predict the render-time pose without buffering future telemetry samples."""
    HORIZON = 0.25

    def __init__(self):
        self.history = {}
        self.filters = {}

    def angles(self, key, sample, now, horizon=None):
        if horizon is None:
            horizon = self.HORIZON
        angles = np.array([sample[k + '_rad'] for k in ('roll', 'pitch', 'yaw')])
        age = max(0, sample.get('age_ms', 0)) / 1000
        stamp = now - age
        previous = self.history.get(key)
        rates = np.zeros(3)
        if previous is not None:
            old_stamp, old_angles, rates = previous
            dt = stamp - old_stamp
            # Millisecond timestamp rounding can move the same sample slightly.
            if dt > 0.005:
                rates = ((angles - old_angles + math.pi) % (2 * math.pi) - math.pi) / dt
                if dt > 1.0:
                    rates = np.zeros(3)
                self.history[key] = (stamp, angles.copy(), rates)
            elif dt < -0.005:
                rates = np.zeros(3)
                self.history[key] = (stamp, angles.copy(), rates)
        else:
            self.history[key] = (stamp, angles.copy(), rates)
        rates = rates.copy()
        # AUTOPILOT_STATE supplies earth-frame yaw rate. ATTITUDE fallback is
        # converted from body rates by the MAVLink receiver.
        yaw_rate = sample.get('yaw_rate_rad_s')
        if isinstance(yaw_rate, (int, float)) and math.isfinite(yaw_rate):
            rates[2] = yaw_rate
        target = angles + rates * min(age, horizon)
        observer = self.filters.setdefault(key, MotionFilter([math.pi / 4] * 3, 2 * math.pi))
        predicted = observer.update(target, rates, now, stamp, horizon)
        return (predicted + math.pi) % (2 * math.pi) - math.pi

    def pose(self, record, now=None):
        pose = camera_pose(record)
        if pose is None:
            self.history.clear()
            self.filters.clear()
            return None
        if now is None:
            # Request PTS is sampled with the metadata, before IPC/render delay.
            # Use it to reconstruct stable sample times from their ages.
            pts = record.get('pts90k')
            now = (pts / 90000 if isinstance(pts, (int, float)) and math.isfinite(pts)
                   else time.monotonic())
        # The producer renders ahead of its scheduled presentation time.
        # Extend source ages, without changing the raw recorded telemetry.
        lead = max(0, min(self.HORIZON * 1000, record.get('prediction_ms', 0)))
        # The freshness allowance applies at request time. Rendering ahead
        # must not consume it: a 150 ms lead otherwise freezes 4 Hz position
        # prediction halfway between updates despite the link being healthy.
        horizon = self.HORIZON + lead / 1000
        record = dict(record)
        for key in ('position', 'vehicle_attitude', 'gimbal_attitude'):
            record[key] = dict(record[key], age_ms=record[key].get('age_ms', 0) + lead)
        lat, lon, alt = pose[:3]
        position = record['position']
        velocity = record.get('velocity') or {}
        speeds = [velocity.get(k) for k in ('vn_m_s', 've_m_s', 'vd_m_s')]
        if (all(isinstance(v, (int, float)) and math.isfinite(v) for v in speeds) and
                0 <= velocity.get('age_ms', 0) <= 10000):
            dt = min(max(0, position.get('age_ms', 0)) / 1000, horizon)
            north, east, down = speeds
            rates = [math.degrees(north / terrain.R),
                     math.degrees(east / (terrain.R * max(0.01, math.cos(math.radians(pose[0]))))),
                     -down]
            lat += rates[0] * dt
            lon += rates[1] * dt
            lon = (lon + 180) % 360 - 180
            lat = max(-90, min(90, lat))
            alt -= down * dt
            observer = self.filters.setdefault('position', MotionFilter([0.001, 0.001, 100]))
            lat, lon, alt = observer.update([lat, lon, alt], rates, now,
                                            now - position.get('age_ms', 0) / 1000, horizon)
        else:
            self.filters.pop('position', None)
        vehicle = self.angles('vehicle', record['vehicle_attitude'], now, horizon)
        gimbal = self.angles('gimbal', record['gimbal_attitude'], now, horizon)
        return (lat, lon, alt, math.degrees(gimbal[0]), math.degrees(gimbal[1]),
                math.degrees(gimbal[2] + vehicle[2]) % 360)


class Scene:
    def __init__(self, sizes, downloads=16, radius=2):
        self.predictor = PosePredictor()
        self.controls = ImageControls()
        self.sizes = sizes
        self.radius = radius
        self.ren = vtk.vtkRenderer()
        self.ren.SetBackground(0.35, 0.55, 0.8)
        self.window = vtk.vtkRenderWindow()
        self.window.SetOffScreenRendering(1)
        self.window.SetMultiSamples(0)
        self.window.AddRenderer(self.ren)
        self.window_size = (max(w for w, _ in sizes), max(h for _, h in sizes))
        self.window.SetSize(*self.window_size)
        self.capture = vtk.vtkWindowToImageFilter()
        self.capture.SetInput(self.window)
        self.capture.SetInputBufferTypeToRGB()
        self.capture.ReadFrontBufferOff()
        self.capture.ShouldRerenderOff()
        # Use map3d's tile fetching/cache implementation. ESRI is an additional
        # service here; an existing mp_tile service may be selected instead.
        mp_tile.TILE_SERVICES.setdefault('EsriWorldImagery',
            'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/${ZOOM}/${Y}/${X}')
        service = os.environ.get('CAMERA_GIMBAL_SITL_IMAGERY', 'EsriWorldImagery')
        self.tiles = mp_tile.MPTile(service=service, tile_delay=0.02,
                                    download_threads=downloads, cache_size=2048)
        self.manager = None
        self.previous = None
        self.rendered = {}
        self.last_update = 0
        self.last_redrape = 0
        self.tc = SimpleNamespace(cam=self.ren.GetActiveCamera(), pos=(0, 0, 0), focal=(0, 0, 0))
        print(f'SITL terrain: {terrain.QUANTIZED_BASE}; {service}; '
              f'{downloads} imagery and 6 terrain workers; prefetch radius {radius}',
              file=sys.stderr, flush=True)

    def update(self, record):
        self.rendered = {}
        pose = self.predictor.pose(record)
        if pose is None:
            return False
        lat, lon, alt, roll, pitch, yaw = pose
        if self.manager is None:
            self.manager = make_manager(self.ren, self.tiles, lat, lon,
                                        max(h for _, h in self.sizes), self.radius)
        manager = self.manager
        forward, up = camera_vectors(*map(math.radians, (roll, pitch, yaw)))
        pos = terrain.enu(lat, lon, alt, manager.lat0, manager.lon0)
        # Keep the visible ground and a ten-second flight corridor cached.
        now = time.monotonic()
        manager.vehicle = (lat, lon)
        manager.ahead = (lat, lon)
        if self.previous is not None:
            old_time, old_lat, old_lon = self.previous
            dt = now - old_time
            if dt > 0.1:
                dlat, dlon = lat - old_lat, lon - old_lon
                distance = math.hypot(math.radians(dlat) * terrain.R,
                                      math.radians(dlon) * terrain.R * math.cos(math.radians(lat)))
                # Teleports must not queue an unbounded corridor.
                factor = min(10 / dt, 3000 / max(1, distance))
                manager.ahead = (lat + dlat * factor, lon + dlon * factor)
        if self.previous is None or now - self.previous[0] >= 0.5:
            self.previous = (now, lat, lon)
        agl = max(20, (record.get('position') or {}).get('alt_relative_m', 100))
        distance = min(5000, max(100, agl / max(0.05, -forward[2])))
        self.tc.pos = pos
        self.tc.focal = tuple(pos[i] + forward[i] * distance for i in range(3))
        cam = self.tc.cam
        cam.SetPosition(*pos)
        cam.SetFocalPoint(*self.tc.focal)
        cam.SetViewUp(*up)
        # Terrain prefetch uses the normal viewport; a previous thermal render
        # must not leave the camera's narrower explicit aspect active here.
        cam.SetUseExplicitAspectRatio(False)
        cam.SetClippingRange(0.5, 150000)
        # Terrain's texture LOD uses a vertical FOV. Use the narrowest stream
        # so zooming either view requests enough imagery for both encoders.
        vfov = min(math.degrees(2 * math.atan(math.tan(math.radians(f) / 2) * h / w))
                   for f, (w, h) in zip(record['fov'], self.sizes))
        cam.SetViewAngle(vfov)
        if now - self.last_update >= 0.5:
            manager.update(self.tc)
            self.last_update = now
        # process() calculates the desired terrain set even with an empty
        # result queue. Avoid that work in the steady-state video loop.
        if (not manager.results.empty() or manager.redrape_pending or
                self.tiles.download_revision() != manager.download_revision):
            manager.results.begin_frame()
            manager.process(self.tc)
        # map3d refreshes placeholders after its complete download queue drains.
        # A flying camera keeps requesting tiles: publish completed imagery
        # progressively instead of waiting for distant prefetches to finish.
        if (manager.redrape_pending and now - self.last_redrape >= 1 and
                not any(jid[0] == 'T' for jid in manager.inflight)):
            for tile in manager.tiles.values():
                tile.tex_key = None
            manager.redrape_pending = False
            manager.update(self.tc)
            self.last_redrape = now
        return True

    def render(self, stream, record, valid):
        width, height = self.sizes[stream]
        if not valid:
            import cv2
            image = np.zeros((height, width, 3), dtype=np.uint8)
            cv2.putText(image, 'SITL: waiting for vehicle and gimbal telemetry',
                        (12, height // 2), cv2.FONT_HERSHEY_SIMPLEX,
                        width / 1100, (230, 230, 230), 1, cv2.LINE_AA)
            return image
        hfov, thermal = record['fov'][stream], record['thermal'][stream]
        cached = self.rendered.get((hfov, thermal))
        if cached is not None:
            old_width, old_height, image = cached
            if old_width >= width and old_height >= height:
                # A8 has two encodings of one sensor. Reuse the render, avoiding
                # a second VTK pass and framebuffer resize on every frame.
                import cv2
                return cv2.resize(image, (width, height), interpolation=cv2.INTER_AREA)
        # Keep the OpenGL framebuffer allocation stable across differently
        # sized RGB/thermal streams. Render/read only the requested viewport.
        viewport = (0, 0, width / self.window_size[0], height / self.window_size[1])
        self.ren.SetViewport(*viewport)
        self.capture.SetViewport(*viewport)
        set_camera_fov(self.tc.cam, hfov, width / height,
                       record.get('thermal_aspect', width / height) if thermal else None)
        self.window.Render()
        self.capture.Modified()
        self.capture.Update()
        image = numpy_support.vtk_to_numpy(self.capture.GetOutput().GetPointData().GetScalars())
        image = np.ascontiguousarray(image.reshape(height, width, 3)[::-1])
        image = self.controls.apply(image, record.get('image', {}), thermal)
        self.rendered[hfov, thermal] = (width, height, image)
        return image

    def close(self):
        if self.manager:
            self.manager.shutdown()
        self.window.Finalize()


class Fixture:
    """Loop a source video at its original rate, retaining only the current frame."""
    def __init__(self, path):
        self.path = path
        self.container = None
        self.index = -1
        self.open()

    def open(self):
        if self.container:
            self.container.close()
        self.container = av.open(self.path)
        stream = self.container.streams.video[0]
        stream.thread_count = 2
        self.rate = float(stream.average_rate or stream.guessed_rate or 10)
        self.frames = iter(self.container.decode(video=0))

    def at(self, seconds):
        target = int(seconds * self.rate)
        while self.index < target:
            frame = next(self.frames, None)
            if frame is None:
                self.open()
                frame = next(self.frames, None)
                if frame is None:
                    raise ValueError(f'Empty video fixture: {self.path}')
            self.image = frame.to_ndarray(format='rgb24')
            self.index += 1
        return self.image

    def close(self):
        self.container.close()


class FixtureScene:
    def __init__(self, sizes):
        self.sizes = sizes
        self.controls = ImageControls()
        self.manager = None
        self.tiles = SimpleNamespace(tiles_pending=lambda: 0)
        first = os.environ.get('CAMERA_APP_SITL_VIDEO1')
        second = os.environ.get('CAMERA_APP_SITL_VIDEO2') or first
        third = os.environ.get('CAMERA_APP_SITL_VIDEO3') or first
        if not first:
            raise ValueError('Simple video requires CAMERA_APP_SITL_VIDEO1')
        self.sources = {path: Fixture(path) for path in set((first, second, third))}
        self.paths = [first, second, third, first, second]
        self.started = None

    def update(self, record):
        pts = record['pts90k'] / 90000
        if self.started is None:
            self.started = pts
        self.images = {path: source.at(pts - self.started) for path, source in self.sources.items()}
        self.rendered = {}
        return True

    def render(self, stream, record, valid):
        import cv2
        width, height = self.sizes[stream]
        thermal = record['thermal'][stream]
        path = self.paths[stream]
        fov = record['fov'][stream]
        key = path, thermal, fov
        cached = self.rendered.get(key)
        if cached is not None and cached.shape[1] >= width and cached.shape[0] >= height:
            return cv2.resize(cached, (width, height), interpolation=cv2.INTER_AREA)
        image = self.images[path]
        if not thermal:
            ratio = max(1, math.tan(math.radians(record.get('base_fov', fov)) / 2) /
                        math.tan(math.radians(fov) / 2))
            h, w = image.shape[:2]
            cw, ch = max(1, round(w / ratio)), max(1, round(h / ratio))
            x, y = (w - cw) // 2, (h - ch) // 2
            image = image[y:y + ch, x:x + cw]
        image = cv2.resize(image, (width, height), interpolation=cv2.INTER_LINEAR)
        image = self.controls.apply(image, record.get('image', {}), thermal)
        self.rendered[key] = image
        return image

    def close(self):
        for source in self.sources.values():
            source.close()


class Encoder:
    def __init__(self, size, fps, codec='h264'):
        hevc = codec == 'h265'
        self.codec = av.CodecContext.create('libx265' if hevc else 'libx264', 'w')
        self.codec.width, self.codec.height = size
        self.codec.pix_fmt = 'yuv420p'
        self.codec.time_base = Fraction(1, 90000)
        self.codec.framerate = Fraction(fps)
        self.codec.thread_count = 2
        self.codec.options = {'preset': 'ultrafast', 'tune': 'zerolatency', 'crf': '23',
                              'x265-params' if hevc else 'x264-params':
                                  f'aud=1:repeat-headers=1:keyint={fps}:scenecut=0:bframes=0' +
                                  (':pools=none:frame-threads=1:log-level=error' if hevc else '')}
        self.codec.open()

    def encode(self, image, pts, force_key=False):
        frame = av.VideoFrame.from_ndarray(image, format='rgb24')
        frame.pts = pts
        frame.time_base = self.codec.time_base
        if force_key:
            frame.pict_type = av.video.frame.PictureType.I
        packets = self.codec.encode(frame)
        if not packets:
            raise RuntimeError('Video encoder buffered a frame despite zerolatency')
        data = b''.join(bytes(p) for p in packets)
        return struct.pack('!II', len(data), int(any(p.is_keyframe for p in packets))) + data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='check optional Python dependencies')
    parser.add_argument('--simple', action='store_true', help='check only software video dependencies')
    parser.add_argument('--fd', type=int)
    parser.add_argument('--connect', type=int, help='private loopback connection from Windows camera service')
    parser.add_argument('--token', default='')
    for stream in (1, 2, 3, 4, 5):
        parser.add_argument(f'--width{stream}', type=int, default=640)
        parser.add_argument(f'--height{stream}', type=int, default=360)
        parser.add_argument(f'--codec{stream}', choices=('h264', 'h265'), default='h264')
    parser.add_argument('--fps', type=int, default=20)
    args = parser.parse_args()
    terrain_mode = not args.simple and (args.check or bool(os.environ.get('CAMERA_APP_SITL_TERRAIN')))
    dependencies(terrain_mode)
    socket.setdefaulttimeout(10)  # also bounds mp_tile's background HTTP requests
    downloads = env_int('CAMERA_GIMBAL_SITL_TILE_THREADS', 16, 1, 64)
    radius = env_int('CAMERA_GIMBAL_SITL_PREFETCH_RADIUS', 2, 1, 4)
    env_int('CAMERA_GIMBAL_SITL_FPS', args.fps, 1, 60)
    if args.check:
        print('SITL video dependencies available' + (' (including terrain)' if terrain_mode else ''))
        return
    if (args.fd is None) == (args.connect is None):
        parser.error('exactly one of --fd and --connect is required')
    if args.connect is not None and (not 1 <= args.connect <= 65535 or len(args.token) != 32):
        parser.error('invalid private connection')
    sizes = [(args.width1, args.height1), (args.width2, args.height2),
             (args.width3, args.height3), (args.width4, args.height4), (args.width5, args.height5)]
    scene = Scene(sizes, downloads, radius) if terrain_mode else FixtureScene(sizes)
    encoders = [Encoder(size, args.fps, getattr(args, f'codec{i+1}')) for i, size in enumerate(sizes)]
    try:
        connection = socket.create_connection(('127.0.0.1', args.connect)) if args.connect else socket.socket(fileno=args.fd)
        if args.connect:
            connection.sendall(args.token.encode('ascii'))
        connection.sendall(b'R')  # source and codec setup succeeded
        with connection as sock, sock.makefile('rb') as requests, \
                ThreadPoolExecutor(max_workers=4, thread_name_prefix='video-encode') as pool:
            count, started = 0, time.monotonic()
            timings = []
            previous_swap = None
            previous_recording = False
            while True:
                line = requests.readline(4096)
                if not line:
                    break
                if not line.endswith(b'\n'):
                    raise ValueError('oversized pose request')
                frame_started = time.monotonic()
                record = json.loads(line)
                # The extra RGB encoding is needed when MT11 routes visible
                # video to the substream. Keep the RGB recording at main size.
                visible_sub = record['thermal'][1] and record.get('swap', False)
                record['fov'].extend([record['fov'][0], record['fov'][0], record['fov'][1]])
                record['thermal'].extend([False, False, record['thermal'][1]])
                valid = scene.update(record)
                force_key = record.get('swap') != previous_swap
                previous_swap = record.get('swap')
                active = [True, True, visible_sub, bool(record.get('recording')),
                          bool(record.get('recording')) and record['thermal'][1]]
                scene.controls.exposure = None
                futures = []
                for i in range(5):
                    pixels = scene.render(i, record, valid) if active[i] else None
                    if pixels is not None:
                        pixels = apply_overlay(pixels, record.get('overlays', [{}] * 5)[i])
                    if i == 0:  # RGB sensor, before other streams/still captures
                        exposure = scene.controls.exposure_packet()
                    futures.append(pool.submit(encoders[i].encode, pixels, record['pts90k'],
                                               force_key or (i >= 3 and not previous_recording)) if active[i] else None)
                for future in futures:
                    sock.sendall(future.result() if future else struct.pack('!II', 0, 0))
                sock.sendall(exposure)
                # Still images use the same current scene and image controls.
                # Render each requested lens without changing the live selection.
                for lens in range(3):
                    if not record.get('capture', 0) & (1 << lens):
                        continue
                    import cv2
                    stream = 1 if lens == 2 else 0
                    photo_record = dict(record, fov=list(record['fov']))
                    photo_record['fov'][stream] = record['capture_fov'][lens]
                    pixels = scene.render(stream, photo_record, valid)
                    ok, jpeg = cv2.imencode('.jpg', cv2.cvtColor(pixels, cv2.COLOR_RGB2BGR))
                    if not ok:
                        raise RuntimeError('SITL photo encoding failed')
                    sock.sendall(struct.pack('!I', jpeg.size) + jpeg.tobytes())
                previous_recording = active[3]
                timings.append((time.monotonic() - frame_started) * 1000)
                count += 1
                if count >= args.fps * 10:
                    elapsed = time.monotonic() - started
                    p95, maximum = np.percentile(timings, [95, 100])
                    print(f'SITL terrain: {count / elapsed:.1f} fps, '
                          f'render/encode p95={p95:.1f} max={maximum:.1f} ms, '
                          f'{len(scene.manager.tiles) if scene.manager else 0} meshes, '
                          f'{scene.tiles.tiles_pending()} imagery pending', file=sys.stderr, flush=True)
                    count, started = 0, time.monotonic()
                    timings.clear()
    finally:
        scene.close()


if __name__ == '__main__':
    try:
        main()
    except (ImportError, ValueError, RuntimeError) as error:
        sys.exit(f'SITL video: {error}. See sitl/requirements-video.txt '
                 '(plus requirements-terrain.txt for 3D imagery) and sitl/README.md')
