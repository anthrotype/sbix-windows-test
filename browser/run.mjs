import { createServer } from 'node:http';
import { createHash } from 'node:crypto';
import { readFile, mkdir, writeFile } from 'node:fs/promises';
import { arch, release, version } from 'node:os';
import path from 'node:path';
import { chromium } from 'playwright';
import { PNG } from 'pngjs';

function option(name, fallback = undefined) {
  const index = process.argv.indexOf(name);
  if (index === -1) return fallback;
  const value = process.argv[index + 1];
  if (!value) throw new Error(`Missing value for ${name}`);
  return value;
}

const fontPath = path.resolve(option('--font'));
const manifestPath = path.resolve(option('--manifest'));
const outputDir = path.resolve(option('--output-dir'));
const manifest = JSON.parse(await readFile(manifestPath, 'utf8'));
const fontBytes = await readFile(fontPath);
const actualHash = createHash('sha256').update(fontBytes).digest('hex');
if (actualHash !== manifest.font.subset_sha256) {
  throw new Error(`Fixture SHA-256 ${actualHash} != manifest ${manifest.font.subset_sha256}`);
}
await mkdir(outputDir, { recursive: true });

const pageHtml = `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <style>
    @font-face {
      font-family: "Noto3DSbixFixture";
      src: url("/font.ttf") format("truetype");
      font-style: normal;
      font-weight: 400;
      font-display: block;
    }
    html, body { margin: 0; padding: 0; background: #fff; }
    #capture { display: inline-block; min-width: 190px; min-height: 190px; padding: 24px; background: #fff; }
    #glyph { display: inline-block; color: #111; font-family: "Noto3DSbixFixture", sans-serif; line-height: 1; white-space: nowrap; }
  </style>
</head>
<body><div id="capture"><span id="glyph"></span></div></body>
</html>`;

const server = createServer(async (request, response) => {
  if (request.url === '/') {
    response.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    response.end(pageHtml);
    return;
  }
  if (request.url === '/font.ttf') {
    response.writeHead(200, {
      'Content-Type': 'font/ttf',
      'Content-Length': fontBytes.length,
      'Cache-Control': 'no-store',
    });
    response.end(fontBytes);
    return;
  }
  response.writeHead(404);
  response.end('Not found');
});

await new Promise((resolve, reject) => {
  server.once('error', reject);
  server.listen(0, '127.0.0.1', resolve);
});

let browser;
const diagnostics = {
  os: { platform: process.platform, release: release(), version: version(), arch: arch() },
  runtime: { node: process.version, playwright: '1.63.0' },
  fixture: {
    source_revision: manifest.source.revision,
    source_path: manifest.source.path,
    family_name: manifest.font.family_name,
    sha256: actualHash,
    ppem: manifest.font.sbix_strikes[0].ppem,
  },
  results: [],
};

function inspectPng(buffer) {
  const png = PNG.sync.read(buffer);
  let nonBackground = 0;
  let chromaticPixels = 0;
  const colors = new Set();
  const chromaticColors = new Set();
  for (let i = 0; i < png.data.length; i += 4) {
    const r = png.data[i];
    const g = png.data[i + 1];
    const b = png.data[i + 2];
    if (r < 248 || g < 248 || b < 248) {
      nonBackground += 1;
      const color = `${r >> 3}:${g >> 3}:${b >> 3}`;
      colors.add(color);
      if (Math.max(r, g, b) - Math.min(r, g, b) >= 8) {
        chromaticPixels += 1;
        chromaticColors.add(color);
      }
    }
  }
  return {
    width: png.width,
    height: png.height,
    nonBackgroundPixels: nonBackground,
    quantizedColors: colors.size,
    chromaticPixels,
    chromaticColors: chromaticColors.size,
  };
}

try {
  browser = await chromium.launch({ channel: 'msedge', headless: true });
  diagnostics.browser = {
    name: 'Microsoft Edge (Playwright channel msedge)',
    version: browser.version(),
  };
  const context = await browser.newContext({ viewport: { width: 320, height: 250 }, deviceScaleFactor: 1 });
  const page = await context.newPage();
  await page.goto(`http://127.0.0.1:${server.address().port}/`, { waitUntil: 'load' });
  diagnostics.browser.userAgent = await page.evaluate(() => navigator.userAgent);
  const cdp = await context.newCDPSession(page);
  await cdp.send('DOM.enable');
  await cdp.send('CSS.enable');

  for (const testCase of manifest.cases) {
    for (const size of [32, 64, 96, 128]) {
      await page.evaluate(async ({ text, size }) => {
        const glyph = document.querySelector('#glyph');
        glyph.textContent = text;
        glyph.style.fontSize = `${size}px`;
        await document.fonts.load(`${size}px "Noto3DSbixFixture"`, text);
        await document.fonts.ready;
      }, { text: testCase.text, size });

      const loaded = await page.evaluate(({ text, size }) =>
        document.fonts.check(`${size}px "Noto3DSbixFixture"`, text),
      { text: testCase.text, size });
      if (!loaded) throw new Error(`${testCase.id} @ ${size}px: @font-face did not load`);

      const { root } = await cdp.send('DOM.getDocument', { depth: -1 });
      const { nodeId } = await cdp.send('DOM.querySelector', { nodeId: root.nodeId, selector: '#glyph' });
      const { fonts } = await cdp.send('CSS.getPlatformFontsForNode', { nodeId });
      if (fonts.length !== 1 || fonts[0].familyName !== manifest.font.family_name || !fonts[0].isCustomFont) {
        throw new Error(`${testCase.id} @ ${size}px used fallback or multiple faces: ${JSON.stringify(fonts)}`);
      }
      if (fonts[0].glyphCount !== testCase.glyph_count) {
        throw new Error(
          `${testCase.id} @ ${size}px used ${fonts[0].glyphCount} glyphs; expected ${testCase.glyph_count}`,
        );
      }

      const filename = `${testCase.id}_${size}.png`;
      const screenshot = await page.locator('#capture').screenshot({ path: path.join(outputDir, filename) });
      const pixels = inspectPng(screenshot);
      if (pixels.nonBackgroundPixels < 100 || pixels.quantizedColors < 4 ||
          (size >= 64 && (pixels.chromaticPixels === 0 || pixels.chromaticColors === 0))) {
        throw new Error(`${filename} is blank or monochrome: ${JSON.stringify(pixels)}`);
      }
      const result = {
        case: testCase.id,
        codepoints: testCase.codepoints,
        size,
        platformFonts: fonts,
        screenshot: filename,
        pixels,
      };
      diagnostics.results.push(result);
      console.log(JSON.stringify(result));
    }
  }

  diagnostics.passed = `${diagnostics.results.length} case/size renders; all used the downloaded custom font and colorful PNG pixels`;
  console.log(JSON.stringify({ browser: diagnostics.browser, os: diagnostics.os, passed: diagnostics.passed }));
} catch (error) {
  diagnostics.error = error instanceof Error ? error.stack : String(error);
  throw error;
} finally {
  if (browser) await browser.close();
  await writeFile(path.join(outputDir, 'browser-results.json'), `${JSON.stringify(diagnostics, null, 2)}\n`);
  await new Promise((resolve) => server.close(resolve));
}
