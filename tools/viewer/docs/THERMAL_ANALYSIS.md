# Thermal image analysis

Open a lossless raw thermal MKV and expand **Thermal analysis** below the video.
The panel displays a histogram of the current frame. Videos containing only RGB
or a colourized thermal stream cannot supply original detector measurements, so
analysis is disabled for those sources. Existing APCG/MISB playback is unchanged.

Pause at a possible source, choose a method and **Apply to paused frame**. Use
**Compare → Original / Processed result** to compare the same pixels. A badge
above the image identifies a processed display even with the panel collapsed.
Seeking or playing cancels analysis and returns to the original frame. The next
paused frame gets its own histogram; filters are applied explicitly. Settings
are remembered in the browser, but processed images and worker buffers are not
saved in local storage. No recordings or detector arrays are uploaded.

## A useful first search

1. Start with the original histogram and a sensible palette/stretch. Check
   clipping, camera non-uniformity correction changes and scene gradients.
2. Try **Gaussian matched filter** with an assumed PSF sigma of 1 raw pixel,
   then nearby scales. This is a hypothesis, not a measured MT11 calibration.
   The displayed FWHM is 2.355 sigma pixels. Range changes ground sampling and
   source fill fraction; it does not establish the optical PSF in pixel units.
3. Compare **Guarded local contrast** and **Multi-scale LoG**. Background scale
   should exceed the candidate's footprint. Try optional row/column correction
   if banding dominates, and compare it off: real elongated heat can be removed.
4. **Show analysis candidates** lists up to 10 local maxima above a heuristic
   score of 3. Select a listed raw pixel to inspect or mark it. Border pixels
   affected by filter support are excluded; the panel states the border width.
   A small ROI wholly inside that border cannot contain an eligible candidate.
   Pixel-scale excess flags are retained, not automatically rejected: either a
   defect or a genuine undersampled source may occupy one detector pixel.
5. Compare the original frame and neighbouring views before marking a finding.
   The separate **Find raw contrast peaks** button is the original unnormalized
   raw-contrast baseline. Its raw-count contrasts differ from analysis scores.
6. Deconvolution can explore blur hypotheses. Compare **Richardson–Lucy** and
   **Wiener**, starting with few iterations / conservative regularization.
   Neither restoration provides a newly calibrated temperature. Their candidate
   list deliberately uses the independent matched-filter response, so ringing
   created by restoration cannot promote a new candidate.

The existing hover/typed pixel controls, temperatures and ground positions use
**original** pixels and telemetry in every display mode. A small hot source's
measured pixel temperature mixes source/background radiance and fill fraction;
it is not the object's physical surface temperature.

## Methods and exact conventions

| Method                  | Output / controls                                                                                                                                                                                                                                                                                          |
| ----------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Original                | Raw 16-bit histogram; no processed image                                                                                                                                                                                                                                                                   |
| Gaussian background     | Raw counts minus reflected Gaussian blur; background sigma=max(selected background,2×PSF sigma)                                                                                                                                                                                                            |
| Difference of Gaussians | Reflected Gaussian sigma minus 1.6 sigma; signed raw-count residual                                                                                                                                                                                                                                        |
| White top-hat           | Raw counts minus square opening; radius=ceil(effective background scale)                                                                                                                                                                                                                                   |
| Guarded local contrast  | Centre minus square-ring mean, divided by ring standard deviation with a global noise floor; guard=ceil(3 sigma), outer=max(guard+2,ceil(2 background scale))                                                                                                                                              |
| Matched filter          | Gaussian of background-subtracted frame; divided by max(response MAD sigma, white-noise smoothing floor). Empirical contrast score, not a calibrated SNR or probability                                                                                                                                    |
| Multi-scale LoG         | Max of scale-normalized negative five-point Laplacian after Gaussian smoothing at sigma×{0.7,1,1.4}; each response divided by max(its MAD sigma,0.05 input-noise sigma). Reports winning scale per candidate                                                                                               |
| CLAHE                   | 32-pixel tiles,1024 bins over the frame min/max range, clipped histograms with uniform excess redistribution and bilinear tile interpolation. Display intensity only                                                                                                                                       |
| Richardson–Lucy         | Positive max(input−P01,0)+0.001; 1–25 iterations, default 8. Optional residual soft threshold in measured noise sigma: ratio=(prediction+soft(observation−prediction,threshold))/prediction. Half-sample reflection; output adds the floor back. Exploratory damped restoration, not a Poisson-noise claim |
| Wiener                  | Mean-subtracted, reflected/tapered FFT, sampled Gaussian transfer. K=adjustment×noise²/max(noise²,input variance−noise²), bounded 0.001–10; transfer gain capped at 8. Mean restored after inverse FFT                                                                                                     |

