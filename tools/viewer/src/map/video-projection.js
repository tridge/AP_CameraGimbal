import {
  ClassificationType,
  GeometryInstance,
  GroundPrimitive,
  Material,
  MaterialAppearance,
  Rectangle,
  RectangleGeometry,
  Texture,
} from 'cesium';
import { validPose } from '../domain/geometry.js';
import { buildVideoProjection } from '../domain/video-projection.js';

/** One current-frame geographic raster draped over the 2D/3D terrain.
 * Geometry is rebuilt only when pose, lens, dimensions or terrain change.
 */
export class VideoProjection {
  constructor(scene, onChange = () => {}) {
    this.scene = scene;
    this.enabled = false;
    this.dirty = true;
    this.terrainHeightsReady = false;
    GroundPrimitive.initializeTerrainHeights().then(
      () => {
        this.terrainHeightsReady = true;
        this.dirty = true;
        if (!this.scene.isDestroyed()) {
          onChange();
          this.scene.requestRender();
        }
      },
      (error) => {
        this.initializationError = error;
        if (!this.scene.isDestroyed()) onChange();
      },
    );
  }
  setEnabled(enabled) {
    this.enabled = !!enabled;
    if (!this.enabled) this.clear();
    this.scene.requestRender();
  }
  invalidate() {
    this.dirty = true;
  }
  update(image, record, elevation) {
    if (!this.enabled) return null;
    if (this.initializationError) throw this.initializationError;
    if (!GroundPrimitive.supportsMaterials(this.scene))
      throw new Error('Terrain video projection needs depth-texture support.');
    if (!image) {
      this.clear();
      return 'Open a video to project it onto terrain.';
    }
    const key = JSON.stringify([image.width, image.height, record?.hfov_deg, validPose(record)]);
    if (this.dirty || key !== this.key || this.terrain !== elevation.terrain) {
      this.mesh = buildVideoProjection(elevation, record, image.width, image.height);
      this.key = key;
      this.terrain = elevation.terrain;
      this.dirty = false;
      this.removePrimitive();
      this.rasterDirty = true;
      if (this.mesh.indices.length) {
        this.updateTexture(image);
        this.drawMesh();
      } else this.clearTexture();
    } else if (this.mesh.indices.length) this.updateTexture(image);
    this.scene.requestRender();
    return this.mesh.complete
      ? 'Video projected onto terrain.'
      : `${this.mesh.indices.length ? 'Partial video projection' : 'Video projection unavailable'}: ${this.mesh.reason}`;
  }
  updateTexture(image) {
    if (image === this.image && !this.rasterDirty) return;
    this.rasterDirty = false;
    if (!this.raster || this.raster.width !== image.width || this.raster.height !== image.height)
      this.raster = new OffscreenCanvas(image.width, image.height);
    const points = this.mesh.vertices;
    const west = Math.min(...points.map((p) => p.lon)),
      east = Math.max(...points.map((p) => p.lon));
    const south = Math.min(...points.map((p) => p.lat)),
      north = Math.max(...points.map((p) => p.lat));
    this.rectangle = Rectangle.fromDegrees(west, south, east, north);
    const context = this.raster.getContext('2d');
    context.clearRect(0, 0, image.width, image.height);
    for (let i = 0; i < this.mesh.indices.length; i += 3) {
      const triangle = this.mesh.indices.slice(i, i + 3).map((index) => points[index]);
      const source = triangle.map((p) => ({ x: p.u * image.width, y: (1 - p.v) * image.height }));
      const target = triangle.map((p) => ({
        x: ((p.lon - west) / (east - west)) * image.width,
        y: ((north - p.lat) / (north - south)) * image.height,
      }));
      const [a, b, c] = source,
        [p, q, r] = target;
      const det = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
      if (Math.abs(det) < 1e-9) continue;
      const xx = ((q.x - p.x) * (c.y - a.y) - (r.x - p.x) * (b.y - a.y)) / det;
      const xy = ((q.y - p.y) * (c.y - a.y) - (r.y - p.y) * (b.y - a.y)) / det;
      const yx = ((r.x - p.x) * (b.x - a.x) - (q.x - p.x) * (c.x - a.x)) / det;
      const yy = ((r.y - p.y) * (b.x - a.x) - (q.y - p.y) * (c.x - a.x)) / det;
      // Subpixel overlap avoids antialiasing cracks at shared edges.
      const cx = (p.x + q.x + r.x) / 3,
        cy = (p.y + q.y + r.y) / 3;
      const sx = (a.x + b.x + c.x) / 3,
        sy = (a.y + b.y + c.y) / 3,
        crop = [];
      context.save();
      context.beginPath();
      target.forEach((v, j) => {
        const length = Math.hypot(v.x - cx, v.y - cy),
          scale = 1 + 0.75 / Math.max(length, 0.001);
        const x = cx + (v.x - cx) * scale,
          y = cy + (v.y - cy) * scale;
        crop.push({ x: sx + (source[j].x - sx) * scale, y: sy + (source[j].y - sy) * scale });
        if (j) context.lineTo(x, y);
        else context.moveTo(x, y);
      });
      context.closePath();
      context.clip();
      context.setTransform(xx, xy, yx, yy, p.x - xx * a.x - yx * a.y, p.y - xy * a.x - yy * a.y);
      // Crop to the source region under this expanded triangle. Drawing the
      // full HD frame for every clip needlessly magnifies rasterisation cost.
      const left = Math.max(0, Math.floor(Math.min(...crop.map((p) => p.x))) - 1),
        top = Math.max(0, Math.floor(Math.min(...crop.map((p) => p.y))) - 1),
        right = Math.min(image.width, Math.ceil(Math.max(...crop.map((p) => p.x))) + 1),
        bottom = Math.min(image.height, Math.ceil(Math.max(...crop.map((p) => p.y))) + 1);
      if (right > left && bottom > top)
        context.drawImage(
          image,
          left,
          top,
          right - left,
          bottom - top,
          left,
          top,
          right - left,
          bottom - top,
        );
      context.restore();
    }
    const raster = this.raster;
    if (this.texture && this.texture.width === image.width && this.texture.height === image.height)
      this.texture.copyFrom({ source: raster });
    else {
      const texture = new Texture({ context: this.scene.context, source: raster });
      if (!this.material)
        this.material = new Material({
          fabric: {
            type: 'ViewerVideoProjection',
            uniforms: { image: Material.DefaultImageId },
            source: `czm_material czm_getMaterial(czm_materialInput materialInput) {
              czm_material material = czm_getDefaultMaterial(materialInput);
              vec4 color = texture(image, materialInput.st);
              material.diffuse = color.rgb;
              material.alpha = color.a;
              return material;
            }`,
          },
          translucent: true,
        });
      this.material.uniforms.image = texture;
      // Bind immediately so there is no white/default-texture frame, and the
      // material owns the texture even if cleared before the next scene render.
      this.material.update(this.scene.context);
      this.texture = texture;
    }
    this.image = image;
  }
  drawMesh() {
    if (!this.terrainHeightsReady) return;
    this.removePrimitive();
    // Drape one geographic raster on the rendered terrain, at its current LOD.
    const geometry = new RectangleGeometry({
      rectangle: this.rectangle,
      vertexFormat: MaterialAppearance.MaterialSupport.TEXTURED.vertexFormat,
    });
    this.primitive = this.scene.primitives.add(
      new GroundPrimitive({
        geometryInstances: new GeometryInstance({ geometry }),
        appearance: new MaterialAppearance({
          material: this.material,
          flat: true,
          translucent: true,
          closed: false,
        }),
        asynchronous: false,
        allowPicking: false,
        classificationType: ClassificationType.TERRAIN,
      }),
    );
  }
  removePrimitive() {
    if (this.primitive) this.scene.primitives.remove(this.primitive);
    this.primitive = null;
  }
  clearTexture() {
    this.material?.destroy();
    this.material = null;
    this.texture = null;
    this.image = null;
    this.raster = null;
  }
  clear() {
    this.removePrimitive();
    this.clearTexture();
    this.mesh = null;
    this.key = null;
    this.terrain = null;
    this.dirty = true;
    this.scene.requestRender();
  }
}
