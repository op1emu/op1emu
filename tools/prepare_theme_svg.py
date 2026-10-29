#!/usr/bin/env python3
"""Add paint classes to the supplied artwork without rewriting its XML/filter data."""
import hashlib
from pathlib import Path
import re
import sys
from xml.etree import ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "gui/op1.svg"
TARGET = ROOT / "gui/op1-themeable.svg"
SOURCE_SHA256 = "8af5c1c4ec44c7601763c4c4d02f0a558378531dc998cc8428d7081da5407f29"


def prepare():
    source = SOURCE.read_bytes()
    if hashlib.sha256(source).hexdigest() != SOURCE_SHA256:
        raise SystemExit("Artwork changed; review the paint assignments before regenerating")
    elements = iter(ET.fromstring(source).iter())
    counts = {}

    def annotate(match):
        element = next(elements)
        tag = element.tag.rsplit("}", 1)[-1]
        role = None
        if element.get("fill") == "#A7A7A7":
            if tag == "rect":
                width = float(element.get("width"))
                role = "body" if width == 1200 else "keycap" if width < 40 else "panel"
            elif tag == "use":
                href = element.get("{http://www.w3.org/1999/xlink}href")
                rings = {f"#stroke{i}_1452_16035" for i in (8, 12, 16, 20)}
                role = "encoder-ring" if href in rings else "keycap"
            else:
                raise SystemExit(f"Unassigned grey paint: {tag}")
        text = match.group()
        if role:
            counts[role] = counts.get(role, 0) + 1
            text = text.replace(f"<{tag}", f'<{tag} class="op1-{role}"', 1)
        return text

    result = re.sub(r"<[A-Za-z][^>]*>", annotate, source.decode("utf-8"))
    assert next(elements, None) is None
    assert counts == {"body": 1, "panel": 60, "keycap": 54, "encoder-ring": 4}, counts
    if "--check" in sys.argv:
        if TARGET.read_text(encoding="utf-8") != result:
            raise SystemExit("Themeable artwork is stale; run tools/prepare_theme_svg.py")
    else:
        TARGET.write_text(result, encoding="utf-8")
    print(f"Prepared {TARGET.name}: {counts}")


if __name__ == "__main__":
    prepare()
