# Thermal analysis workbench — pr-web-viewer2

Scope: browser-local analysis of decoded, lossless gray16 thermal frames to
support review of faint, stationary heat sources. No upload, source mutation,
new temperature calibration, ignition classification or deployment is involved.
Use the existing local decoder, raw-coordinate georeferencing and findings.

## Evidence and method selection

| Method                                     | Purpose                                                                             | Limitations                                                                                    |
| ------------------------------------------ | ----------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------- |
| Histogram + percentile/statistical summary | Inspect raw distribution, contrast and clipping; optional raw-coordinate region     | A tiny source may be invisible in a full-frame histogram                                       |
| Gaussian background subtraction            | Remove broad scene gradients                                                        | Background scale must exceed target extent; edges can respond                                  |
| Difference of Gaussians                    | Bandpass for compact sources at a chosen scale                                      | Scale mismatch and textured edges cause responses                                              |
| White top-hat                              | Subtract morphological opening, retaining compact bright features                   | Structuring element changes what is considered background                                      |
| Guarded local contrast / z-score           | Contrast against surrounding mean/noise, excluding core target                      | A local contrast score is not a calibrated probability or temperature                          |
| Gaussian matched filter                    | Smooth a PSF-sized source after background suppression, normalized by kernel energy | Assumed Gaussian PSF and white-noise model; compare with raw imagery                           |
| CLAHE                                      | Local contrast visualization                                                        | Can amplify noise; display-only intensity, never temperature                                   |
| Richardson–Lucy deconvolution              | Iterative positive restoration with a normalized Gaussian PSF                       | Thermal detector noise is not necessarily Poisson; iterations regularize; PSF is an assumption |
| Wiener deconvolution                       | Regularized Fourier restoration using the same assumed Gaussian PSF                 | Ringing/noise and regularization dependence; reflected padding, bounded FFT budget             |

Sources (primary research / official implementation references):

- https://scikit-image.org/docs/stable/api/skimage.restoration.html
- https://scikit-image.org/docs/stable/api/skimage.morphology.html
- https://scikit-image.org/docs/stable/api/skimage.exposure.html
- https://www.mdpi.com/2079-9292/11/10/1547 (local contrast research)
- https://www.mdpi.com/2226-4310/10/5/449 (LoG/local contrast research)
- https://docs.astropy.org/projects/photutils/en/stable/user_guide/detection.html
- https://opus.lib.uts.edu.au/handle/10453/116386 (infrared patch-image model)

Follow-on techniques to evaluate: multi-scale LoG/scale-space maxima, annular
CFAR, wavelet denoising, robust low-rank/sparse background decomposition,
registered temporal stacking / track-before-detect and temporal median
subtraction. Temporal methods require registration of ground features, temporal
noise/NUC handling and evidence from multiple views. Neural networks need
representative labeled data and explicit model/provenance management. Do not
claim a single-frame filter can recover information absent from sampling.

## Implementation stages

1. Pure, tested numerical module: immutable raw input, finite Float32 outputs,
   normalized kernels, reflected boundaries, bounded parameters/iterations and
   image/FFT budgets. Histogram/statistics of raw and processed planes; region
   affects statistics/candidate selection, not convolution boundaries. Candidate
   ranking records processed score alongside original raw value and temperature.
2. Dedicated disposable Web Worker and latest-request-wins controller. Snapshot
   input by copying, never transfer the decoder's pixels. Cancel/recreate worker
   on changes, seek, source replacement, reset and close. Reject stale results
   using frame/file generation. Analysis is explicit on a paused current frame;
   playback/seek shows original until a matching result exists.
3. Collapsible Thermal analysis panel outside image: raw/result comparison,
   histogram with linear/log counts and raw/processed source; method controls,
   PSF sigma, background/guard scales, RL iterations, Wiener regularization,
   CLAHE settings, region controls, apply/cancel/reset and result candidates.
   Raw hover temperatures and map coordinates always come from original pixels.
   Derived output is labeled as sample residual, local contrast score, or display
   intensity; deconvolution output is explicitly non-radiometric. Existing
   Celsius stretch applies only to raw images. Export histogram CSV + reproducible
   analysis settings/frame identity in findings; persist bounded settings only.
4. Validate numerical invariants and synthetic weak spots against seeded noise,
   gradients, edges, constants and border impulses. Compare deconvolution with
   independently computed Python/scipy references. Browser tests in a visible
   Xephyr window: real thermal recording and fixtures, every method, histogram,
   raw hover/georeferencing, changes/seek cancellation, RGB disablement,
   source replacement, export provenance and unchanged raw samples. Run existing
   unit/build/browser checks appropriate to integration changes.

Claude should review this plan before implementation and independently propose
additional techniques, priorities, and numerical pitfalls. Record accepted
changes and subsequent code-review findings here before final qualification.

