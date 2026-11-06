import {
  BoundingSphere,
  Cartesian3,
  ComponentDatatype,
  Geometry,
  GeometryAttribute,
  GeometryInstance,
  Material,
  MaterialAppearance,
  Primitive,
  PrimitiveType,
  Texture,
} from 'cesium';
import { validPose } from '../domain/geometry.js';
import { buildVideoProjection } from '../domain/video-projection.js';

/** One current-frame texture and terrain mesh, shared by the 2D/3D scene.
 * Geometry is rebuilt only when pose, lens, dimensions or terrain change.
 */
export class VideoProjection {
  constructor(scene, position) {
    this.scene = scene;
    this.position = position;
    this.enabled = false;
    this.dirty = true;
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
    if (image === this.image) return;
    if (this.texture && this.texture.width === image.width && this.texture.height === image.height)
      this.texture.copyFrom({ source: image });
    else {
      const texture = new Texture({ context: this.scene.context, source: image });
      if (!this.material)
        this.material = new Material({
          fabric: {
            type: 'ViewerVideoProjection',
            uniforms: { image: Material.DefaultImageId },
            source: `czm_material czm_getMaterial(czm_materialInput materialInput) {
              czm_material material = czm_getDefaultMaterial(materialInput);
              material.diffuse = texture(image, materialInput.st).rgb;
              material.alpha = 1.0;
              return material;
            }`,
          },
          translucent: false,
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
    const positions = new Float64Array(this.mesh.vertices.length * 3),
      normals = new Float32Array(positions.length),
      uv = new Float32Array(this.mesh.vertices.length * 2);
    this.mesh.vertices.forEach((p, i) => {
      // Keep the mesh just above the sampled surface to avoid z fighting.
      const world = this.position(p.lat, p.lon, p.altMsl + 0.5),
        normal = Cartesian3.normalize(world, new Cartesian3());
      Cartesian3.pack(world, positions, i * 3);
      Cartesian3.pack(normal, normals, i * 3);
      uv.set([p.u, p.v], i * 2);
    });
    const attribute = (values, componentDatatype, componentsPerAttribute) =>
      new GeometryAttribute({ values, componentDatatype, componentsPerAttribute });
    const geometry = new Geometry({
      attributes: {
        position: attribute(positions, ComponentDatatype.DOUBLE, 3),
        normal: attribute(normals, ComponentDatatype.FLOAT, 3),
        st: attribute(uv, ComponentDatatype.FLOAT, 2),
      },
      indices: new Uint16Array(this.mesh.indices),
      primitiveType: PrimitiveType.TRIANGLES,
      boundingSphere: BoundingSphere.fromVertices(positions),
    });
    this.primitive = this.scene.primitives.add(
      new Primitive({
        geometryInstances: new GeometryInstance({ geometry }),
        appearance: new MaterialAppearance({
          material: this.material,
          flat: true,
          translucent: false,
          closed: false,
          renderState: { polygonOffset: { enabled: true, factor: -1, units: -1 } },
        }),
        asynchronous: false,
        allowPicking: false,
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
