# WebSplat: thermal splats in the browser

WebSplat turns an AP_CameraGimbal thermal recording (an `.mkv` from the camera gimbal, with the
vehicle telemetry in every frame) into a 3D Gaussian splat of the scene, entirely in your browser:
nothing is uploaded. The splat shows temperature, and you can view it in the page or download it.

## What you need

- **Chrome (or Edge) with WebGPU**, on a computer with a discrete GPU (8 GB or more of GPU memory
  is enough; tested on an NVIDIA RTX laptop GPU and a desktop RTX 4070-class GPU).
- **Windows:** current Chrome works as is. On a laptop with two GPUs, set Chrome to "High
  performance" in Windows Settings > System > Display > Graphics, so it uses the discrete GPU.
- **Linux:** start Chrome with `--enable-unsafe-webgpu --use-angle=gl --enable-features=Vulkan`
  (or enable `chrome://flags/#enable-unsafe-webgpu` and `#enable-vulkan`). On a laptop with an
  NVIDIA GPU, make sure Chrome runs on it (e.g. `prime-run` or `__NV_PRIME_RENDER_OFFLOAD=1`).
  The page checks the GPU when it opens and says what is missing.
- **Browser storage:** a few GB free on the disk Chrome uses. Keep the page's tab open while it works.

## Using it

1. Open the page (test deployment: https://plot.ardupilot.org/websplat/) and choose the recording.
2. The page reads the recording and proposes where the hot spot is (latitude, longitude, target
   altitude, rotation, field of view). Check the values and continue.
3. Wait. Each stage shows its progress and the page estimates the time left. You can press
   **Stop** at any time and **Resume** later, also after closing the tab or reloading: finished
   stages are kept, and the long ones (mapping, training) continue from their last checkpoint. If
   the recording was not copied into browser storage, the page asks you to choose it again.
4. When it finishes, the result view shows the splat with temperatures, and offers the downloads.

## Expected times (a 3.5 minute, 201-frame orbit)

| | laptop (RTX laptop GPU) | desktop (RTX 4070-class) |
|---|---|---|
| reading and selecting frames | under 10 s | under 10 s |
| features | about 20 s | about 20 s |
| matching | about 1 min | under 1 min |
| mapping | about 1.2 min | about 1.3 min |
| training | about 12.5 min | about 7 min |
| export | about 10 s | about 10 s |
| **total** | **about 15.5 min** | **about 10 min** |

Training dominates; it is slower if other programs use the GPU at the same time.

## The stages

ingest (read the recording and its telemetry) - select (choose the frames, decode the thermal
images) - features (find image features) - match (match frames and verify the geometry) - map
(reconstruct the camera poses, seeded by the telemetry) - prepare (build the training dataset) -
train (train the thermal splat on the GPU) - export (write the downloads).

## Outputs

- `splat.ply`: the splat (3D Gaussian PLY), with the temperature scale in its header.
- `splat.spz`: the same splat, compressed, for the web viewer.
- `meta.json`: the temperature scale, hot spot, start camera and other details.
- The reconstructed camera model as a COLMAP archive (`sparse/0/*.bin`).

## When something goes wrong

The page says what happened and what to do. Common cases: the browser storage is full (free
space, then Resume); the job is open in another tab (use that tab); the GPU was lost or ran out of
memory (close other GPU programs, then Resume); the recording has no telemetry (the page cannot
build a splat from it); the job was made by another version of the page (its files are kept; delete
the job from the page and start again). Jobs stay in browser storage until deleted.
