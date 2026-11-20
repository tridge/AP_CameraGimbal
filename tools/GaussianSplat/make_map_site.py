#!/usr/bin/env python3
"""Static web page for a surface_map.py + drape_thermal.py result.

Copies the overlays, meshes and GeoTIFFs into OUTDIR and writes OUTDIR/index.html with a
2D map (orthophoto and thermal over satellite imagery, hot spot markers) and a 3D view of
the surface textured with RGB or thermal. Needs no server-side code: any static host works.

  make_map_site.py RGBWORK/dense/map OUTDIR --title "2024-11-07 flight 2"
"""
import argparse
import html
import json
from pathlib import Path
import shutil

PAGE = r'''<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="robots" content="noindex"><title>%(title)s</title>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css">
<style>
body{margin:0;font-family:system-ui,sans-serif;display:flex;flex-direction:column;height:100vh}
header{padding:.5em 1em;background:#222;color:#eee;display:flex;gap:1em;align-items:center;flex-wrap:wrap}
header a{color:#9cf}button{padding:.3em .8em}button.on{background:#9cf}
#map,#three{flex:1;min-height:0}#three{display:none;background:#111}
.legend{background:#fff;padding:4px 8px;font-size:12px;line-height:1.3}
.legend .bar{width:180px;height:10px;background:linear-gradient(to right,%(gradient)s)}
</style></head><body>
<header><b>%(title)s</b>
<span><button id="b2d" class="on">Map</button> <button id="b3d">3D</button></span>
<span id="tex3d" style="display:none">Texture: <button id="trgb" class="on">RGB</button> <button id="tth">Thermal</button></span>
<span>%(downloads)s</span></header>
<div id="map"></div><div id="three"></div>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<script type="importmap">{"imports":{"three":"https://unpkg.com/three@0.170.0/build/three.module.js",
"three/addons/":"https://unpkg.com/three@0.170.0/examples/jsm/"}}</script>
<script>
const INFO = %(info)s;
const bounds = [[INFO.south, INFO.west], [INFO.north, INFO.east]];
const map = L.map('map', {maxZoom: 23}).fitBounds(bounds);
setTimeout(() => { map.invalidateSize(); map.fitBounds(bounds); }, 200);
const sat = L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}',
  {maxNativeZoom: 19, maxZoom: 23, attribution: 'Imagery &copy; Esri'}).addTo(map);
const osm = L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',
  {maxNativeZoom: 19, maxZoom: 23, attribution: '&copy; OpenStreetMap'});
const ortho = L.imageOverlay('ortho.png', bounds).addTo(map);
const thermal = L.imageOverlay('thermal_max.png', bounds, {opacity: 0.85});
const hot = L.imageOverlay('overlay.png', bounds).addTo(map);
const spots = L.layerGroup().addTo(map);
fetch(INFO.hotspots).then(r => r.json()).then(g => g.features.forEach(f => {
  const p = f.properties, c = f.geometry.coordinates;
  L.circleMarker([c[1], c[0]], {radius: p.kind === 'triangulated' ? 9 : 6, color: '#fff', weight: 2,
                  fillColor: p.kind === 'triangulated' ? '#f30' : '#fa0', fillOpacity: .9})
    .bindPopup(`<b>${p.peak_c} &deg;C</b> peak<br>` + (p.kind === 'triangulated'
                 ? `triangulated from ${p.views} views (rays meet within ${p.ray_error_px} px)<br>height ${c[2].toFixed(1)} m AMSL`
                 : `on the surface model: ${p.area_m2} m&sup2; above ${INFO.hot_c} &deg;C, ${p.views} views`) +
               `<br>${c[1].toFixed(6)}, ${c[0].toFixed(6)}`).addTo(spots);
}));
L.control.layers({'Satellite (Esri)': sat, 'OpenStreetMap': osm},
  {'RGB orthophoto (this flight)': ortho, 'Thermal, all temperatures': thermal,
   ['Hot areas (above ' + INFO.hot_c + ' °C)']: hot, 'Hot spots': spots}, {collapsed: false}).addTo(map);
L.control.scale({imperial: false}).addTo(map);
const legend = L.control({position: 'bottomleft'});
legend.onAdd = () => { const d = L.DomUtil.create('div', 'legend');
  d.innerHTML = `Apparent temperature<div class="bar"></div>${INFO.range[0].toFixed(0)}` +
    `<span style="float:right">${INFO.range[2].toFixed(0)} &deg;C</span><br>` +
    `<small>palette: ${INFO.range[0].toFixed(0)}&ndash;${INFO.range[1].toFixed(0)} &deg;C linear, hotter compressed</small>`;
  return d; };
legend.addTo(map);
</script>
<script type="module">
import * as THREE from 'three';
import {OrbitControls} from 'three/addons/controls/OrbitControls.js';
import {GLTFLoader} from 'three/addons/loaders/GLTFLoader.js';
const el = document.getElementById('three');
let renderer, scene, camera, controls, meshes = {}, started = false;
function start() {
  started = true;
  renderer = new THREE.WebGLRenderer({antialias: true});
  renderer.setPixelRatio(window.devicePixelRatio);
  el.appendChild(renderer.domElement);
  scene = new THREE.Scene();
  scene.background = new THREE.Color(0x1a1a1a);
  scene.add(new THREE.AmbientLight(0xffffff, 3));
  camera = new THREE.PerspectiveCamera(50, 1, 1, 20000);
  controls = new OrbitControls(camera, renderer.domElement);
  const loader = new GLTFLoader();
  for (const [key, file] of [['rgb', 'surface.glb'], ['thermal', 'thermal.glb']]) {
    loader.load(file, g => {
      g.scene.traverse(o => { if (o.material) { o.material = new THREE.MeshBasicMaterial({map: o.material.map}); } });
      meshes[key] = g.scene; g.scene.visible = key === 'rgb'; scene.add(g.scene);
      if (key === 'rgb') {
        const box = new THREE.Box3().setFromObject(g.scene), c = box.getCenter(new THREE.Vector3());
        const r = box.getSize(new THREE.Vector3()).length() / 2;
        controls.target.copy(c); camera.position.set(c.x, c.y + r * 0.8, c.z + r * 0.9); controls.update();
      }
    });
  }
  fetch(INFO.hotspots).then(r => r.json()).then(g => g.features.slice(0, 10).forEach(f => {
    const e = f.properties.enu, s = new THREE.Mesh(new THREE.SphereGeometry(1.5),
      new THREE.MeshBasicMaterial({color: 0xff3300}));
    s.position.set(e[0], e[2] + 2, -e[1]); scene.add(s);
  }));
  resize(); window.addEventListener('resize', resize);
  renderer.setAnimationLoop(() => { controls.update(); renderer.render(scene, camera); });
}
function resize() {
  if (!renderer) return;
  renderer.setSize(el.clientWidth, el.clientHeight);
  camera.aspect = el.clientWidth / el.clientHeight; camera.updateProjectionMatrix();
}
const on = (a, b) => { a.classList.add('on'); b.classList.remove('on'); };
document.getElementById('b2d').onclick = e => {
  on(e.target, document.getElementById('b3d'));
  document.getElementById('map').style.display = 'block'; el.style.display = 'none';
  document.getElementById('tex3d').style.display = 'none'; window.dispatchEvent(new Event('resize'));
};
document.getElementById('b3d').onclick = e => {
  on(e.target, document.getElementById('b2d'));
  document.getElementById('map').style.display = 'none'; el.style.display = 'block';
  document.getElementById('tex3d').style.display = 'inline';
  if (!started) start(); else resize();
};
const tex = key => { for (const k in meshes) meshes[k].visible = k === key; };
document.getElementById('trgb').onclick = e => { on(e.target, document.getElementById('tth')); tex('rgb'); };
document.getElementById('tth').onclick = e => { on(e.target, document.getElementById('trgb')); tex('thermal'); };
</script></body></html>
'''

