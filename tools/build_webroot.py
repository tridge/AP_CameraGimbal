#!/usr/bin/env python3
"""Generate the exact allowlist shared by serving and firmware installation."""
from pathlib import Path
import json
import sys

root, output = map(Path, sys.argv[1:])
names = sorted(path.name for path in root.iterdir() if path.is_file())
if not names or any(not name.replace("-", "").replace("_", "").replace(".", "").isalnum()
                    or Path(name).suffix not in {".html", ".css", ".js"} for name in names):
    raise SystemExit("invalid webroot asset name")
text = "#pragma once\nstatic const char *const apcam_web_assets[] = {\n" + \
       "".join("    " + json.dumps(name) + ",\n" for name in names) + "};\n"
# Z1 can receive a new web binary through an older updater that cannot install
# asset entries. Keep only its recovery uploader in the binary, generated from
# the same source as the normal UI to avoid a second upload implementation.
text += "static const char apcam_recovery_upgrade_js[] =\n" + \
        "\n".join(json.dumps(line, ensure_ascii=True) for line in
                  (root / "upgrade.js").read_text().splitlines(keepends=True)) + ";\n"
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != text:
    output.write_text(text)
