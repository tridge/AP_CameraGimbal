# plot.ardupilot.org/video

Build with `VIEWER_BASE_PATH=/video/ npm run build` from tools/viewer. The
browser still reads local files; this installation adds no upload service,
Node process or video backend. Development defaults to /viewer/.

The installation uses nginx's existing plot.ardupilot.org HTTPS virtual host:

- `/data/sites/logviewer/video/releases/<release>/`: complete build, immutable after promotion.
- `/data/sites/logviewer/video/current`: atomic symlink to the active release.
- `/data/sites/logviewer/video/shared/viewer-config.js`: public runtime settings,
  preserved independently of releases; no private keys belong here.
- `/etc/nginx/snippets/plot-video.conf`: copy of `deploy/plot-video.nginx`.
- `/etc/nginx/sites-available/stable-web`: one include inside its HTTPS server
  block. Other virtual hosts, the stable log-viewer document root and the
  `/quantized/` data paths are unchanged. The terrain location additionally
  includes the separate CORS snippet described below.

The shared runtime template is `deploy/plot-viewer-config.js`. It uses the same
`/quantized/` service as the log viewer, with Esri satellite imagery. Google keys
can be added to that public file and must be origin restricted. The plot tiler preserves SRTM orthometric/EGM96 heights, so its runtime config
sets terrainDatum to amsl. Those heights feed both the visual globe and pixel
picking directly; no local elevation file is required. Other ellipsoidal terrain
services require the corresponding datum choice and local geoid separation.

For each update, create a new release as logviewer, upload all of dist (including
workers, WASM, notices and source materials), and verify a SHA256 manifest before
switching current. Use 755 directories and 644 files so nginx can read through
all parents; reject hidden files and symlinks in the uploaded artifact. Retain old `assets/` hashes and versioned `vendor/` directories
in the new release so already-open tabs can finish lazy loads. The manifest
therefore covers the new build plus that retained asset union; finish this
preparation before treating the release as immutable. Leave shared
runtime configuration alone. `current` can be switched using a temporary symlink
and `mv -T` on this Linux host; no nginx reload is needed for application updates.
Use a unique release name and never upload directly over the active release.

The initial nginx setup requires root. Back up stable-web and record its hash;
install the separate snippet, add its single include, run `nginx -t`, and only
then `systemctl reload nginx`. Create the release, shared config and current
symlink before reloading. Verify inherited headers and JavaScript/WASM MIME
types; this host already includes application/wasm. A graceful reload preserves existing connections.
Do not restart other services. Save baseline HTTP status/body hashes for all
virtual hosts and the terrain layer/tile, and compare after deployment.

Rollback an update by atomically switching current to the previous release.
Rollback the initial installation by removing only the added include from the
plot HTTPS block, running `nginx -t`, then reloading nginx. The exact original
stable-web backup is retained in `/root/video-deploy-backups/<release>/`; restore
it only after checking no later unrelated edits need preserving. The video data
directory can remain unreferenced. No log-viewer files need restoring.

Verify the deployed prefix in a browser, including software-decoded thermal,
H.264 APCG, MPEG-TS MISB, local flight logs, map modes, and terrain requests.
Inspect JavaScript/WASM MIME types and worker URLs. Selected files must cause
no upload requests. Record the release ID, manifest, nginx backup and test
results with the deployment receipt; do not include private credentials.

## Sharing terrain with other installations

The portable runtime default points to https://plot.ardupilot.org/quantized/ with
terrainDatum amsl. It works from another browser origin without a local terrain
route. The plot installation may retain its same-origin /quantized/ setting.

`deploy/plot-quantized-cors.nginx` is installed as
`/etc/nginx/snippets/plot-quantized-cors.conf`, included in both the plot HTTPS
/quantized/ location and its nested .terrain location. Both need the headers
because nested nginx add_header directives replace inherited headers. The
existing root, MIME and Content-Encoding gzip handling are retained.

The public service uses Access-Control-Allow-Origin: \* without credentials,
permits GET/HEAD/OPTIONS and accepts the Accept/Content-Type/Range headers.
OPTIONS returns204; CORS headers are also present on errors. Private viewer
assets and other virtual hosts are outside this change. Any hosting server
that adds a separate CSP must also permit connect-src to
https://plot.ardupilot.org/quantized/.

Validate from a distinct origin with no /quantized/ route, including a real
browser preflight and decoded terrain tile; HTTP header checks alone do not
prove the viewer's CSP or URL validation accepts the shared service.
