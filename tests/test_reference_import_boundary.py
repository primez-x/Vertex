"""Keep both desktop source-file decode entry points behind the broker."""
from pathlib import Path
import unittest


class ReferenceImportBoundary(unittest.TestCase):
    def test_source_decoders_are_not_in_desktop_callers(self):
        source = (Path(__file__).resolve().parents[1] / "src/desktop/main_window.cpp").read_text(encoding="utf-8")
        for start, end in (("    QString importReferenceImage(", "    bool calibrateReference("),
                           ("    void showReferenceImport()", "    void showReferenceCalibration()")):
            body = source[source.index(start):source.index(end, source.index(start))]
            self.assertIn("decodeReferenceFile", body)
            self.assertNotIn("QPdfDocument", body)
            self.assertNotIn("QImage::fromData", body)


if __name__ == "__main__":
    unittest.main()
