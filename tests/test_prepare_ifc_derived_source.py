"""Offline source-derivation fixtures; no Git, compiler or upstream execution."""
import hashlib
import importlib.util
import json
import os
import pathlib
import tempfile
import unittest
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "prepare_ifc_derived_source", ROOT / "scripts/prepare_ifc_derived_source.py")
derived = importlib.util.module_from_spec(spec)
spec.loader.exec_module(derived)


class IfcDerivedSourceTests(unittest.TestCase):
    def test_executable_suffix_regular_file_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            for suffix in (".bat", ".cmd", ".exe", ".py"):
                path = pathlib.Path(directory) / ("fixture" + suffix)
                path.write_bytes(b"input bytes only; never execute\n")
                self.assertEqual(derived.read_bounded(path, 1024), path.read_bytes())

    def fixture(self, parent):
        workspace = parent / "workspace"
        source = workspace / ".deps/ifc-src"
        source.mkdir(parents=True)
        lock = json.loads((ROOT / "third_party/ifc-source-lock.json").read_bytes())
        lock_path = workspace / "third_party/ifc-source-lock.json"
        lock_path.parent.mkdir(parents=True)
        lock_path.write_bytes(json.dumps(lock).encode())
        patch_path = workspace / derived.PATCH_PATH
        patch_path.parent.mkdir(parents=True)
        patch_path.write_bytes((ROOT / derived.PATCH_PATH).read_bytes())
        files = {
            ".gitmodules": b"fixture gitlinks\n",
            "cmake/CMakeLists.txt": b"fixture cmake\n",
            "cmake/empty.txt": b"",
            derived.MODIFIED_PATH: b"// fixture original typemaps\r\n",
        }
        for submodule in lock["submodules"]:
            files[submodule["path"] + "/fixture.txt"] = b"selected child\n"
        records = []
        for relative, data in files.items():
            target = source / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            records.append({"path": relative, "bytes": len(data),
                            "sha256": hashlib.sha256(data).hexdigest()})
        (source / ".git").mkdir()
        (source / ".git/metadata").write_bytes(b"not copied")
        for submodule in lock["submodules"]:
            (source / submodule["path"] / ".git").write_bytes(b"gitdir: fixture\n")
        preparation = {
            "schema_version": 1,
            "lock_sha256": hashlib.sha256(lock_path.read_bytes()).hexdigest(),
            "build_qualified": False, "source_closure_qualified": False,
            "source": {**lock["source"], "files": sorted(records, key=lambda x: x["path"])},
            "submodules": [{k: s[k] for k in ("path", "repository", "revision")}
                           for s in lock["submodules"]],
            "swig": {"version": lock["swig"]["version"],
                     "path": ".deps/ifc-tools/swigwin-4.3.1",
                     "archive_sha512": lock["swig"]["sha512"], "files": []},
        }
        prep_path = workspace / ".deps/ifc-source-preparation.json"
        prep_path.write_bytes(json.dumps(preparation).encode())
        external = parent / "candidate"
        external.mkdir()
        return workspace, external / "source", external / "source-derivation.json"

    def read_preparation(self, workspace):
        return json.loads((workspace / ".deps/ifc-source-preparation.json").read_bytes())

    def write_preparation(self, workspace, value):
        (workspace / ".deps/ifc-source-preparation.json").write_bytes(json.dumps(value).encode())

    def test_exact_single_file_derivation_and_verifier(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace, output, manifest = self.fixture(pathlib.Path(directory))
            source = workspace / ".deps/ifc-src"
            original = (source / derived.MODIFIED_PATH).read_bytes()
            patch_bytes = (workspace / derived.PATCH_PATH).read_bytes()
            result = derived.derive(workspace, output, manifest)
            self.assertEqual((output / derived.MODIFIED_PATH).read_bytes(), original + patch_bytes)
            self.assertEqual((source / derived.MODIFIED_PATH).read_bytes(), original)
            self.assertEqual((output / "cmake/CMakeLists.txt").read_bytes(), b"fixture cmake\n")
            self.assertFalse((output / ".git").exists())
            for item in self.read_preparation(workspace)["submodules"]:
                self.assertFalse((output / item["path"] / ".git").exists())
            self.assertFalse(result["build_qualified"])
            self.assertFalse(result["source_closure_qualified"])
            self.assertFalse(result["product_runtime_replaced"])
            original_records = self.read_preparation(workspace)["source"]["files"]
            self.assertEqual(len(result["files"]), len(original_records))
            changed = [a["path"] for a, b in zip(result["files"], original_records) if a != b]
            self.assertEqual(changed, ["src/ifcwrap/utils/typemaps_out.i"])
            self.assertEqual(result["modified_file"]["original_sha256"], hashlib.sha256(original).hexdigest())
            self.assertEqual(result["modified_file"]["derived_sha256"], hashlib.sha256(original + patch_bytes).hexdigest())
            self.assertEqual(result, derived.verify_derivation(workspace, output, manifest))
            self.assertEqual(json.loads(manifest.read_bytes()), result)

    def test_input_content_and_bindings_fail_before_copy(self):
        for fault in ("source", "extra", "patch", "lock_binding", "source_revision",
                      "submodule_revision", "qualified", "swig_binding"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as directory:
                workspace, output, manifest = self.fixture(pathlib.Path(directory))
                preparation = self.read_preparation(workspace)
                if fault == "source":
                    (workspace / ".deps/ifc-src/cmake/CMakeLists.txt").write_bytes(b"altered")
                elif fault == "extra":
                    (workspace / ".deps/ifc-src/foreign.txt").write_bytes(b"foreign")
                elif fault == "patch":
                    (workspace / derived.PATCH_PATH).write_bytes(b"unreviewed patch")
                elif fault == "lock_binding":
                    preparation["lock_sha256"] = "0" * 64
                elif fault == "source_revision":
                    preparation["source"]["revision"] = "0" * 40
                elif fault == "submodule_revision":
                    preparation["submodules"][0]["revision"] = "0" * 40
                elif fault == "qualified":
                    preparation["build_qualified"] = True
                else:
                    preparation["swig"]["archive_sha512"] = "0" * 128
                self.write_preparation(workspace, preparation)
                with self.assertRaises(ValueError):
                    derived.derive(workspace, output, manifest)
                self.assertFalse(output.exists())
                self.assertFalse(manifest.exists())

    def test_table_rejects_unsafe_paths_collisions_and_invalid_sizes(self):
        bad_paths = ("../escape", "/absolute", "x\\y", "CON.txt", "x:stream",
                     "trailing. ", "src/.git/config", "x//y", "a/../b")
        for name in bad_paths:
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                workspace, output, manifest = self.fixture(pathlib.Path(directory))
                prep = self.read_preparation(workspace)
                prep["source"]["files"][0]["path"] = name
                self.write_preparation(workspace, prep)
                with self.assertRaises(ValueError):
                    derived.derive(workspace, output, manifest)
                self.assertFalse(output.exists())
        for extra in ({"path": "cmake/CMakeLists.txt", "bytes": 0, "sha256": "0" * 64},
                      {"path": "CMAKE/another.txt", "bytes": 0, "sha256": "0" * 64},
                      {"path": "cmake/CMakeLists.txt/child", "bytes": 0, "sha256": "0" * 64},
                      {"path": "bad", "bytes": -1, "sha256": "0" * 64},
                      {"path": "bad", "bytes": True, "sha256": "0" * 64},
                      {"path": "bad", "bytes": 0, "sha256": "wrong"}):
            with self.subTest(extra=extra), tempfile.TemporaryDirectory() as directory:
                workspace, output, manifest = self.fixture(pathlib.Path(directory))
                prep = self.read_preparation(workspace)
                prep["source"]["files"].append(extra)
                prep["source"]["files"].sort(key=lambda x: x["path"])
                self.write_preparation(workspace, prep)
                with self.assertRaises(ValueError):
                    derived.derive(workspace, output, manifest)
                self.assertFalse(output.exists())

    def test_foreign_output_overlap_and_parent_rules_preserve_inputs(self):
        for fault in ("output", "manifest", "workspace_overlap", "source_overlap",
                      "manifest_in_output", "missing_parent", "same_path"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as directory:
                workspace, output, manifest = self.fixture(pathlib.Path(directory))
                if fault == "output":
                    output.mkdir()
                    (output / "keep.txt").write_bytes(b"keep")
                elif fault == "manifest":
                    manifest.write_bytes(b"keep")
                elif fault == "workspace_overlap":
                    output = workspace / "derived"
                elif fault == "source_overlap":
                    output = workspace / ".deps/ifc-src/derived"
                elif fault == "manifest_in_output":
                    manifest = output / "manifest.json"
                elif fault == "missing_parent":
                    output = output.parent / "missing/source"
                else:
                    manifest = output
                with self.assertRaises(ValueError):
                    derived.derive(workspace, output, manifest)
                self.assertEqual((workspace / ".deps/ifc-src/cmake/CMakeLists.txt").read_bytes(), b"fixture cmake\n")
                if fault == "output":
                    self.assertEqual((output / "keep.txt").read_bytes(), b"keep")
                elif fault == "manifest":
                    self.assertEqual(manifest.read_bytes(), b"keep")

    def test_resource_bounds_and_duplicate_json_keys(self):
        for limit in ("MAX_FILES", "MAX_TOTAL_BYTES", "MAX_FILE_BYTES", "MAX_JSON_BYTES"):
            with self.subTest(limit=limit), tempfile.TemporaryDirectory() as directory:
                workspace, output, manifest = self.fixture(pathlib.Path(directory))
                with patch.object(derived, limit, 1), self.assertRaises(ValueError):
                    derived.derive(workspace, output, manifest)
                self.assertFalse(output.exists())
        with tempfile.TemporaryDirectory() as directory:
            workspace, output, manifest = self.fixture(pathlib.Path(directory))
            (workspace / ".deps/ifc-source-preparation.json").write_bytes(b'{"schema_version":1,"schema_version":1}')
            with self.assertRaises(ValueError):
                derived.derive(workspace, output, manifest)

    def test_final_copy_and_input_recheck_preserve_failed_output(self):
        for fault in ("destination", "source", "preparation", "patch"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as directory:
                workspace, output, manifest = self.fixture(pathlib.Path(directory))
                original_copy = derived.copy_checked_file
                changed = False

                def altered_copy(*args, **kwargs):
                    nonlocal changed
                    original_copy(*args, **kwargs)
                    if changed:
                        return
                    changed = True
                    if fault == "destination":
                        (output / "foreign.txt").write_bytes(b"injected")
                    elif fault == "source":
                        (workspace / ".deps/ifc-src/cmake/CMakeLists.txt").write_bytes(b"changed during copy")
                    elif fault == "preparation":
                        path = workspace / ".deps/ifc-source-preparation.json"
                        path.write_bytes(path.read_bytes() + b" ")
                    else:
                        (workspace / derived.PATCH_PATH).write_bytes(b"changed during copy")
                with patch.object(derived, "copy_checked_file", side_effect=altered_copy), self.assertRaises(ValueError):
                    derived.derive(workspace, output, manifest)
                self.assertTrue(output.is_dir())
                self.assertFalse(manifest.exists())

    def test_verifier_rejects_changed_output_and_manifest(self):
        for fault in ("output", "extra", "git", "manifest", "binding", "qualified"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as directory:
                workspace, output, manifest = self.fixture(pathlib.Path(directory))
                derived.derive(workspace, output, manifest)
                data = json.loads(manifest.read_bytes())
                if fault == "output":
                    (output / derived.MODIFIED_PATH).write_bytes(b"changed")
                elif fault == "extra":
                    (output / "extra").write_bytes(b"extra")
                elif fault == "git":
                    (output / ".git").mkdir()
                elif fault == "manifest":
                    data["modified_file"]["derived_sha256"] = "0" * 64
                elif fault == "binding":
                    data["inputs"]["patch"]["sha256"] = "0" * 64
                else:
                    data["source_closure_qualified"] = True
                manifest.write_bytes(json.dumps(data).encode())
                with self.assertRaises(ValueError):
                    derived.verify_derivation(workspace, output, manifest)

    def test_verifier_requires_strict_schema_flags_and_file_sizes(self):
        for fault in ("boolean_schema", "build_qualified", "source_closure_qualified",
                      "product_runtime_replaced", "boolean_file_bytes"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as directory:
                workspace, output, manifest = self.fixture(pathlib.Path(directory))
                derived.derive(workspace, output, manifest)
                data = json.loads(manifest.read_bytes())
                if fault == "boolean_schema":
                    data["schema_version"] = True
                elif fault == "boolean_file_bytes":
                    next(record for record in data["files"] if record["path"] == "cmake/empty.txt")["bytes"] = False
                else:
                    data[fault] = 0
                manifest.write_bytes(json.dumps(data).encode())
                with self.assertRaises(ValueError):
                    derived.verify_derivation(workspace, output, manifest)

    def test_missing_inputs_report_value_error_before_copy(self):
        for relative in (derived.LOCK_PATH, derived.PREPARATION_PATH, derived.PATCH_PATH):
            with self.subTest(relative=relative), tempfile.TemporaryDirectory() as directory:
                workspace, output, manifest = self.fixture(pathlib.Path(directory))
                (workspace / relative).unlink()
                with self.assertRaisesRegex(ValueError, "Cannot read input"):
                    derived.derive(workspace, output, manifest)
                self.assertFalse(output.exists())
                self.assertFalse(manifest.exists())

    def test_malformed_submodule_keys_report_value_error_before_copy(self):
        for fault in ("missing_keys", "non_string_path", "non_string_repository", "non_string_revision"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as directory:
                workspace, output, manifest = self.fixture(pathlib.Path(directory))
                lock_path = workspace / derived.LOCK_PATH
                lock = json.loads(lock_path.read_bytes())
                item = lock["submodules"][0]
                if fault == "missing_keys":
                    item.pop("path")
                    item.pop("repository")
                elif fault == "non_string_path":
                    item["path"] = []
                elif fault == "non_string_repository":
                    item["repository"] = None
                else:
                    item["revision"] = 42
                lock_path.write_bytes(json.dumps(lock).encode())
                preparation = self.read_preparation(workspace)
                preparation["lock_sha256"] = hashlib.sha256(lock_path.read_bytes()).hexdigest()
                self.write_preparation(workspace, preparation)
                with self.assertRaisesRegex(ValueError, "Invalid locked submodule identity"):
                    derived.derive(workspace, output, manifest)
                self.assertFalse(output.exists())
                self.assertFalse(manifest.exists())

    def test_links_and_simulated_windows_reparse_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace, output, manifest = self.fixture(pathlib.Path(directory))
            real = derived.is_link
            target = workspace / ".deps/ifc-src/cmake"
            with patch.object(derived, "is_link", side_effect=lambda p: pathlib.Path(p) == target or real(p)), self.assertRaises(ValueError):
                derived.derive(workspace, output, manifest)
            self.assertFalse(output.exists())
        with tempfile.TemporaryDirectory() as directory:
            workspace, output, manifest = self.fixture(pathlib.Path(directory))
            link = workspace / ".deps/ifc-src/linked.txt"
            try:
                os.symlink(workspace / ".deps/ifc-src/cmake/CMakeLists.txt", link)
            except (OSError, NotImplementedError):
                self.skipTest("symlink creation unavailable")
            with self.assertRaises(ValueError):
                derived.derive(workspace, output, manifest)
            self.assertTrue(link.is_symlink())
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
