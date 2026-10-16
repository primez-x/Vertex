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


def tree_snapshot(root):
    return {path.relative_to(root).as_posix(): path.read_bytes() for path in root.rglob("*") if path.is_file()}


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
        self.assertEqual(set(runtime.RUNTIME_NAMES), {
            "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll",
            "vcruntime140.dll", "vcruntime140_1.dll"})
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

    def test_each_missing_or_changed_source_rejected_even_when_output_exists(self):
        self.prepare()
        for name in runtime.RUNTIME_NAMES:
            with self.subTest(name=name):
                dll = self.source / name
                original = dll.read_bytes()
                dll.unlink()
                with self.assertRaisesRegex(ValueError, f"Missing redistributable: {name}"):
                    self.prepare()
                dll.write_bytes(original + b"changed")
                with self.assertRaisesRegex(ValueError, "differs"):
                    self.prepare()
                self.assertEqual((self.output / "bin" / name).read_bytes(), original)
                dll.write_bytes(original)

    def test_invalid_pe_architecture_and_non_dll_rejected_before_output(self):
        bad_inputs = [b"not a DLL", synthetic_dll(machine=0x14C), synthetic_dll(characteristics=0x22),
                      synthetic_dll()[:200]]
        for name in runtime.RUNTIME_NAMES:
            for data in bad_inputs:
                with self.subTest(name=name, size=len(data)):
                    (self.source / name).write_bytes(data)
                    with self.assertRaises(ValueError):
                        self.prepare()
                    self.assertFalse(self.output.exists())
                    (self.source / name).write_bytes(synthetic_dll())

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
        blocked_paths = [self.source / name for name in runtime.RUNTIME_NAMES] + [self.output.parent]
        for blocked in blocked_paths:
            def fake_lstat(path, *args, **kwargs):
                if path == blocked:
                    return SimpleNamespace(st_mode=stat.S_IFREG, st_file_attributes=0x400)
                return original_lstat(path, *args, **kwargs)
            with self.subTest(blocked=blocked), mock.patch.object(Path, "lstat", fake_lstat):
                with self.assertRaisesRegex(ValueError, "Reparse"):
                    self.prepare()
        self.assertFalse(self.output.exists())

    def test_fresh_named_complete_sibling_stages_all_members(self):
        complete = self.root / ".deps/msvc-runtime" / f"{self.version}-complete"
        manifest = self.prepare(output=complete)
        self.assertEqual(manifest["version"], self.version)
        self.assertEqual({row["path"] for row in manifest["files"]},
                         {f"bin/{name}" for name in runtime.RUNTIME_NAMES})
        self.assertEqual(set(path.name for path in (complete / "bin").iterdir()), set(runtime.RUNTIME_NAMES))
        self.assertFalse(self.output.exists())
        self.assertEqual(self.prepare(output=complete), manifest)

    def test_legacy_five_dll_cohort_preserved_when_complete_sibling_is_staged(self):
        legacy_names = tuple(name for name in runtime.RUNTIME_NAMES if name != "msvcp140_atomic_wait.dll")
        with mock.patch.object(runtime, "RUNTIME_NAMES", legacy_names):
            legacy_manifest = self.prepare()
        self.assertEqual(len(legacy_manifest["files"]), 5)
        legacy_tree = tree_snapshot(self.output)
        self.prepare(output=self.root / ".deps/msvc-runtime" / f"{self.version}-complete")
        self.assertEqual(tree_snapshot(self.output), legacy_tree)
        with self.assertRaisesRegex(ValueError, "differs"):
            self.prepare()
        self.assertEqual(tree_snapshot(self.output), legacy_tree)

    def test_output_name_limited_to_version_or_strict_cohort_suffix(self):
        deps = self.root / ".deps/msvc-runtime"
        # Reject before staging any sibling: Windows paths are case-insensitive, so
        # a created "-complete" directory would make "-COMPLETE" exist for the wrong reason.
        rejected = [f"{self.version}-", f"{self.version}-{'a' * 41}", f"{self.version}-COMPLETE",
                    f"{self.version}-complete_x", f"{self.version}-complete.old", f"{self.version}-complete x",
                    f"{self.version}--complete", f"{self.version}-complete-", f"{self.version}x",
                    "14.44.35211.1-complete"]
        for name in rejected:
            with self.subTest(name=name):
                with self.assertRaisesRegex(ValueError, "Output"):
                    self.prepare(output=deps / name)
                self.assertFalse((deps / name).exists())
        accepted = [f"{self.version}-{'a' * 40}", f"{self.version}-complete", f"{self.version}-a1-b2"]
        for name in accepted:
            with self.subTest(name=name):
                self.prepare(output=deps / name)
                self.assertTrue((deps / name / "manifest.json").is_file())
        escapes = [self.root / ".deps" / f"{self.version}-complete",
                   self.root / ".deps/msvc-runtime-other" / f"{self.version}-complete",
                   deps / f"{self.version}-complete" / "nested",
                   deps / ".." / f"{self.version}-complete"]
        for destination in escapes:
            with self.subTest(destination=destination):
                with self.assertRaisesRegex(ValueError, "Output"):
                    self.prepare(output=destination)


if __name__ == "__main__":
    unittest.main()
