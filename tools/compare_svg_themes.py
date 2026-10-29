#!/usr/bin/env python3
"""Render actual resvg palettes at 1x/2x and make a local review sheet (requires Pillow)."""
import argparse
import json
from pathlib import Path
import subprocess
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
THEMES = ("slate", "silver", "yellow", "green", "purple", "orange")


def font(size):
    for name in ("/System/Library/Fonts/Supplemental/Arial.ttf", "DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default(size=size)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, default=ROOT / "build/op1-svg-render")
    parser.add_argument("--output", type=Path, default=ROOT / "build/visual-comparisons/themes")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    sheet = Image.new("RGB", (2504, 1610), "#202020")
    draw = ImageDraw.Draw(sheet)
    draw.text((32, 22), "OP-1 / six SVG themes", font=font(34), fill="#eeeeee")
    draw.text((32, 67), "Reference palette study  /  Original SVG filters  /  Blank screens; no firmware loaded",
              font=font(20), fill="#aaaaaa")
    for index, theme in enumerate(THEMES):
        palette = json.loads((ROOT / f"gui/themes/{theme}.json").read_text())
        for scale in (1, 2):
            subprocess.run([str(args.renderer.resolve()), "gui/op1-themeable.svg", str(1204 * scale),
                            str(434 * scale), str(output / f"{theme}-{scale}x.png"), theme],
                           cwd=ROOT, check=True)
        x, y = 32 + (index % 2) * 1236, 126 + (index // 2) * 490
        color = palette["colors"]["body"]
        draw.rounded_rectangle((x, y, x + 23, y + 23), radius=5, fill=color)
        draw.text((x + 36, y - 3), f'{palette["name"]}  /  {color}', font=font(25), fill="#dddddd")
        with Image.open(output / f"{theme}-2x.png") as image:
            image = image.resize((1204, 434), Image.Resampling.LANCZOS)
            sheet.paste(image, (x, y + 38), image)
    sheet.save(output / "six-themes-comparison.png")
    print(output / "six-themes-comparison.png")


if __name__ == "__main__":
    main()