All filtering uses Float32 planes and normalized Gaussian kernels of radius
ceil(4 sigma), except the Fourier transform and integral statistics use Float64.
The source Uint16Array remains immutable. The noise estimate is 1.4826×MAD of
input−Gaussian(input,1), corrected for the 2D filter energy. Sampling is bounded
and shifted by row to avoid repeatedly sampling the same columns. A 1-raw-count
floor is recorded explicitly. Correlated scene/noise structure invalidates a
Gaussian-tail interpretation; thresholds do not provide a false-alarm rate.

Optional destriping subtracts row and column medians of a high-pass residual
before other processing. The unmodified original remains the radiometric source.
Noise and response normalization use the **full frame**, so changing the ROI
cannot change the processed plane. The region is always in raw coordinates,
independent of 180° display rotation; **Around cursor** uses the existing raw
pixel cursor. A dashed border identifies a non-full-frame region.

Raw percentiles use an exact 65536-bin histogram and nearest-rank quantiles.
The chart/CSV has 256 explicit bins spanning the selected region's min/max
(constant regions span min to min+1). Bins are half-open, with the last upper
bound included. Processed histograms retain signed float values. Celsius is
shown only for an original histogram with valid recorded calibration. **Log
counts** changes the count axis only; it does not change the source samples.

Processed display limits always use full-frame 1–99 percentiles. A regional
histogram does not rescale the rest of the displayed image.

## Reproduction and limits

Histogram CSV exports include source/file/frame identity, input pixel SHA-256,
ROI, units, bin bounds/counts and optional analysis provenance. Finding JSON and
GeoJSON retain the existing original raw value and temperature, plus an optional
`apcg.thermal-analysis.v1` block: parameters, assumed PSF, noise/floor/scope,
effective Wiener K, score method/units, border exclusion, original/result view,
input hash and Float32 implementation version. A marked pixel outside the ROI
is identified. A raw preview after a seek has no stale analysis attached.

Analysis runs in a disposable worker, with latest-request-wins identity checks,
30-second timeout and cancel on seeks/source changes/reset/close. The limit is
2,097,152 input pixels; Wiener additionally limits its power-of-two padded FFT
to 1,048,576 complex cells. Full-size pixels are never silently downsampled.
Memory is bounded by these dimensions; the CFAR integral images and Wiener FFT
are the largest allocations. Leave analysis off during playback on slow devices.

The numerical tests compare independent SciPy 1.16.3 / NumPy reference planes,
including reflection and both restorations. Regenerate with
`scripts/thermal-analysis-reference.py`. No Python numerical package is needed
by the browser or by the ordinary unit tests; the reference fixture is checked
in. `scripts/qualify-thermal-analysis.mjs` reports seeded injection recovery and
other candidates at several signal levels, including a no-source control.
These results characterize that synthetic setup, not field accuracy. Real-flight
Xephyr qualification verifies operation, raw preservation and UI behavior, not
that any particular thermal peak is a lamp, lightning strike or ignition.

Next useful extensions are measured PSFs and PSF mismatch diagnostics,
registered temporal stacking/persistence, robust temporal backgrounds, and
spatial noise/NUC characterization. Wavelet and low-rank/sparse methods deserve
controlled comparisons. Learned detectors require representative labeled data.
See the [research and reviewed plan](THERMAL_ANALYSIS_PLAN.md) for primary sources.
