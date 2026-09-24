# Noto 3D sbix Windows test

This repository checks whether Windows' native DirectWrite/Direct2D path and a downloadable browser web font can render real Noto 3D `sbix` PNGs. The two paths run as separate Actions jobs: a native failure remains visible even if Edge passes.

The test uses the regular public [`Noto-3D-128.ttf`](https://github.com/googlefonts/noto-emoji/blob/06121655d0e82f9cae6e7ba6feed4fa6fdbfc2a4/3D/fonts/Noto-3D-128.ttf), not the watch build. Its source is pinned to noto-emoji commit [`06121655d0e82f9cae6e7ba6feed4fa6fdbfc2a4`](https://github.com/googlefonts/noto-emoji/commit/06121655d0e82f9cae6e7ba6feed4fa6fdbfc2a4) and checked against SHA-256 `6f4312a7c02d0c9de88095ee65e320ffe9316d2d8d56e4c515e7181fca5ed439`. The fixture job downloads that 147,882,884-byte upstream file once, then subsets it to three sequences and one 109-ppem strike. Only the small subset and its OFL license are passed to the Windows jobs; the full font is neither committed nor uploaded.

The sequences are U+1F600 (`😀`), U+1F468 U+200D U+1F469 U+200D U+1F467 U+200D U+1F466 (family ZWJ), and U+1F469 U+200D U+1F4BB (woman technologist ZWJ). The fixture generator checks that HarfBuzz shapes each to one glyph and that the glyph has a PNG in the selected strike. It uses FontTools subsetting to retain the required cmap/GSUB mappings, glyph outlines and metrics. It compares the shaped glyphs' `glyf` geometry and `hmtx`/`vmtx` metrics against the upstream font, and records those checks in `manifest.json`. The only deliberate image-table change is dropping all `sbix` strikes except 109 ppem.

## What the jobs prove

- **Native DirectWrite + Direct2D:** opens the subset by file reference without installing it, shapes each sequence using an explicit `IDWriteFontFace`, requests PNG glyph data from DirectWrite, translates color glyph runs, and draws them into a D3D11 WARP-backed Direct2D target. It saves offscreen PNGs and fails if the output is blank; from 64 px upward it also requires chromatic pixels. At 32 px the family design's few saturated details can disappear during downsampling. There is no system-font fallback path in this probe.
- **Microsoft Edge:** loads the same fixture through a downloadable `@font-face`, records Edge and OS versions, and saves screenshots. The test uses Chromium's `CSS.getPlatformFontsForNode` to require the custom Noto face and expected glyph count; it also rejects blank output and requires chromatic pixels from 64 px upward.

The Edge result is deliberately not treated as evidence for native DirectWrite rendering. Chromium's `MakeSbixTypeface()` selects Fontations for sbix on Windows in [this Chromium source revision](https://chromium.googlesource.com/chromium/src/+/d728134238db029c2251e3faf56ff2429166a00d/third_party/blink/renderer/platform/fonts/web_font_typeface_factory.cc). Microsoft documents native sbix support in DirectWrite/Direct2D since Windows 10 version 1607, with applications opting in to color glyph drawing ([color font support](https://learn.microsoft.com/en-us/windows/win32/directwrite/color-fonts)).

The jobs run on GitHub's `windows-11-arm` hosted runner and record its actual OS build and architecture. This is Windows 11 ARM64 client OS coverage; it is not x64 coverage and does not establish compatibility for every Windows application or installation/system-emoji-replacement scenario. The motivating report is [noto-emoji issue #567](https://github.com/googlefonts/noto-emoji/issues/567), which does not identify the application or error.

## Reproduce

Fixture generation requires Python 3.12 and the pinned dependencies in `fixture/requirements.txt`:

```sh
python -m pip install -r fixture/requirements.txt
python fixture/prepare_fixture.py --output-dir artifacts/fixture
```

On Windows with Visual Studio C++ tools and the Windows SDK, compile and run the native probe:

```powershell
cl /nologo /EHsc /std:c++17 /utf-8 /W4 /DUNICODE /D_UNICODE native\sbix_probe.cpp /Fe:artifacts\sbix-native-probe.exe
artifacts\sbix-native-probe.exe artifacts\fixture\noto-3d-sbix-test.ttf artifacts\native
```

For the browser path, install Edge and run:

```sh
npm ci --prefix browser
node browser/run.mjs --font artifacts/fixture/noto-3d-sbix-test.ttf --manifest artifacts/fixture/manifest.json --output-dir artifacts/browser
```

Each Actions run retains fixture metadata, logs, and PNG output for 30 days. A passing browser job cannot mask a failing native job.
