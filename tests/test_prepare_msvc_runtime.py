"""Synthetic PE fixtures test staging policy, not runtime compatibility."""

import importlib.util
import json
from pathlib import Path
import stat
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "prepare_msvc_runtime.py"
SPEC = importlib.util.spec_from_file_location("prepare_msvc_runtime", SCRIPT)
runtime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runtime)


def synthetic_dll(machine=0x8664, characteristics=0x2022):
    data = bytearray(512)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 128)
    data[128:132] = b"PE\0\0"
    struct.pack_into("<HH", data, 132, machine, 1)
    struct.pack_into("<HH", data, 148, 112, characteristics)
    struct.pack_into("<H", data, 152, 0x20B)
    return bytes(data)


class PrepareMsvcRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.version = "14.44.35211.0"
        self.source = self.root / "SDK/VC/Redist/MSVC/14.44.35112/x64/Microsoft.VC143.CRT"
        self.source.mkdir(parents=True)
        for name in runtime.RUNTIME_NAMES:
            (self.source / name).write_bytes(synthetic_dll())
        self.notice = self.root / "ThirdPartyNotices.txt"
        self.notice.write_text("Synthetic notice; not licensing clearance.\n", encoding="utf-8")
        self.output = self.root / ".deps/msvc-runtime" / self.version

    def prepare(self, **overrides):
        arguments = dict(crt_dir=self.source, notice_files=[self.notice], output=self.output,
                         version=self.version, workspace_root=self.root)
        arguments.update(overrides)
        return runtime.prepare(**arguments)

    def test_exact_allowlist_portable_manifest_and_identical_rerun(self):
        (self.source / "concrt140.dll").write_bytes(synthetic_dll())
        manifest = self.prepare()
        expected = {f"bin/{name}" for name in runtime.RUNTIME_NAMES}
        self.assertEqual({row["path"] for row in manifest["files"]}, expected)
        self.assertEqual(set(path.name for path in (self.output / "bin").iterdir()), set(runtime.RUNTIME_NAMES))
        self.assertFalse(manifest["licensing_clearance"])
        self.assertEqual(manifest["provenance"]["version_verification"], "operator-declared")
        text = (self.output / "manifest.json").read_text(encoding="utf-8")
        self.assertNotIn(str(self.root), text)
        for record in manifest["files"] + manifest["notices"]:
            data = (self.output / record["path"]).read_bytes()
            self.assertEqual(record["bytes"], len(data))
            self.assertEqual(record["sha256"], runtime.hashlib.sha256(data).hexdigest())
            self.assertFalse(Path(record["path"]).is_absolute())
        before = {path: path.stat().st_mtime_ns for path in self.output.rglob("*")}
        self.assertEqual(self.prepare(), manifest)
        self.assertEqual(before, {path: path.stat().st_mtime_ns for path in self.output.rglob("*")})
        self.assertEqual(json.loads(text), manifest)

    def test_missing_and_changed_sources_rejected_even_when_output_exists(self):
        self.prepare()
        dll = self.source / runtime.RUNTIME_NAMES[0]
        original = dll.read_bytes()
        dll.unlink()
        with self.assertRaisesRegex(ValueError, "Missing"):
            self.prepare()
        dll.write_bytes(original + b"changed")
        with self.assertRaisesRegex(ValueError, "differs"):
            self.prepare()
        self.assertEqual((self.output / "bin" / dll.name).read_bytes(), original)

    def test_invalid_pe_architecture_and_non_dll_rejected_before_output(self):
        bad_inputs = [b"not a DLL", synthetic_dll(machine=0x14C), synthetic_dll(characteristics=0x22),
                      synthetic_dll()[:200]]
        for data in bad_inputs:
            with self.subTest(size=len(data)):
                (self.source / runtime.RUNTIME_NAMES[0]).write_bytes(data)
                with self.assertRaises(ValueError):
                    self.prepare()
                self.assertFalse(self.output.exists())

    def test_existing_extra_or_modified_output_is_never_overwritten(self):
        self.prepare()
        extra = self.output / "extra.txt"
        extra.write_text("keep", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "differs"):
            self.prepare()
        self.assertEqual(extra.read_text(encoding="utf-8"), "keep")
        extra.unlink()
        binary = self.output / "bin" / runtime.RUNTIME_NAMES[0]
        binary.write_bytes(b"changed output")
        with self.assertRaisesRegex(ValueError, "differs"):
            self.prepare()
        self.assertEqual(binary.read_bytes(), b"changed output")

    def test_restricted_destination_and_version(self):
        for destination in (self.root / "outside", self.root / ".deps/msvc-runtime/wrong",
                            self.root / ".deps/msvc-runtime" / self.version / "nested"):
            with self.subTest(destination=destination):
                with self.assertRaisesRegex(ValueError, "Output"):
                    self.prepare(output=destination)
        with self.assertRaisesRegex(ValueError, "Version"):
            self.prepare(version="../escape")
        self.assertFalse(self.output.exists())

    def test_forbidden_and_non_sdk_sources(self):
        for component in ("System32", "SysWOW64", "SystemWOW64", "debug_nonredist"):
            with self.subTest(component=component):
                with self.assertRaisesRegex(ValueError, "System and debug"):
                    self.prepare(crt_dir=self.root / component / "x64/Microsoft.VC143.CRT")
        with self.assertRaisesRegex(ValueError, "SDK Redist"):
            self.prepare(crt_dir=self.root)

    def test_notices_required_existing_and_unique_case_insensitively(self):
        alternate = self.root / "other/thirdpartynotices.TXT"
        alternate.parent.mkdir()
        alternate.write_text("other", encoding="utf-8")
        for notices in ([], [self.notice, alternate], [self.root / "missing.txt"]):
            with self.subTest(notices=notices):
                with self.assertRaises(ValueError):
                    self.prepare(notice_files=notices)
        self.notice.write_bytes(b"")
        with self.assertRaisesRegex(ValueError, "empty"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_reparse_sources_and_destination_ancestors_rejected(self):
        original_lstat = Path.lstat
        for blocked in (self.source / runtime.RUNTIME_NAMES[0], self.output.parent):
            def fake_lstat(path, *args, **kwargs):
                if path == blocked:
                    return SimpleNamespace(st_mode=stat.S_IFREG, st_file_attributes=0x400)
                return original_lstat(path, *args, **kwargs)
            with self.subTest(blocked=blocked), mock.patch.object(Path, "lstat", fake_lstat):
                with self.assertRaisesRegex(ValueError, "Reparse"):
                    self.prepare()
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
