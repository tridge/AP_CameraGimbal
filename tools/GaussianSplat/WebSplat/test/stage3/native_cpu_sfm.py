#!/usr/bin/env python3
"""Native sfm.py --mapper prior on CPU SIFT features: builds the database as sfm.py does but with
pycolmap's CPU SIFT (device=cpu), then runs sfm.py on it (matching, verification, prior mapping).
Separates CPU-vs-GPU SIFT from the browser chain's own differences. Writes OUT/colmap/.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

import numpy as np

GS = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(GS))
from splat_common import import_pycolmap  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('workdir', type=Path, help='reference work dir (frames.json, images/)')
    ap.add_argument('out', type=Path)
    args = ap.parse_args()
    pycolmap = import_pycolmap()
    pycolmap.logging.minloglevel = 2
    args.out.mkdir(parents=True, exist_ok=True)
    for name in ('frames.json', 'images'):
        link = args.out / name
        if not link.exists():
            link.symlink_to((args.workdir / name).resolve())
    info = json.loads((args.workdir / 'frames.json').read_text())
    frames = info['frames']
    K = np.array(info['K'])
    colmap = args.out / 'colmap'
    colmap.mkdir(exist_ok=True)
    db_path = colmap / 'database.db'
    if not db_path.exists():
        params = [K[0, 0], K[0, 2], K[1, 2], 0.0]  # sfm.py's SIMPLE_RADIAL reader camera
        reader = pycolmap.ImageReaderOptions(camera_model='SIMPLE_RADIAL',
                                             camera_params=','.join('%.6f' % p for p in params))
        ex = pycolmap.FeatureExtractionOptions()
        ex.sift.max_num_features = 8192
        ex.use_gpu = False
        pycolmap.extract_features(str(db_path), str(args.workdir / 'images'), image_names=[f['name'] for f in frames],
                                  camera_mode=pycolmap.CameraMode.SINGLE, camera_model='SIMPLE_RADIAL',
                                  reader_options=reader, extraction_options=ex, device=pycolmap.Device.cpu)
        db = pycolmap.Database.open(str(db_path))
        for camera in db.read_all_cameras():
            camera.has_prior_focal_length = True
            db.update_camera(camera)
        by_name = {im.name: im.image_id for im in db.read_all_images()}
        for f in frames:
            db.write_pose_prior(by_name[f['name']], pycolmap.PosePrior(
                np.array(f['position']), np.eye(3) * 25.0, pycolmap.PosePriorCoordinateSystem.CARTESIAN))
        db.close()
    env = dict(os.environ, OMP_NUM_THREADS=os.environ.get('OMP_NUM_THREADS', '8'))
    subprocess.run([sys.executable, str(GS / 'sfm.py'), str(args.out), '--mapper', 'prior'], check=True, env=env)


if __name__ == '__main__':
    main()
