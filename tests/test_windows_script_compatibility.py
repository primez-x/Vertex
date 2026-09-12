"""Keep documented Windows PowerShell harnesses compatible with 5.1 and 7."""
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class WindowsScriptCompatibilityTests(unittest.TestCase):
    def test_process_harnesses_fallback_when_argument_list_is_unavailable(self):
        for name in ("test-desktop.ps1", "test-building-editor.ps1"):
            script = (ROOT / "scripts" / name).read_text(encoding="utf-8")
            self.assertIn("PSObject.Properties['ArgumentList']", script)
            self.assertIn("$StartInfo.Arguments", script)
            self.assertIn("-match '[\\s\"]'", script)


if __name__ == "__main__":
    unittest.main()
