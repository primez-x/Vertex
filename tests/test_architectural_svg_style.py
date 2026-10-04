"""Structural and palette invariants for the print-friendly symbol library."""
import importlib.util
import json
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets/symbols/architectural_v2"
NS = "{http://www.w3.org/2000/svg}"
spec = importlib.util.spec_from_file_location("restyle", ROOT / "scripts/restyle_architectural_svg_library.py")
restyle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(restyle)


class ArchitecturalStyleTests(unittest.TestCase):
    def test_palette_and_identity(self):
        index = json.loads((ASSETS / "index.json").read_text())
        self.assertEqual(index["count"], 342)
        self.assertEqual(len({(r["category"], r["id"]) for r in index["symbols"]}), 342)
        for row in index["symbols"]:
            with self.subTest(symbol=row["id"]):
                root = ET.parse(ASSETS / row["file"]).getroot()
                self.assertEqual(root.get("data-vertex-style"), restyle.VERSION)
                self.assertFalse(restyle.restyle(root))
                for e in root.iter():
                    if "stop-color" in e.attrib:
                        self.assertIn(e.get("stop-color"), ("#ffffff", "#f2f2f2", "#333333"))
                    if e.get("stroke", "none") != "none":
                        self.assertIn(e.get("stroke"), ("#111111", "#b8b8b8"))

    def test_overhead_cabinets_are_shallow_dashed_projections(self):
        for width in (600, 900):
            root = ET.parse(ASSETS / f"symbols/02_kitchen/wall-cabinet-{width}.svg").getroot()
            self.assertIn(f"Nominal footprint {width} by 350 millimetres", root.find(NS + "desc").text)
            footprint = root.find(".//" + NS + "rect")
            self.assertEqual(footprint.get("width"), str(width))
            self.assertEqual(footprint.get("height"), "350")
            self.assertIsNotNone(footprint.get("stroke-dasharray"))


if __name__ == "__main__":
    unittest.main()
