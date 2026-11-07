import { CESIUM_VERSION } from '../versions.js';
import {
  Viewer,
  Ion,
  SceneMode,
  Color,
  Cartesian3,
  Cartesian2,
  Cartographic,
  Math as CesiumMath,
  EllipsoidTerrainProvider,
  CesiumTerrainProvider,
  OpenStreetMapImageryProvider,
  UrlTemplateImageryProvider,
  DiscardMissingTileImagePolicy,
  Google2DImageryProvider,
  PolylineDashMaterialProperty,
  HeightReference,
  LabelStyle,
  BoundingSphere,
  Request,
  ScreenSpaceEventType,
  Transforms,
  Matrix4,
  EllipsoidalOccluder,
  CallbackProperty,
  Primitive,
  GeometryInstance,
  PolylineGeometry,
  PolylineColorAppearance,
  PointPrimitiveCollection,
  BlendOption,
} from 'cesium';
import 'cesium/Build/Cesium/Widgets/widgets.css';
import { freshMapPosition, validateMapConfiguration } from './map-config.js';
import { finite } from '../domain/time.js';
import { missionSegments, resolveMissionAltitude } from '../domain/flight-log.js';
import { TerrainElevation } from './terrain-elevation.js';
import { VideoProjection } from './video-projection.js';
import {
  buildVideoTrack,
  buildVideoTrackAsync,
  nearestTrackTime,
  trackTemperatureRange,
  trackTemperatureColor,
} from '../domain/video-track.js';

