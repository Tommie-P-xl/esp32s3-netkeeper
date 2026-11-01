"""Collect every non-ASCII character used by the firmware sources.

The UI renders Chinese labels and the status line prints Chinese detail text, so
the generated LVGL font must cover the union of all of them. Scanning the sources
(comments included, harmless superset) is cheaper and safer than hand-listing.

Run from anywhere:  python tools/collect_symbols.py
Output:             tools/fontgen/symbols.txt  (consumed by tools/fontgen/gen.js)
"""

import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"
OUT = ROOT / "tools" / "fontgen" / "symbols.txt"
OUT.parent.mkdir(parents=True, exist_ok=True)

chars = set()
for f in sorted(MAIN.glob("*.c")) + sorted(MAIN.glob("*.h")):
    text = f.read_text(encoding="utf-8", errors="replace")
    for ch in text:
        if ord(ch) > 0x7F:
            chars.add(ch)

# 常用全角标点兜底（运行期拼出来的字符串也要能显示）
for ch in "（），、：。；！？“”%·-—":
    chars.add(ch)

ordered = "".join(sorted(chars))
OUT.write_text(ordered, encoding="utf-8")

print("unique non-ascii chars: %d" % len(chars))
print("sample:", ordered[:60])
print("written to", OUT)
