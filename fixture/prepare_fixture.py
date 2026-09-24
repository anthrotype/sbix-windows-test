#!/usr/bin/env python3
"""Fetch public Noto 3D and make a small, one-strike sbix test font."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import tempfile
import urllib.request
from pathlib import Path

import uharfbuzz as hb
from fontTools import subset
from fontTools.ttLib import TTFont


UPSTREAM_REVISION = "06121655d0e82f9cae6e7ba6feed4fa6fdbfc2a4"
UPSTREAM_FONT_PATH = "3D/fonts/Noto-3D-128.ttf"
UPSTREAM_FONT_SHA256 = "6f4312a7c02d0c9de88095ee65e320ffe9316d2d8d56e4c515e7181fca5ed439"
UPSTREAM_LICENSE_PATH = "3D/fonts/LICENSE"
SELECTED_PPEM = 109
CASES = [
    {"id": "grinning-face", "text": "😀"},
    {"id": "family-zwj", "text": "👨‍👩‍👧‍👦"},
    {"id": "woman-technologist-zwj", "text": "👩‍💻"},
]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def download(url: str, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    request = urllib.request.Request(url, headers={"User-Agent": "sbix-windows-test/1.0"})
    with urllib.request.urlopen(request, timeout=180) as response, destination.open("wb") as out:
        shutil.copyfileobj(response, out, length=1024 * 1024)


def shape(font_path: Path, ttfont: TTFont, text: str) -> list[str]:
    face = hb.Face(hb.Blob.from_file_path(str(font_path)))
    hb_font = hb.Font(face)
    hb_font.scale = (face.upem, face.upem)
    buffer = hb.Buffer()
    buffer.add_str(text)
    buffer.guess_segment_properties()
    hb.shape(hb_font, buffer)
    order = ttfont.getGlyphOrder()
    return [order[info.codepoint] for info in buffer.glyph_infos]


def outline_signature(font: TTFont, glyph_name: str) -> tuple:
    glyph = font["glyf"][glyph_name]
    coordinates, endpoints, flags = glyph.getCoordinates(font["glyf"])
    return (
        tuple((int(x), int(y)) for x, y in coordinates),
        tuple(int(point) for point in endpoints),
        tuple(int(flag) for flag in flags),
    )


def metric_signature(font: TTFont, glyph_name: str) -> dict[str, list[int]]:
    return {
        "hmtx": list(font["hmtx"].metrics[glyph_name]),
        "vmtx": list(font["vmtx"].metrics[glyph_name]),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--source-font", type=Path, help="Use an already-downloaded upstream TTF.")
    args = parser.parse_args()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    subset_path = output_dir / "noto-3d-sbix-test.ttf"
    manifest_path = output_dir / "manifest.json"
    license_path = output_dir / "Noto-3D-OFL.txt"

    if args.source_font:
        source_path = args.source_font.resolve()
        scratch = None
    else:
        scratch = tempfile.TemporaryDirectory(prefix="noto-3d-source-")
        source_path = Path(scratch.name) / Path(UPSTREAM_FONT_PATH).name
        url = (
            "https://media.githubusercontent.com/media/googlefonts/noto-emoji/"
            f"{UPSTREAM_REVISION}/{UPSTREAM_FONT_PATH}"
        )
        print(f"Downloading {UPSTREAM_FONT_PATH} at {UPSTREAM_REVISION}", flush=True)
        download(url, source_path)

    try:
        source_digest = sha256(source_path)
        if source_digest != UPSTREAM_FONT_SHA256:
            raise RuntimeError(
                f"Unexpected upstream font SHA-256: {source_digest}; "
                f"expected {UPSTREAM_FONT_SHA256}"
            )

        license_url = (
            "https://raw.githubusercontent.com/googlefonts/noto-emoji/"
            f"{UPSTREAM_REVISION}/{UPSTREAM_LICENSE_PATH}"
        )
        download(license_url, license_path)

        source = TTFont(source_path)
        if "sbix" not in source:
            raise RuntimeError("The selected upstream font has no sbix table")
        strikes = source["sbix"].strikes
        if SELECTED_PPEM not in strikes:
            raise RuntimeError(
                f"Expected {SELECTED_PPEM} ppem strike; available strikes: {sorted(strikes)}"
            )
        source_strike = strikes[SELECTED_PPEM]
        source_family = source["name"].getDebugName(1)
        source_glyph_count = len(source.getGlyphOrder())

        shaped: dict[str, list[str]] = {}
        source_signatures: dict[str, dict[str, object]] = {}
        for case in CASES:
            glyph_names = shape(source_path, source, case["text"])
            if len(glyph_names) != 1:
                raise RuntimeError(
                    f"Upstream {case['id']} shapes to {len(glyph_names)} glyphs: {glyph_names}"
                )
            glyph_name = glyph_names[0]
            glyph = source_strike.glyphs.get(glyph_name)
            if glyph is None or not glyph.imageData:
                raise RuntimeError(
                    f"Upstream {case['id']} maps to {glyph_name}, which has no {SELECTED_PPEM} ppem sbix image"
                )
            if not glyph.imageData.startswith(b"\x89PNG\r\n\x1a\n"):
                raise RuntimeError(f"{case['id']} does not use a PNG sbix image")
            shaped[case["id"]] = glyph_names
            source_signatures[case["id"]] = {
                "glyph_id": source.getGlyphID(glyph_name),
                "glyph_name": glyph_name,
                "metrics": metric_signature(source, glyph_name),
                "outline": outline_signature(source, glyph_name),
            }

        options = subset.Options()
        options.glyph_names = True
        options.hinting = True
        options.layout_features = ["*"]
        options.recalc_bounds = False
        subsetter = subset.Subsetter(options=options)
        subsetter.populate(text="".join(case["text"] for case in CASES))
        subsetter.subset(source)

        # Keep the font's real outline/metric data and color PNGs, but only one sbix strike.
        source["sbix"].strikes = {
            SELECTED_PPEM: source["sbix"].strikes[SELECTED_PPEM]
        }
        source.save(subset_path)
        output = TTFont(subset_path)
        if "sbix" not in output or list(output["sbix"].strikes) != [SELECTED_PPEM]:
            raise RuntimeError("The generated fixture does not contain exactly one sbix strike")
        if output["head"].unitsPerEm != source["head"].unitsPerEm:
            raise RuntimeError("Subsetting changed unitsPerEm")

        case_manifest = []
        output_strike = output["sbix"].strikes[SELECTED_PPEM]
        for case in CASES:
            names = shape(subset_path, output, case["text"])
            if len(names) != 1:
                raise RuntimeError(
                    f"Subset {case['id']} shapes to {len(names)} glyphs: {names}"
                )
            glyph_name = names[0]
            expected_glyph_id = subsetter.glyph_index_map[source_signatures[case["id"]]["glyph_id"]]
            expected_glyph_name = output.getGlyphOrder()[expected_glyph_id]
            if glyph_name != expected_glyph_name:
                raise RuntimeError(
                    f"Subset {case['id']} glyph mapping changed unexpectedly: "
                    f"HarfBuzz={glyph_name}, source-to-subset={expected_glyph_name}"
                )
            image = output_strike.glyphs.get(glyph_name)
            if image is None or not image.imageData:
                raise RuntimeError(f"Subset {case['id']} has no sbix image for {glyph_name}")
            if not image.imageData.startswith(b"\x89PNG\r\n\x1a\n"):
                raise RuntimeError(f"Subset {case['id']} image is not PNG")

            after_metrics = metric_signature(output, glyph_name)
            if source_signatures[case["id"]]["metrics"] != after_metrics:
                raise RuntimeError(f"Subsetting changed advance/bearing metrics for {glyph_name}")
            if source_signatures[case["id"]]["outline"] != outline_signature(output, glyph_name):
                raise RuntimeError(f"Subsetting changed glyf outline geometry for {glyph_name}")

            case_manifest.append(
                {
                    **case,
                    "codepoints": [f"U+{ord(char):04X}" for char in case["text"]],
                    "glyph_name": glyph_name,
                    "glyph_id": output.getGlyphID(glyph_name),
                    "glyph_count": 1,
                    "image_format": "PNG",
                    "image_bytes": len(image.imageData),
                    "metrics": after_metrics,
                    "outline_preserved": True,
                }
            )

        manifest = {
            "source": {
                "repository": "https://github.com/googlefonts/noto-emoji",
                "revision": UPSTREAM_REVISION,
                "path": UPSTREAM_FONT_PATH,
                "sha256": source_digest,
                "license_path": UPSTREAM_LICENSE_PATH,
                "license_file": license_path.name,
            },
            "font": {
                "family_name": source_family,
                "units_per_em": output["head"].unitsPerEm,
                "source_glyph_count": source_glyph_count,
                "subset_glyph_count": len(output.getGlyphOrder()),
                "sbix_strikes": [
                    {"ppem": SELECTED_PPEM, "resolution": output_strike.resolution}
                ],
                "file": subset_path.name,
                "file_bytes": subset_path.stat().st_size,
                "subset_sha256": sha256(subset_path),
                "metric_outline_policy": (
                    "Keep the source glyf outlines and hmtx/vmtx metrics for each shaped test glyph; "
                    "drop unrelated glyphs and all sbix strikes except 109 ppem."
                ),
            },
            "cases": case_manifest,
        }
        manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
        print(json.dumps(manifest, ensure_ascii=False, indent=2), flush=True)
    finally:
        if scratch is not None:
            scratch.cleanup()


if __name__ == "__main__":
    main()
