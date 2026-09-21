"""Focused integrity/failure-path checks for the offline SVG metadata generator."""
import json
from pathlib import Path
import shutil
import tempfile
import unittest

from generate_architectural_svg_catalog import ASSETS, generate


class ArchitecturalSvgCatalogTests(unittest.TestCase):
    def test_checked_in_metadata_matches_all_assets(self):
        self.assertEqual(generate(ASSETS), (ASSETS / "catalog_data.inc").read_text(encoding="utf-8"))

    def test_missing_duplicate_and_unsafe_index_entries_are_rejected(self):
        for mutation in ("missing", "duplicate", "unsafe", "zero_viewbox"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temp:
                directory = Path(temp) / "library"
                shutil.copytree(ASSETS, directory)
                index_path = directory / "index.json"
                index = json.loads(index_path.read_text(encoding="utf-8"))
                if mutation == "missing":
                    index["symbols"].pop()
                elif mutation == "duplicate":
                    index["symbols"][1] = index["symbols"][0]
                elif mutation == "unsafe":
                    index["symbols"][0]["file"] = "../escape.svg"
                else:
                    (directory / index["symbols"][0]["file"]).write_text(
                        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 0 100"/>')
                index_path.write_text(json.dumps(index), encoding="utf-8")
                with self.assertRaises(ValueError):
                    generate(directory)


if __name__ == "__main__":
    unittest.main()
