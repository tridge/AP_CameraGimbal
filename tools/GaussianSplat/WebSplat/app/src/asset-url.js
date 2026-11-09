// URL of a file the app serves (vendor/...): under vite's base, so a deployed release (built with
// base ./releases/<release>/, scripts/deploy.sh) loads its own copies whichever release is live
export const assetURL = (path) =>
  new URL((import.meta.env?.BASE_URL ?? './') + path, globalThis.document?.baseURI ?? location.href).href;
