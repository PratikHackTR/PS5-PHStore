#!/usr/bin/env python3
"""Create the PS5 icon and C-embedded frontend/assets for PH Store."""
import argparse
import json
from pathlib import Path

from PIL import Image


def c_bytes(name: str, data: bytes, storage: str = "static const") -> str:
    rows = [", ".join(f"0x{value:02x}" for value in data[i:i + 16]) for i in range(0, len(data), 16)]
    return f"{storage} unsigned char {name}[] = {{\n    " + ",\n    ".join(rows) + "\n};\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-type", choices=["Debug", "Release"], required=True)
    parser.add_argument("--source-logo", type=Path, default=Path("resim/phstorelogo.png"))
    parser.add_argument("--param-json", type=Path, default=Path("assets/phstore-param.json"))
    parser.add_argument("--icon-output", type=Path, default=Path("assets/generated/icon0.png"))
    parser.add_argument("--header-output", type=Path, default=Path("build/phstore_assets.h"))
    parser.add_argument("--source-output", type=Path, default=Path("build/phstore_assets.c"))
    args = parser.parse_args()
    development = args.build_type == "Debug"

    args.icon_output.parent.mkdir(parents=True, exist_ok=True)
    with Image.open(args.source_logo) as source:
        if source.format != "PNG" or source.size != (1254, 1254):
            raise SystemExit("PH Store logo source changed; review expected 1254x1254 PNG before regenerating.")
        # Match the reference project's shipped 1024x1024 PS5 icon0.png.
        icon = source.convert("RGBA").resize((1024, 1024), Image.Resampling.LANCZOS)
        icon.save(args.icon_output, format="PNG", optimize=True)
    icon_bytes = args.icon_output.read_bytes()
    param_json = args.param_json.read_bytes()
    parsed_param = json.loads(param_json)
    if parsed_param.get("titleId") != "PHST00002" or parsed_param.get("deeplinkUri") != "http://127.0.0.1:1903/":
        raise SystemExit("Shortcut metadata must use PHST00002 and the loopback PH Store URL.")

    html = Path("frontend/index.html").read_text(encoding="utf-8")
    css = Path("frontend/src/style.css").read_text(encoding="utf-8")
    virtual_grid = Path("frontend/src/virtual-grid.js").read_text(encoding="utf-8")
    js = Path("frontend/src/app.js").read_text(encoding="utf-8")
    js = "window.PHSTORE_DEBUG_BUILD=" + ("true" if development else "false") + ";\n" + virtual_grid + "\n" + js
    html = html.replace("/*PHSTORE_CSS*/", css).replace("/*PHSTORE_JS*/", js)
    if "PHSTORE_CSS" in html or "PHSTORE_JS" in html:
        raise SystemExit("Unexpanded frontend template marker")

    catalog = Path('config/generated/embedded-catalog.json').read_bytes()
    json.loads(catalog)
    manifests = json.loads(Path('config/generated/embedded-manifests.json').read_text(encoding='utf-8'))
    header = ["#ifndef PHSTORE_GENERATED_ASSETS_H\n#define PHSTORE_GENERATED_ASSETS_H\n#include <stddef.h>\n\n",
              "typedef struct { const char *path; const unsigned char *data; size_t size; const char *mime; } phstore_embedded_asset_t;\n",
              "extern const unsigned char phstore_frontend_html[];\nextern const size_t phstore_frontend_html_size;\n",
              "extern const unsigned char phstore_shortcut_icon[];\nextern const size_t phstore_shortcut_icon_size;\n",
              "extern const unsigned char phstore_shortcut_param_json[];\nextern const size_t phstore_shortcut_param_json_size;\n",
              "extern const phstore_embedded_asset_t phstore_embedded_assets[];\nextern const size_t phstore_embedded_asset_count;\n",
              "extern const unsigned char phstore_catalog_json[];\nextern const size_t phstore_catalog_json_size;\nextern const phstore_embedded_asset_t phstore_embedded_manifests[];\nextern const size_t phstore_embedded_manifest_count;\n",
              "#define PHSTORE_EMBEDDED_ASSET_COUNT phstore_embedded_asset_count\n#endif\n"]
    chunks = ['#include "phstore_assets.h"\n\n',
              c_bytes("phstore_frontend_html", html.encode("utf-8"), "const"),
              "const size_t phstore_frontend_html_size = sizeof(phstore_frontend_html);\n",
              c_bytes("phstore_shortcut_icon", icon_bytes, "const"),
              "const size_t phstore_shortcut_icon_size = sizeof(phstore_shortcut_icon);\n",
              c_bytes("phstore_shortcut_param_json", param_json, "const"),
              "const size_t phstore_shortcut_param_json_size = sizeof(phstore_shortcut_param_json);\n",
              ]
    chunks.extend([c_bytes("phstore_catalog_json", catalog, "const"), "const size_t phstore_catalog_json_size = sizeof(phstore_catalog_json);\n"])
    manifest_entries = []
    for index, (pid, manifest) in enumerate(sorted(manifests.items())):
        symbol = f"phstore_manifest_{index}"
        payload = json.dumps(manifest,ensure_ascii=False,separators=(',',':')).encode('utf-8')
        chunks.append(c_bytes(symbol,payload))
        manifest_entries.append(f'    {{ {json.dumps(pid)}, {symbol}, sizeof({symbol}), "application/json" }},\n')
    chunks.append("const phstore_embedded_asset_t phstore_embedded_manifests[] = {\n" + ''.join(manifest_entries) + "};\nconst size_t phstore_embedded_manifest_count = sizeof(phstore_embedded_manifests)/sizeof(phstore_embedded_manifests[0]);\n")
    asset_entries = ['    { "/assets/phstore-logo.png", phstore_shortcut_icon, sizeof(phstore_shortcut_icon), "image/png" },\n']
    cover_urls=Path('config/generated/ph-ps5-covers.txt').read_bytes()
    chunks.append(c_bytes('phstore_ps5_cover_urls',cover_urls))
    asset_entries.append('    { "/metadata/ph-ps5-covers.txt", phstore_ps5_cover_urls, sizeof(phstore_ps5_cover_urls), "text/plain" },\n')
    args.header_output.parent.mkdir(parents=True, exist_ok=True)
    args.header_output.write_text("".join(header), encoding="utf-8", newline="\n")
    chunks.append("const phstore_embedded_asset_t phstore_embedded_assets[] = {\n")
    chunks.extend(asset_entries)
    chunks.append("};\nconst size_t phstore_embedded_asset_count = sizeof(phstore_embedded_assets) / sizeof(phstore_embedded_assets[0]);\n")
    args.source_output.write_text("".join(chunks), encoding="utf-8", newline="\n")
    print(f"Embedded {args.build_type} frontend; icon 1254x1254 -> 1024x1024 ({len(icon_bytes)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