globalThis.CESIUM_BASE_URL = `${import.meta.env.BASE_URL}vendor/cesium/${CESIUM_VERSION}/`;
Ion.defaultAccessToken = '';
const color = {
  aircraft: Color.CYAN,
  track: Color.CYAN.withAlpha(0.6),
  mission: Color.LIME,
  inclusion: Color.ORANGE,
  exclusion: Color.RED,
  footprint: Color.YELLOW,
  finding: Color.MAGENTA,
};
export class ReviewMap {
  constructor(
    container,
    onStatus = () => {},
    onTerrainChange = () => {},
    onTrackSeek = () => {},
    onPointerHeight = () => {},
  ) {
    this.onStatus = onStatus;
    this.onPointerHeight = onPointerHeight;
    this.pointerPosition = null;
    this.pointerFrame = null;
    this.pointerTimer = null;
    this.pointerSettled = false;
    this.closed = false;
    this.generation = 0;
    this.offset = 0;
    this.configuration = { imagery: 'none', terrainUrl: '', amslToEllipsoidMetres: 0 };
    this.terrain = new TerrainElevation(
      () => {
        this.videoProjection?.invalidate();
        onTerrainChange();
        this.refreshPointerHeight();
      },
      () => new Request({ throttle: false }),
    );
    this.focused = false;
    this.follow = true;
    this.currentPosition = null;
    this.followAnchor = null;
    this.videoTrack = [];
    this.videoPts = 0;
    this.logEntities = [];
    this.flightEntities = [];
    this.findingEntities = [];
    this.viewer = new Viewer(container, {
      animation: false,
      timeline: false,
      baseLayerPicker: false,
      baseLayer: false,
      geocoder: false,
      homeButton: false,
      fullscreenButton: false,
      navigationHelpButton: false,
      sceneModePicker: false,
      infoBox: false,
      selectionIndicator: false,
      requestRenderMode: true,
      maximumRenderTimeChange: Infinity,
      terrainProvider: new EllipsoidTerrainProvider(),
      sceneMode: SceneMode.SCENE2D,
    });
    this.viewer.scene.globe.baseColor = Color.fromCssColorString('#213847');
    this.videoProjection = new VideoProjection(this.viewer.scene, onTerrainChange);
    this.viewer.scene.backgroundColor = Color.fromCssColorString('#101b24');
    this.viewer.scene.globe.depthTestAgainstTerrain = true;
    this.aircraft = this.viewer.entities.add({
      point: {
        pixelSize: 12,
        color: color.aircraft,
        outlineColor: Color.BLACK,
        outlineWidth: 2,
        disableDepthTestDistance: Infinity,
      },
    });
    this.hover = this.viewer.entities.add({
      point: {
        pixelSize: 9,
        color: Color.WHITE,
        outlineColor: Color.BLACK,
        outlineWidth: 2,
        disableDepthTestDistance: Infinity,
      },
      show: false,
    });
    this.footprintPieces = [];
    this.footprintCoordinates = [];
    this.footprint = this.createFootprintPiece();
    this.viewer.camera.setView({ destination: Cartesian3.fromDegrees(148.838, -35.322, 3500) });
    this.viewer.scene.renderError.addEventListener((_scene, error) =>
      this.onStatus(`Map rendering: ${error.message}`),
    );
    this.dragging = false;
    this.pointerDown = () => {
      this.dragging = true;
    };
    this.pointerUp = () => {
      this.dragging = false;
    };
    this.viewer.scene.canvas.addEventListener('pointerdown', this.pointerDown);
    window.addEventListener('pointerup', this.pointerUp);
    window.addEventListener('pointercancel', this.pointerUp);
    this.viewer.scene.canvas.addEventListener('lostpointercapture', this.pointerUp);
    document.addEventListener('visibilitychange', this.pointerUp);
    window.addEventListener('blur', this.pointerUp);
    const handler = this.viewer.screenSpaceEventHandler;
    const originalClick = handler.getInputAction(ScreenSpaceEventType.LEFT_CLICK);
    handler.setInputAction((event) => {
      const hit = this.pickVideoTrack(event.position);
      if (hit) onTrackSeek(hit.pts)?.catch?.(this.onStatus);
      else originalClick?.(event);
    }, ScreenSpaceEventType.LEFT_CLICK);
    // Tracking is owned by the Follow checkbox, not the SDK's double-click.
    handler.removeInputAction(ScreenSpaceEventType.LEFT_DOUBLE_CLICK);
    this.pointerMove = (event) => {
      const rect = this.viewer.scene.canvas.getBoundingClientRect();
      this.pointerPosition = new Cartesian2(event.clientX - rect.left, event.clientY - rect.top);
      this.pointerSettled = false;
      clearTimeout(this.pointerTimer);
      // Pointer sampling shares the video terrain cache. A moving pointer must
      // not queue a tile for every crossed location on a slow connection.
      this.pointerTimer = setTimeout(() => {
        this.pointerTimer = null;
        this.pointerSettled = true;
        this.refreshPointerHeight();
      }, 180);
      if (this.pointerFrame === null)
        this.pointerFrame = requestAnimationFrame(() => {
          this.pointerFrame = null;
          this.refreshPointerHeight();
        });
    };
    this.pointerLeave = () => {
      clearTimeout(this.pointerTimer);
      this.pointerTimer = null;
      this.pointerSettled = false;
      this.pointerPosition = null;
      this.onPointerHeight(null);
    };
    this.viewer.scene.canvas.addEventListener('pointermove', this.pointerMove);
    this.viewer.scene.canvas.addEventListener('pointerleave', this.pointerLeave);
    // Re-pick stationary pointers when Follow, pan, zoom or terrain changes the view.
    this.pointerRender = () => this.refreshPointerHeight();
    this.viewer.scene.postRender.addEventListener(this.pointerRender);
  }
  refreshPointerHeight() {
    if (this.closed || !this.viewer || !this.pointerPosition) return;
    if (!this.pointerSettled) {
      this.onPointerHeight({ heightAmslM: null, reason: 'waiting for pointer to stop' });
      return;
    }
    const scene = this.viewer.scene;
    if (scene.mode === SceneMode.MORPHING) {
      this.onPointerHeight({ heightAmslM: null, reason: 'map changing mode' });
      return;
    }
    const ray = this.viewer.camera.getPickRay(this.pointerPosition);
    const hit =
      ray &&
      (scene.globe.pick(ray, scene) ||
        this.viewer.camera.pickEllipsoid(this.pointerPosition, scene.globe.ellipsoid));
    if (!hit) {
      this.onPointerHeight({ heightAmslM: null, reason: 'pointer outside terrain' });
      return;
    }
    const position = Cartographic.fromCartesian(hit),
      lat = CesiumMath.toDegrees(position.latitude),
      lon = CesiumMath.toDegrees(position.longitude),
      heightAmslM = this.terrain.at(lat, lon);
    // at() queues missing meshes; their callback samples the current pointer,
    // never a stale latitude/longitude captured before an asynchronous load.
    this.onPointerHeight({
      lat,
      lon,
      heightAmslM,
      reason: finite(heightAmslM) ? null : this.terrain.lastIssue?.reason || 'terrain unavailable',
    });
  }
  render() {
    this.viewer.scene.requestRender();
  }
  createFootprintPiece() {
    const index = this.footprintPieces.length;
    this.footprintCoordinates.push([]);
    // Constant positions cause asynchronous geometry rebuilds on every video
    // frame. A dynamic line keeps the existing primitive visible while moving.
    const piece = this.viewer.entities.add({
      polyline: {
        positions: new CallbackProperty(() => this.footprintCoordinates[index], false),
        width: 2,
        material: color.footprint,
      },
      show: false,
    });
    this.footprintPieces.push(piece);
    return piece;
  }
  zoom(inward) {
    const scene = this.viewer.scene,
      camera = this.viewer.camera;
    if (scene.mode === SceneMode.MORPHING) return;
    camera.cancelFlight();
    let distance;
    if (scene.mode === SceneMode.SCENE2D) {
      const f = camera.frustum;
      distance = Math.max(f.right - f.left, f.top - f.bottom);
    } else {
      const center = new Cartesian2(scene.canvas.clientWidth / 2, scene.canvas.clientHeight / 2);
      const ray = camera.getPickRay(center);
      const target =
        ray &&
        (scene.globe.pick(ray, scene) || camera.pickEllipsoid(center, scene.globe.ellipsoid));
      distance = target
        ? Cartesian3.distance(camera.positionWC, target)
        : Math.max(10, camera.positionCartographic.height);
    }
    const next = Math.max(
      10,
      Math.min(scene.globe.ellipsoid.maximumRadius * 8, distance * (inward ? 0.75 : 1 / 0.75)),
    );
    camera.zoomIn(distance - next);
    this.render();
  }
  position(lat, lon, amsl = 0) {
    return Cartesian3.fromDegrees(lon, lat, amsl + this.offset);
  }
  setMode(mode) {
    this.dragging = false;
    if (mode === '3d') this.viewer.scene.morphTo3D(0);
    else this.viewer.scene.morphTo2D(0);
    if (this.follow) this.centerCurrent();
    this.render();
  }
  async configure(config) {
    const own = ++this.generation;
    const validated = validateMapConfiguration(config, location.href);
    const dispose = (provider) => {
      if (provider?.destroy && !provider.isDestroyed?.()) provider.destroy();
    };
    // Prepare both independently and wait for both even on failure, so a late
    // provider cannot leak or partially replace the active configuration.
    const prepared = await Promise.allSettled([
      (async () => {
        let provider = null;
        if (validated.imagery === 'osm')
          provider = new OpenStreetMapImageryProvider({ url: 'https://tile.openstreetmap.org/' });
        if (validated.imagery === 'satellite')
          provider = new UrlTemplateImageryProvider({
            url: 'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}',
            credit: 'Esri, Maxar, Earthstar Geographics',
            maximumLevel: 23,
            enablePickFeatures: false,
            // Esri returns HTTP-200 placeholder JPEGs beyond local coverage.
            // Match the same reference tile/pixels as Cesium's ArcGIS provider
            // so it retains valid ancestor imagery. Avoid a metadata request:
            // imagery outages must not prevent terrain/datum configuration.
            tileDiscardPolicy: new DiscardMissingTileImagePolicy({
              missingImageUrl:
                'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/23/0/0',
              pixelsToCheck: [
                [0, 0],
                [200, 20],
                [20, 200],
                [80, 110],
                [160, 130],
              ].map(([x, y]) => new Cartesian2(x, y)),
              disableCheckIfAllPixelsAreTransparent: true,
            }),
          });
        if (validated.imagery === 'google')
          provider = await Google2DImageryProvider.fromUrl({
            key: validated.googleMapsApiKey,
            mapType: 'satellite',
            language: 'en-AU',
            region: 'AU',
          });
        if (provider?.readyPromise) {
          try {
            await provider.readyPromise;
          } catch (error) {
            dispose(provider);
            throw error;
          }
        }
        return provider;
      })(),
      (async () =>
        validated.terrainUrl
          ? CesiumTerrainProvider.fromUrl(new URL(validated.terrainUrl, location.href).href)
          : new EllipsoidTerrainProvider())(),
    ]);
    const imagery = prepared[0].status === 'fulfilled' ? prepared[0].value : null;
    const terrain = prepared[1].status === 'fulfilled' ? prepared[1].value : null;
    if (own !== this.generation) {
      dispose(imagery);
      dispose(terrain);
      return false;
    }
    const failure = prepared.find((result) => result.status === 'rejected');
    if (failure) {
      dispose(imagery);
      dispose(terrain);
      throw failure.reason;
    }
    const layers = this.viewer.imageryLayers;
    const previousLayers = Array.from({ length: layers.length }, (_, i) => layers.get(i));
    const previousTerrain = this.viewer.terrainProvider;
    let nextLayer = null;
    try {
      // No await within commit: old layers and datum remain active throughout
      // provider preparation, including rejected/CORS-failed terrain requests.
      if (imagery) nextLayer = layers.addImageryProvider(imagery);
      this.viewer.terrainProvider = terrain;
    } catch (error) {
      if (nextLayer) layers.remove(nextLayer, true);
      this.viewer.terrainProvider = previousTerrain;
      dispose(imagery);
      dispose(terrain);
      throw error;
    }
    for (const layer of previousLayers) layers.remove(layer, true);
    // The plot tiles store orthometric SRTM heights. Draw AMSL entities in
    // that same numeric reference; ellipsoidal datasets use the geoid offset.
    this.offset =
      validated.terrainUrl && validated.terrainDatum === 'amsl'
        ? 0
        : validated.amslToEllipsoidMetres;
    this.configuration = { ...validated };
    this.terrain.configure(terrain, validated);
    this.render();
    return true;
  }
  focus(lat, lon, height = 3500) {
    this.viewer.camera.setView({ destination: Cartesian3.fromDegrees(lon, lat, height) });
    this.focused = true;
    this.render();
  }
  setFollow(enabled) {
    this.dragging = false;
    this.follow = !!enabled;
    if (this.follow) this.centerCurrent();
  }
  centerCurrent() {
    if (this.currentPosition) this.followPosition(this.currentPosition, true);
  }
  followPosition(position, force = false) {
    const camera = this.viewer.camera,
      scene = this.viewer.scene;
    const target = this.position(position.lat, position.lon, position.altMsl);
    if (
      !force &&
      this.followAnchor &&
      Cartesian3.equalsEpsilon(target, this.followAnchor, 0, 0.001)
    )
      return;
    if (scene.mode === SceneMode.MORPHING || this.dragging) return;
    if (scene.mode === SceneMode.SCENE2D) {
      camera.setView({
        destination: Cartesian3.fromDegrees(
          position.lon,
          position.lat,
          camera.positionCartographic.height,
        ),
        orientation: { heading: camera.heading },
      });
    } else {
      const screen = this.followAnchor && scene.cartesianToCanvasCoordinates(this.followAnchor);
      const center = new Cartesian2(scene.canvas.clientWidth / 2, scene.canvas.clientHeight / 2);
      const anchored = screen && Cartesian2.distance(screen, center) < 2;
      const ray = camera.getPickRay(center);
      const viewCenter =
        ray &&
        (scene.globe.pick(ray, scene) || camera.pickEllipsoid(center, scene.globe.ellipsoid));
      const oldCenter = anchored
        ? this.followAnchor
        : viewCenter ||
          Cartesian3.add(
            camera.positionWC,
            Cartesian3.multiplyByScalar(
              camera.directionWC,
              Math.max(1, camera.positionCartographic.height),
              new Cartesian3(),
            ),
            new Cartesian3(),
          );
      const oldFrame = Transforms.eastNorthUpToFixedFrame(oldCenter);
      const newFrame = Transforms.eastNorthUpToFixedFrame(target);
      const move = Matrix4.multiply(
        newFrame,
        Matrix4.inverseTransformation(oldFrame, new Matrix4()),
        new Matrix4(),
      );
      camera.setView({
        destination: Matrix4.multiplyByPoint(move, camera.positionWC, new Cartesian3()),
        orientation: {
          direction: Matrix4.multiplyByPointAsVector(move, camera.directionWC, new Cartesian3()),
          up: Matrix4.multiplyByPointAsVector(move, camera.upWC, new Cartesian3()),
        },
      });
    }
    this.followAnchor = Cartesian3.clone(target);
    this.render();
  }
  update(record, footprint = [], videoPts = null) {
    const position = freshMapPosition(record);
    this.currentPosition = position;
    if (finite(videoPts)) this.videoPts = videoPts;
    this.aircraft.show = !!position;
    if (position) {
      this.aircraft.position = this.position(position.lat, position.lon, position.altMsl);
      if (this.follow) this.followPosition(position);
    }
    const segments = Array.isArray(footprint)
      ? footprint.length === 4 && footprint.every(Boolean)
        ? [[...footprint, footprint[0]]]
        : []
      : footprint.segments;
    for (let i = 0; i < Math.max(segments.length, this.footprintPieces.length); i++) {
      if (!this.footprintPieces[i]) this.createFootprintPiece();
      const piece = this.footprintPieces[i],
        points = segments[i];
      piece.show = !!points?.length;
      this.footprintCoordinates[i] = points
        ? points.map((p) => this.position(p.lat, p.lon, p.altMsl + 1))
        : [];
    }
    this.render();
  }
  setHover(point) {
    this.hover.show = !!point;
    if (point) this.hover.position = this.position(point.lat, point.lon, point.altMsl + 2);
    this.render();
  }
  clearEntities(collection) {
    for (const e of collection) this.viewer.entities.remove(e);
    collection.length = 0;
  }
  drawTrack(points, collection = this.flightEntities, tint = color.track, encodedSegments = false) {
    const valid = points.filter((p) => finite(p.lat) && finite(p.lon) && finite(p.altMsl));
    // Break at long gaps rather than drawing a fictitious ground path.
    let segment = [];
    const commit = () => {
      if (segment.length > 1)
        collection.push(
          this.viewer.entities.add({ polyline: { positions: segment, width: 2, material: tint } }),
        );
      segment = [];
    };
    let previous = null;
    for (const p of valid) {
      if (
        previous &&
        (encodedSegments
          ? p.clockSegment !== previous.clockSegment
          : (p.utcUs && previous.utcUs && p.utcUs - previous.utcUs > 1e7) ||
            (p.clockSegment !== undefined &&
              previous.clockSegment !== undefined &&
              p.clockSegment !== previous.clockSegment) ||
            Math.abs(p.lat - previous.lat) > 0.1 ||
            Math.abs(p.lon - previous.lon) > 0.1)
      )
        commit();
      segment.push(this.position(p.lat, p.lon, p.altMsl));
      previous = p;
    }
    commit();
    this.render();
  }
  async setVideoTrack(frames, recordForFrame) {
    this.trackController?.abort();
    const controller = new AbortController();
    this.trackController = controller;
    this.videoTrack = [];
    try {
      const track =
        frames.length > 20000
          ? await buildVideoTrackAsync(frames.slice(), recordForFrame, 4000, controller.signal)
          : buildVideoTrack(frames, recordForFrame);
      if (controller.signal.aborted) return false;
      this.clearEntities(this.flightEntities);
      this.clearTemperatureTrack();
      this.trackLimited = !!track.limited;
      this.videoTrack = track.map((p) => ({ ...p, world: this.position(p.lat, p.lon, p.altMsl) }));
      this.trackTemperatureRange = trackTemperatureRange(track);
      if (this.trackTemperatureRange) this.drawTemperatureTrack();
      else this.drawTrack(this.videoTrack, this.flightEntities, color.track, true);
      return true;
    } catch (e) {
      if (e.name === 'AbortError') return false;
      throw e;
    }
  }
  clearTemperatureTrack() {
    if (this.temperatureTrack) this.viewer.scene.primitives.remove(this.temperatureTrack);
    if (this.temperaturePoints) this.viewer.scene.primitives.remove(this.temperaturePoints);
    this.temperatureTrack = null;
    this.temperaturePoints = null;
    this.trackTemperatureRange = null;
  }
  drawTemperatureTrack() {
    const instances = [];
    let positions = [],
      colors = [],
      previous = null,
      thermal = null;
    const commit = () => {
      if (positions.length > 1)
        instances.push(
          new GeometryInstance({
            geometry: new PolylineGeometry({
              positions,
              colors,
              colorsPerVertex: true,
              width: 3,
              vertexFormat: PolylineColorAppearance.VERTEX_FORMAT,
            }),
          }),
        );
      positions = [];
      colors = [];
    };
    const tint = (point) => {
      const rgb = thermal && trackTemperatureColor(point.tmaxC, this.trackTemperatureRange);
      return rgb ? Color.fromBytes(...rgb) : Color.CYAN;
    };
    for (const point of this.videoTrack) {
      if (previous && point.clockSegment === previous.clockSegment) {
        const available = finite(previous.tmaxC) && finite(point.tmaxC);
        // Missing temperatures remain cyan; don't interpolate across them.
        if (available !== thermal) commit();
        thermal = available;
        if (!positions.length) {
          positions.push(previous.world);
          colors.push(tint(previous));
        }
        positions.push(point.world);
        colors.push(tint(point));
      } else commit();
      previous = point;
    }
    commit();
    if (instances.length)
      this.temperatureTrack = this.viewer.scene.primitives.add(
        new Primitive({
          geometryInstances: instances,
          appearance: new PolylineColorAppearance({ translucent: false }),
          // One batched primitive keeps long tracks out of the entity updater.
          asynchronous: false,
          allowPicking: false,
        }),
      );
    // A short peak can be smaller than a screen pixel, or underneath another
    // pass at a higher altitude. Keep each sampled temperature visible on the
    // track, drawing the hottest samples last where passes overlap.
    this.temperaturePoints = this.viewer.scene.primitives.add(
      // One alpha-blended pass preserves sample order and leaves the depth
      // buffer available for terrain occlusion checks during track seeking.
      new PointPrimitiveCollection({ blendOption: BlendOption.TRANSLUCENT }),
    );
    const samples = this.videoTrack
      .filter((p) => finite(p.tmaxC))
      .sort((a, b) => a.tmaxC - b.tmaxC);
    for (const point of samples)
      this.temperaturePoints.add({
        position: point.world,
        color: Color.fromBytes(...trackTemperatureColor(point.tmaxC, this.trackTemperatureRange)),
        pixelSize: 4,
        disableDepthTestDistance: Infinity,
      });
    this.render();
  }
  pickVideoTrack(click) {
    const scene = this.viewer.scene;
    const occluder =
      scene.mode === SceneMode.SCENE3D
        ? new EllipsoidalOccluder(scene.globe.ellipsoid, this.viewer.camera.positionWC)
        : null;
    const hit = nearestTrackTime(
      this.videoTrack,
      (point) => {
        if (occluder && !occluder.isPointVisible(point.world)) return null;
        return scene.cartesianToCanvasCoordinates(point.world);
      },
      click,
      8,
      this.videoPts,
    );
    if (hit && scene.mode === SceneMode.SCENE3D && scene.pickPositionSupported) {
      const front = scene.pickPosition(new Cartesian2(hit.x, hit.y));
      const target = Cartesian3.lerp(
        hit.start.world,
        hit.end.world,
        hit.fraction,
        new Cartesian3(),
      );
      const distance = Cartesian3.distance(this.viewer.camera.positionWC, target);
      if (
        front &&
        Cartesian3.distance(this.viewer.camera.positionWC, front) + Math.max(3, distance * 0.005) <
          distance
      )
        return null;
    }
    return hit;
  }
  setLog(log, elevation) {
    this.clearEntities(this.logEntities);
    this.drawTrack(log.track || [], this.logEntities, Color.WHITE.withAlpha(0.7));
    const mission = log.mission?.items || [],
      home = log.home;
    const heights =
      elevation.terrain?.samplePoints(
        mission
          .filter((item) => item.spatial && item.position?.altitude?.reference === 'terrain')
          .map((item) => item.position),
      ) || new Map();
    let line = [];
    const flush = () => {
      if (line.length > 1)
        this.logEntities.push(
          this.viewer.entities.add({
            polyline: {
              positions: line,
              width: 2,
              material: new PolylineDashMaterialProperty({ color: color.mission }),
            },
          }),
        );
      line = [];
    };
    for (const segment of log.mission?.segments || missionSegments(mission)) {
      flush();
      for (const item of segment) {
        const p = item.position;
        if (!p || !item.spatial) continue;
        const resolvedAltitude = resolveMissionAltitude(item, {
          home,
          terrainMsl: heights.get(p) ?? null,
        });
        const resolved = resolvedAltitude.resolved,
          altitude = resolvedAltitude.altMsl;
        if (!resolved) flush();
        const position = this.position(p.lat, p.lon, resolved ? altitude : 0);
        this.logEntities.push(
          this.viewer.entities.add({
            position,
            point: {
              pixelSize: 6,
              color: color.mission,
              heightReference: resolved ? HeightReference.NONE : HeightReference.CLAMP_TO_GROUND,
            },
            label: {
              text: `${item.seq}${resolved ? '' : ' ?alt'}`,
              font: '12px sans-serif',
              fillColor: color.mission,
              style: LabelStyle.FILL_AND_OUTLINE,
              outlineWidth: 2,
              pixelOffset: new Cartesian2(0, -15),
              heightReference: resolved ? HeightReference.NONE : HeightReference.CLAMP_TO_GROUND,
              disableDepthTestDistance: Infinity,
            },
          }),
        );
        if (resolved) line.push(position);
      }
      flush();
    }
    for (const fence of log.fences || []) {
      const tint = fence.exclusion ? color.exclusion : color.inclusion,
        points = fence.points || [];
      if (fence.kind === 'circle' && points[0] && fence.radius > 0) {
        this.logEntities.push(
          this.viewer.entities.add({
            position: Cartesian3.fromDegrees(points[0].lon, points[0].lat),
            ellipse: {
              semiMajorAxis: fence.radius,
              semiMinorAxis: fence.radius,
              material: tint.withAlpha(0.15),
              outline: true,
              outlineColor: tint,
              heightReference: HeightReference.CLAMP_TO_GROUND,
            },
          }),
        );
      }
      if (fence.kind === 'polygon' && points.length >= 3) {
        const positions = [...points, points[0]].map((p) => Cartesian3.fromDegrees(p.lon, p.lat));
        this.logEntities.push(
          this.viewer.entities.add({
            polyline: {
              positions,
              width: 3,
              clampToGround: true,
              material: new PolylineDashMaterialProperty({ color: tint }),
            },
          }),
        );
      }
    }
    this.render();
  }
  setFindings(findings) {
    this.clearEntities(this.findingEntities);
    for (const f of findings)
      if (f.ground)
        this.findingEntities.push(
          this.viewer.entities.add({
            position: this.position(f.ground.lat, f.ground.lon, f.ground.altMsl + 2),
            point: {
              pixelSize: 10,
              color: color.finding,
              outlineColor: Color.BLACK,
              outlineWidth: 2,
            },
            label: {
              text: f.label,
              font: '12px sans-serif',
              pixelOffset: new Cartesian2(0, -16),
              fillColor: Color.WHITE,
              disableDepthTestDistance: Infinity,
            },
          }),
        );
    this.render();
  }
  clear() {
    this.trackController?.abort();
    this.videoProjection.clear();
    this.clearEntities(this.flightEntities);
    this.clearTemperatureTrack();
    this.clearEntities(this.findingEntities);
    for (const piece of this.footprintPieces) piece.show = false;
    for (let i = 0; i < this.footprintCoordinates.length; i++) this.footprintCoordinates[i] = [];
    this.hover.show = false;
    this.aircraft.show = false;
    this.focused = false;
    this.currentPosition = null;
    this.followAnchor = null;
    this.videoTrack = [];
    this.render();
  }
  destroy() {
    this.closed = true;
    this.videoProjection.clear();
    clearTimeout(this.pointerTimer);
    cancelAnimationFrame(this.pointerFrame);
    this.viewer.scene.canvas.removeEventListener('pointermove', this.pointerMove);
    this.viewer.scene.canvas.removeEventListener('pointerleave', this.pointerLeave);
    this.viewer.scene.postRender.removeEventListener(this.pointerRender);
    this.trackController?.abort();
    this.generation++;
    this.terrain.close();
    this.viewer.scene.canvas.removeEventListener('pointerdown', this.pointerDown);
    window.removeEventListener('pointerup', this.pointerUp);
    window.removeEventListener('pointercancel', this.pointerUp);
    this.viewer.scene.canvas.removeEventListener('lostpointercapture', this.pointerUp);
    document.removeEventListener('visibilitychange', this.pointerUp);
    window.removeEventListener('blur', this.pointerUp);
    this.viewer.destroy();
  }
}
