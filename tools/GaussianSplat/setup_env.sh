#!/bin/bash
# Create the Python environment for the Gaussian splat tools.
# usage: setup_env.sh [VENV_DIR]   (default: .venv next to this script)
# Needs an NVIDIA driver supporting CUDA 13 and a host C++ compiler; the CUDA
# toolkit (nvcc) comes from PyPI wheels so no system CUDA install is required.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
VENV=${1:-$HERE/.venv}
PY=${PYTHON_VERSION:-3.12}
TORCH_INDEX=${TORCH_INDEX:-https://download.pytorch.org/whl/cu130}
export UV_HTTP_TIMEOUT=${UV_HTTP_TIMEOUT:-900}

if ! command -v uv >/dev/null; then
    echo "uv not found; install it with: python3 -m pip install --user uv" >&2
    exit 1
fi

uv venv -p "$PY" "$VENV"
uv pip install -p "$VENV" torch torchvision --index-url "$TORCH_INDEX"
uv pip install -p "$VENV" "cuda-toolkit[nvcc,cudart,cccl,crt]==13.0.*" ninja setuptools wheel \
    numpy scipy av opencv-python-headless plyfile tqdm viser aiohttp gsplat pycolmap-cuda12 \
    trimesh rasterio pyproj pymavlink

# torch extension builds link against libcudart.so, the wheel only ships the soname
CU=$("$VENV/bin/python" -c 'import nvidia, os; print(os.path.join(nvidia.__path__[0], "cu13"))')
ln -sf libcudart.so.13 "$CU/lib/libcudart.so"

# compile the gsplat CUDA kernels now (cached in ~/.cache/torch_extensions)
"$VENV/bin/python" -c "
import sys; sys.path.insert(0, '$HERE')
from splat_common import setup_cuda_env; setup_cuda_env()
import torch, gsplat, pycolmap
from gsplat import rasterization
m = torch.zeros(1, 3, device='cuda'); m[0, 2] = 5
img, _, _ = rasterization(m, torch.tensor([[1., 0, 0, 0]], device='cuda'), torch.ones(1, 3, device='cuda') * 0.1,
                          torch.ones(1, device='cuda'), torch.ones(1, 3, device='cuda'),
                          torch.eye(4, device='cuda')[None],
                          torch.tensor([[[100., 0, 32], [0, 100, 32], [0, 0, 1]]], device='cuda'), 64, 64)
print('torch', torch.__version__, 'gpu', torch.cuda.get_device_name(), 'gsplat', gsplat.__version__,
      'pycolmap', pycolmap.__version__, 'cuda' if pycolmap.has_cuda else 'cpu')
"
echo "environment ready: $VENV"
