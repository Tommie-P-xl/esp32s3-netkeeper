// Generate an LVGL font that actually contains the simplified Chinese glyphs the
// UI uses. The stock lv_font_source_han_sans_sc_16_cjk ships a Japanese-leaning
// subset missing 网/认/证/检/测, so it cannot render this screen.
//
// usage:
//   python tools/collect_symbols.py     # refresh symbols.txt from the sources
//   cd tools/fontgen && npm install
//   node gen.js                         # writes ui_font_18.c here
//   copy ui_font_18.c ..\..\main\
//
// The TTF defaults to SimHei on Windows; override with the FONT env var, e.g.
//   FONT=/usr/share/fonts/truetype/noto/NotoSansCJKsc-Regular.otf node gen.js
//
// lv_font_conv has no library entry point (its main file is the CLI and runs
// immediately on require), but lib/cli.js exports run(argv).
const fs = require('fs');
const path = require('path');
const cli = require('lv_font_conv/lib/cli.js');

const out = path.join(__dirname, 'ui_font_18.c');
const symbols = fs.readFileSync(path.join(__dirname, 'symbols.txt'), 'utf8').trim();
const font = process.env.FONT || 'C:/Windows/Fonts/simhei.ttf';

if (!fs.existsSync(font)) {
  console.error('font file not found: ' + font);
  console.error('set FONT=<path to a CJK .ttf/.otf> and run again');
  process.exit(1);
}

const argv = [
  '--font', font,
  '--size', '18',
  '--bpp', '4',
  '--format', 'lvgl',
  '--lv-include', 'lvgl.h',
  '--output', out,
  '--range', '0x20-0x7f',
  '--symbols', symbols,
  '--no-compress',
  '--no-prefilter',
];

cli.run(argv).then(() => {
  const size = fs.statSync(out).size;
  console.log('font generated: %d bytes, %d symbols', size, symbols.length);
}).catch((err) => {
  console.error(err);
  process.exit(1);
});