## Accepted plan after Claude consultation (2 November 2026)

Claude session 521b4205-0f76-49ec-af45-321e9829620f reviewed the plan and
independently recommended MAD noise, destriping, multi-scale LoG and guarded
CFAR. Accept those priorities: implement detection/histograms before restoration.
Use optional row/column median correction, explicitly off by default because it
can remove real elongated heat sources. Use a robust MAD-based noise estimate
and label normalized responses as empirical contrast scores, not calibrated
SNR/probabilities. Quantized/flat noise uses an explicit one-sample floor.

PSF sigma defaults to 1 raw pixel as an _unmeasured Gaussian assumption_, with
FWHM=2.355 sigma shown; compare nearby scales rather than infer optics from
altitude. Geometry changes ground sampling and source fill fraction, not
necessarily optical PSF in pixel units. A measured PSF/import is future work.

Keep both Richardson–Lucy and Wiener as bounded experimental visualization
options because the user explicitly requested deconvolution techniques; disagree
with deferring Wiener entirely. RL uses a positive floor-subtracted image,
early stopping (1–25 iterations, default 8) and optional noise damping. Wiener
uses reflected padding >=4 sigma and K estimated from robust noise variance /
signal variance, with an explicit adjustment factor and reported effective K.
Both restoration methods keep candidate ranking on the separate original-input
matched-filter response, never on invented restored peaks. Exclude a processing
support border and flag unresolved/single-pixel features rather than assert they
are defects (an undersampled genuine source may occupy one pixel).

Use raw-coordinate ROI, exact 65536-bin raw histogram internally, nearest-rank
percentiles, and 256 explicit display/export bins for floats. Export original
pixel temperatures only; optional analysis block v1 includes source identity,
frame hash, method/parameters, noise/PSF assumptions, ROI, display state and
score units. Existing Find local peaks remains a labeled raw-contrast baseline;
analysis candidate selection is explicit and records its own score semantics.

Qualification requires finite/immutable outputs, constant and saturated frames,
flux/centering/reference comparisons, synthetic subpixel spots vs noise/edge
controls, deterministic maxima and browser cancellation/source-switch tests.
Synthetic detection measurements are controlled examples, not field performance.
Temporal registration/stacking, low-rank decomposition and trained networks stay
in the documented roadmap. Code-review consultation follows implementation.

## Numerical review disposition

Accepted Claude's ROI-independence issue: noise, matched-score normalization and
Wiener signal variance now use the full frame; ROI selects statistics/peaks.
Added a 1-count noise floor flag, K>=0.001 and inverse gain<=8, tapered reflected
FFT margins, a percentile RL floor, bounded candidate proposal storage, shifted
MAD sampling, client timeouts/string identities and SHA-256 fallback for HTTP.

Disagree with Claude's first review findings 1/4: they applied _one-dimensional_
noise-energy formulas to the separable two-dimensional kernel. For a 1D kernel
g, the 2D high-pass norm is sqrt(1 + (sum(g²))² - 2\*g_center²), and the 2D
smoothing norm is sum(g²). Retained those formulas and verified a seeded sigma 5
white-noise frame recovers sigma 4.953. SciPy/NumPy reference planes independently
check reflection, Gaussian convolution, morphology, RL and the tapered Wiener
FFT. The review's earlier stale 0.5 floor/K/ROI comments are superseded by the
updated code and regression tests. Full-frame limits remain explicit, with a
separate smaller Wiener FFT budget; oversized work fails without downsampling.

## Implementation review refinements

Claude's final integration review found duplicate old-frame renders during
playback and an Apply/decode race. Fixed by repainting on invalidation only
when a derived display/candidate overlay needs removal, and disabling Apply
while playing/decoding. A visible-browser regression counts extra callbacks
and holds a decode pending to check the Apply guard.

Also addressed the presentation findings: display limits use full-frame
percentiles independently of ROI, histogram backing storage follows CSS size
and device-pixel ratio, and detection records omit the view-at-click field
(the finding records the view when marked). Kept original/raw temperature
provenance, code/PSF/noise assumptions, source-generation and frame hash checks.

Claude verified the five integration fixes with no remaining blockers. Also fixed
the minor ROI outline repaint on panel toggles/region edits using an overlay-only
callback; a browser test checks that closing/reopening removes/restores it.

Final qualification: 101 unit tests passed (one optional-data test skipped),
27 browser regression tests passed in Xephyr (five optional-data tests skipped),
and the real-flight test passed separately against the 2.6 GB thermal recording.
The real-flight test checks both restoration modes and a mid-flight raw-frame
SHA-256 against independently decoded reference pixels. Build and formatting
checks passed. These checks establish operation and raw preservation, not field
detection sensitivity.

After the ROI overlay refinement, all six thermal-analysis browser tests passed
again in Xephyr, including the real-flight/mid-flight hash check.
