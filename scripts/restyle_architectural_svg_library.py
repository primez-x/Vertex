"""Apply the authorized print-friendly palette to the imported SVG library.

Only presentation attributes change; geometry, IDs and dimensions are preserved.
The version marker makes repeated runs safe. Semantic dark material (screens,
burners and openings) stays dark; pale highlight strokes stay subordinate.
"""
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets/symbols/architectural_v2/symbols"
NS = "{http://www.w3.org/2000/svg}"
VERSION = "white-outline-2"
ET.register_namespace("", NS[1:-1])


def brightness(color):
    if color == "white":
        return 255
    if not color.startswith("#"):
        return None
    digits = color[1:]
    if len(digits) == 3:
        digits = "".join(c * 2 for c in digits)
    return sum(int(digits[i:i + 2], 16) for i in (0, 2, 4)) / 3


def restyle(root):
    if root.get("data-vertex-style") == VERSION:
        return False
    previous_white_style = root.get("data-vertex-style") == "white-outline-1"
    span = max(map(float, root.get("viewBox").split()[2:]))
    description = root.find(NS + "desc")
    nominal_mm = description is not None and "Nominal footprint" in (description.text or "")
    for gradient in root.iter():
        if gradient.tag not in (NS + "linearGradient", NS + "radialGradient"):
            continue
        dark = gradient.get("id", "").endswith("--dark")
        stops = list(gradient)
        for i, stop in enumerate(stops):
            # The broad surface stays white; only its last edge has light shading.
            stop.set("stop-color", "#333333" if dark else
                     ("#f2f2f2" if i == len(stops) - 1 else "#ffffff"))
    for element in root.iter():
        fill = element.get("fill", "")
        level = brightness(fill)
        if level is not None and not previous_white_style:
            element.set("fill", "#333333" if level < 145 else
                        ("#f2f2f2" if level < 220 else "#ffffff"))
        stroke = element.get("stroke", "")
        level = brightness(stroke)
        if level is not None:
            highlight = stroke == "#b8b8b8" if previous_white_style else level > 225
            element.set("stroke", "#b8b8b8" if highlight else "#111111")
            width = float(element.get("stroke-width", "1"))
            divisor = 550 if highlight else (220 if fill and fill != "none" else 350)
            # Small fixtures must remain readable beside large furniture. Their
            # native millimetre coordinates need a useful physical line weight,
            # not an ever-thinner percentage of a small footprint.
            minimum = (2 if highlight else 10 if fill and fill != "none" else 7) if nominal_mm else \
                      span / (550 if highlight else 100 if fill and fill != "none" else 140)
            element.set("stroke-width", format(max(width, span / divisor, minimum), ".6g"))
    root.set("data-vertex-style", VERSION)
    return True


if __name__ == "__main__":
    changed = 0
    for path in sorted(ASSETS.rglob("*.svg")):
        root = ET.parse(path).getroot()
        if restyle(root):
            path.write_text(ET.tostring(root, encoding="unicode") + "\n", encoding="utf-8")
            changed += 1
    print(f"Restyled {changed} SVG documents")
