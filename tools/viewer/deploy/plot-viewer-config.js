// Public deployment settings; videos and flight logs remain in the browser.
window.VIEWER_CONFIG = {
  imagery: 'satellite',
  googleMapsApiKey: '',
  terrainUrl: '/quantized/',
  terrainDatum: 'amsl', // The plot tiler preserves SRTM/EGM96 orthometric heights.
  amslToEllipsoidMetres: 0,
  websplatUrl: '../websplat/', // Create splat opens WebSplat here (same origin).
};
