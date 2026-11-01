// Public runtime settings. Browser API keys must be restricted to the hosting
// origin; selected videos/logs are never sent to imagery providers.
window.VIEWER_CONFIG = {
  imagery: 'satellite', // none, osm, satellite, google
  googleMapsApiKey: '',
  terrainUrl: 'https://plot.ardupilot.org/quantized/', // Shared terrain, no local service required.
  terrainDatum: 'amsl', // The shared service preserves SRTM/EGM96 orthometric heights.
  amslToEllipsoidMetres: 0, // Explicit geoid separation; configure for your area.
};