FILES = [('ortho.png', None), ('surface.glb', None), ('dsm.tif', 'surface model (GeoTIFF, m AMSL)'),
         ('ortho.tif', 'RGB orthophoto (GeoTIFF)'), ('thermal/thermal_max.png', None),
         ('thermal/overlay.png', None), ('thermal/hotspots.geojson', 'hot spots (GeoJSON)'),
         ('thermal/thermal.glb', None), ('thermal/thermal_max.tif', 'hottest view per cell (GeoTIFF, C)'),
         ('thermal/thermal_mean.tif', 'mean temperature (GeoTIFF, C)'),
         ('thermal/thermal.kmz', 'Google Earth overlay (KMZ)')]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mapdir', type=Path, help='surface_map.py output (with thermal/ from drape_thermal.py)')
    ap.add_argument('outdir', type=Path)
    ap.add_argument('--title', default='Thermal map')
    ap.add_argument('--prefix', default='', help='prefix for the downloadable files, e.g. DATE_FLIGHT_')
    args = ap.parse_args()

    from colormaps import colormap_lut
    drape = json.loads((args.mapdir / 'thermal' / 'drape.json').read_text())
    args.outdir.mkdir(parents=True, exist_ok=True)
    links = []
    names = {}
    for name, label in FILES:
        base = Path(name).name
        out = (args.prefix + base) if label else base  # downloads carry the prefix; page assets keep fixed names
        shutil.copy2(args.mapdir / name, args.outdir / out)
        names[base] = out
        if label:
            links.append('<a href="%s" download>%s</a>' % (out, html.escape(label)))
    lut = colormap_lut('ironbow')
    gradient = ','.join('rgb(%d,%d,%d)' % tuple(lut[i] * 255) for i in range(0, 256, 32))
    info = dict(north=drape['north'], south=drape['south'], east=drape['east'], west=drape['west'],
                hot_c=drape['hot_c'], range=drape['colour_range_c'], hotspots=names['hotspots.geojson'])
    (args.outdir / 'index.html').write_text(PAGE % dict(
        title=html.escape(args.title), info=json.dumps(info), gradient=gradient,
        downloads='Downloads: ' + ' &middot; '.join(links)))
    print('wrote %s' % args.outdir)


if __name__ == '__main__':
    main()
