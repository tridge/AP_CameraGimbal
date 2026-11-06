The camera/map icon is supplied by Andrew Tridgell. `camera-gimbal.svg` is
the source artwork, preserved with its embedded provenance metadata.
It is also the project icon in the video viewer header, browser tab and README
titles; the viewer imports this source directly rather than maintaining a copy.

`camera-gimbal.ico` contains 16, 24, 32, 48, 64, 128 and 256 pixel images for
the SITL launcher, Windows executables and installer. `favicon.ico` contains
16, 32 and 48 pixel images as a fallback for the web SVG favicon.

Regenerate the ICOs with `python3 tools/build_icons.py --render` using
PyGObject/librsvg, Cairo and Pillow. Each size is rendered directly from the SVG.
Normal builds embed the committed web assets using only Python's standard library.
