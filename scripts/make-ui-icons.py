#!/usr/bin/env python3
"""Renders 3dsDL's interface icons into one atlas for the console.

The icons are the 24 px stroke drawings from the design canvas (see
docs/DESIGN.md), apart from the filled settings cog and star. Each is drawn white on transparent at 64, 32, 24 and 18 px
by headless Chrome, and the app tints them when it draws them.

Writes romfs/ui/icons.png and source/app/icon_atlas.hpp. Run it again after
changing an icon:

    python3 scripts/make-ui-icons.py
"""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHROME = os.environ.get(
    "CHROME", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")

# Name and SVG body on a 24 x 24 grid, drawn with a 2 px round stroke.
ICONS = [
    ("AppMark", '<path d="M7 3.5h7.5L19 8v11a1.5 1.5 0 0 1-1.5 1.5h-10A1.5 1.5 0 0 1 6 19V5a1.5 1.5 0 0 1 1-1.5z"/><path d="M12.5 8.5v7M9.5 12.5l3 3 3-3"/><path d="M9.5 18h6"/>'),
    ("Scan", '<path d="M4 9V5a1 1 0 0 1 1-1h4M15 4h4a1 1 0 0 1 1 1v4M20 15v4a1 1 0 0 1-1 1h-4M9 20H5a1 1 0 0 1-1-1v-4"/><rect x="8" y="8" width="3" height="3"/><rect x="13" y="8" width="3" height="3"/><rect x="8" y="13" width="3" height="3"/><path d="M13 13h3v3"/>'),
    ("Globe", '<circle cx="12" cy="12" r="8"/><path d="M4 12h16M12 4c2.4 2.3 3.5 5 3.5 8s-1.1 5.7-3.5 8c-2.4-2.3-3.5-5-3.5-8s1.1-5.7 3.5-8z"/>'),
    ("History", '<path d="M4.5 12a7.5 7.5 0 1 0 2.2-5.3"/><path d="M4 4.5v3.5h3.5"/><path d="M12 8v4.5l3 2"/>'),
    ("Folder", '<path d="M3.5 7.5a1.5 1.5 0 0 1 1.5-1.5h4l2 2h8a1.5 1.5 0 0 1 1.5 1.5v8a1.5 1.5 0 0 1-1.5 1.5H5a1.5 1.5 0 0 1-1.5-1.5z"/>'),
    # The cog is filled: drawn in strokes, its eight teeth blur together at
    # 18 px.
    ("Gear", '<path fill="#fff" stroke="none" fill-rule="evenodd" d="M10.08 4.85L10.26 2.15A10 10 0 0 1 13.74 2.15L13.92 4.85A7.4 7.4 0 0 1 15.70 5.59L17.74 3.81A10 10 0 0 1 20.19 6.26L18.41 8.30A7.4 7.4 0 0 1 19.15 10.08L21.85 10.26A10 10 0 0 1 21.85 13.74L19.15 13.92A7.4 7.4 0 0 1 18.41 15.70L20.19 17.74A10 10 0 0 1 17.74 20.19L15.70 18.41A7.4 7.4 0 0 1 13.92 19.15L13.74 21.85A10 10 0 0 1 10.26 21.85L10.08 19.15A7.4 7.4 0 0 1 8.30 18.41L6.26 20.19A10 10 0 0 1 3.81 17.74L5.59 15.70A7.4 7.4 0 0 1 4.85 13.92L2.15 13.74A10 10 0 0 1 2.15 10.26L4.85 10.08A7.4 7.4 0 0 1 5.59 8.30L3.81 6.26A10 10 0 0 1 6.26 3.81L8.30 5.59A7.4 7.4 0 0 1 10.08 4.85ZM15.4 12A3.4 3.4 0 1 0 8.6 12A3.4 3.4 0 1 0 15.4 12Z"/>'),
    ("Download", '<path d="M12 4v10M8 10l4 4 4-4"/><path d="M4 15v3a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-3"/>'),
    ("Package", '<path d="M4 8l8-4 8 4-8 4z"/><path d="M4 8v8l8 4 8-4V8"/><path d="M12 12v8"/>'),
    ("Picture", '<rect x="4" y="5" width="16" height="14" rx="1.5"/><circle cx="9" cy="10" r="1.6"/><path d="M5 17l5-5 4 4 2-2 4 4"/>'),
    ("Text", '<path d="M7 3.5h7l4.5 4.5v11a1.5 1.5 0 0 1-1.5 1.5H7A1.5 1.5 0 0 1 5.5 19V5A1.5 1.5 0 0 1 7 3.5z"/><path d="M9 12h6M9 15.5h6M9 8.5h3"/>'),
    ("Zip", '<rect x="5" y="3.5" width="14" height="17" rx="1.5"/><path d="M12 3.5v2M12 7v2M12 10.5v1.5"/><rect x="10" y="13" width="4" height="4" rx="1"/>'),
    ("App", '<rect x="4" y="4" width="16" height="16" rx="3"/><path d="M9 12l2 2 4-4"/>'),
    ("GameCard", '<path d="M6 3.5h9l3 3V20a.5.5 0 0 1-.5.5h-11A.5.5 0 0 1 6 20z"/><path d="M9 8h6v5H9z"/><path d="M9 16.5h6"/>'),
    ("Hex", '<path d="M8 4H6v16h2M16 4h2v16h-2"/><path d="M10.5 9.5v5M13.5 9.5v5"/>'),
    ("File", '<path d="M7 3.5h7l4.5 4.5v11a1.5 1.5 0 0 1-1.5 1.5H7A1.5 1.5 0 0 1 5.5 19V5A1.5 1.5 0 0 1 7 3.5z"/><path d="M14 3.5V8h4.5"/>'),
    ("Trash", '<path d="M5 7h14M10 4h4M7 7l1 12.5a1 1 0 0 0 1 .9h6a1 1 0 0 0 1-.9L17 7"/>'),
    ("Clock", '<circle cx="12" cy="12.5" r="8"/><path d="M12 8.5v4.5l3 1.8"/>'),
    ("Up", '<path d="M9 14l-4-4 4-4"/><path d="M5 10h9a5 5 0 0 1 5 5v3"/>'),
    ("Music", '<path d="M9 18V6l10-2v12"/><circle cx="6.5" cy="18" r="2.5"/><circle cx="16.5" cy="16" r="2.5"/>'),
    ("Video", '<rect x="3.5" y="6" width="12" height="12" rx="1.5"/><path d="M15.5 10.5l5-3v9l-5-3"/>'),
    ("Warning", '<path d="M12 4l9 16H3z"/><path d="M12 10v4M12 17v.5"/>'),
    ("Check", '<path d="M5 12.5l4.5 4.5L19 7.5"/>'),
    ("Chip", '<rect x="6" y="6" width="12" height="12" rx="1.5"/><path d="M9 3v3M15 3v3M9 18v3M15 18v3M3 9h3M3 15h3M18 9h3M18 15h3"/>'),
    ("Lock", '<rect x="5" y="11" width="14" height="9" rx="1.5"/><path d="M8 11V8a4 4 0 0 1 8 0v3"/>'),
    # Filled, so a favourite's star still reads as one beside a row's size.
    ("Star", '<path fill="#fff" d="M12 3.6L14.29 9.44L20.56 9.82L15.71 13.81L17.29 19.88L12 16.5L6.71 19.88L8.29 13.81L3.44 9.82L9.71 9.44Z"/>'),
]

SIZES = [64, 32, 24, 18]
ATLAS = 512
# Shapes the toolkit cuts rounded corners and rings from, in the last row:
# a disc, then rings whose stroke is this share of their radius.
SHAPE_ROW = 448
SHAPE_SIZE = 64
RING_RATIOS = [0.10, 0.16, 0.25, 0.36]
# A 64 px icon gets a slightly lighter stroke so it does not look bold.
STROKE = {64: 1.7, 32: 2.0, 24: 2.0, 18: 2.1}


def layout():
    cells = []  # (name, size, x, y)
    y = 0
    for size in SIZES:
        per_row = ATLAS // size
        for index, (name, _) in enumerate(ICONS):
            row, column = divmod(index, per_row)
            cells.append((name, size, column * size, y + row * size))
        rows = (len(ICONS) + per_row - 1) // per_row
        y += rows * size
    if y > SHAPE_ROW:
        sys.exit("The icons do not fit in the atlas.")
    return cells


def html(cells):
    bodies = dict(ICONS)
    parts = []
    for name, size, x, y in cells:
        parts.append(
            f'<svg style="position:absolute;left:{x}px;top:{y}px" '
            f'width="{size}" height="{size}" viewBox="0 0 24 24" fill="none" '
            f'stroke="#fff" stroke-width="{STROKE[size]}" '
            f'stroke-linecap="round" stroke-linejoin="round">'
            f'{bodies[name]}</svg>')
    half = SHAPE_SIZE / 2
    parts.append(
        f'<svg style="position:absolute;left:0;top:{SHAPE_ROW}px" '
        f'width="{SHAPE_SIZE}" height="{SHAPE_SIZE}">'
        f'<circle cx="{half}" cy="{half}" r="{half}" fill="#fff"/></svg>')
    for index, ratio in enumerate(RING_RATIOS):
        width = half * ratio
        parts.append(
            f'<svg style="position:absolute;left:{(index + 1) * SHAPE_SIZE}px;'
            f'top:{SHAPE_ROW}px" width="{SHAPE_SIZE}" height="{SHAPE_SIZE}">'
            f'<circle cx="{half}" cy="{half}" r="{half - width / 2}" '
            f'fill="none" stroke="#fff" stroke-width="{width}"/></svg>')
    return ('<!doctype html><html><head><style>html,body{margin:0;'
            'background:transparent}</style></head><body>'
            + "".join(parts) + '</body></html>')


def header(cells):
    lines = [
        "#pragma once",
        "",
        "// Generated by scripts/make-ui-icons.py from the icons in that "
        "script.",
        "// romfs/ui/icons.png holds every icon at 64, 32, 24 and 18 px.",
        "",
        "#include <cstdint>",
        "",
        "namespace ui {",
        "",
        "enum class Icon : uint8_t {",
    ]
    for name, _ in ICONS:
        lines.append(f"    {name},")
    lines += ["    None,", "};", "", f"constexpr int IconAtlasSize = {ATLAS};",
              f"constexpr int IconSizeCount = {len(SIZES)};",
              "constexpr int IconSizes[IconSizeCount] = {"
              + ", ".join(str(s) for s in SIZES) + "};", "",
              "struct IconCell {", "    uint16_t x;", "    uint16_t y;", "};",
              "",
              "// [size index][icon]",
              f"constexpr IconCell IconCells[IconSizeCount][{len(ICONS)}] = {{"]
    for size in SIZES:
        row = [c for c in cells if c[1] == size]
        lines.append("    {" + ", ".join(f"{{{x}, {y}}}" for _, _, x, y in row)
                     + "},")
    lines += ["};", "",
              "// A white disc and rings, SHAPE_SIZE across, for corners.",
              f"constexpr int ShapeRow = {SHAPE_ROW};",
              f"constexpr int ShapeSize = {SHAPE_SIZE};",
              f"constexpr int RingCount = {len(RING_RATIOS)};",
              "// Stroke width as a share of the radius, for ring n at x = "
              "(n + 1) * ShapeSize.",
              "constexpr float RingRatios[RingCount] = {"
              + ", ".join(f"{r:.2f}f" for r in RING_RATIOS) + "};",
              "", "} // namespace ui", ""]
    return "\n".join(lines)


def main():
    cells = layout()
    with tempfile.TemporaryDirectory() as work:
        page = os.path.join(work, "icons.html")
        with open(page, "w") as handle:
            handle.write(html(cells))
        out = os.path.join(ROOT, "romfs", "ui", "icons.png")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        subprocess.run([
            CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars",
            "--force-device-scale-factor=1",
            "--default-background-color=00000000",
            f"--window-size={ATLAS},{ATLAS}", f"--screenshot={out}",
            "file://" + page,
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(os.path.join(ROOT, "source", "app", "icon_atlas.hpp"), "w") as handle:
        handle.write(header(cells))
    print(f"{len(ICONS)} icons at {SIZES} px written.")


if __name__ == "__main__":
    main()
