#!/usr/bin/env python3
import argparse, json, sys, zipfile
from pathlib import Path

def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--library", type=Path, required=True)
    p.add_argument("--icon", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    a = p.parse_args()
    lib, icon, out = a.library.resolve(), a.icon.resolve(), a.output.resolve()
    if not lib.is_file() or not icon.is_file():
        print("missing library or icon", file=sys.stderr)
        return 1
    # Levi matches exact strings in minecraft_versions — list current known builds
    manifest = {
        "type": "preload-native",
        "name": "BactroNative",
        "author": "hater12132-ai",
        "version": "1.26.0",
        "entry": "libBactroNative.so",
        "icon": "icon.png",
        "minecraft_versions": [
            "1.26.51.1",
            "1.26.52",
            "1.26.52.1",
            "1.26.52.2",
            "1.26.52.3",
            "1.26.52.4",
            "1.26.52.5",
        ],
        "description": "Snow Chams + Solstice box ESP + ChamsESP. Target 1.26.51–1.26.52.x",
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
        z.write(lib, "libBactroNative.so")
        z.write(icon, "icon.png")
    print(out)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
