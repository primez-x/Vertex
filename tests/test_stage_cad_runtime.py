"""Transactional runtime staging uses only bounded inert fixture bytes."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/stage_cad_runtime.py"


class SimulatedCrash(BaseException):
    pass


def record(name, data):
    return {"path": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


@unittest.skipUnless(os.name == "nt", "Publication requires Windows handle renames")
class StageTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SCRIPT.is_file(), "The exact transactional stage helper is missing")
        spec = importlib.util.spec_from_file_location("runtime_stage", SCRIPT)
        self.stage = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.stage)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "baseline"
        self.source.mkdir()
        self.adapter = self.root / "cad_library_adapter.py"
        self.adapter.write_bytes(b"# inert adapter fixture\n")
        self.output = self.root / "build/cad-runtime"
        self.output.parent.mkdir()
        self.files = {"python.exe": b"inert interpreter", "Lib/site-packages/module.py": b"inert library"}
        for name, data in self.files.items():
            target = self.source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        self.manifest = {"schema_version": 1, "python_version": "3.13.15",
                         "lock_sha256": "a" * 64, "library_versions": {"fixture": "1"},
                         "qualification": "incomplete", "production_worker_integrated": False,
                         "files": sorted((record(n, d) for n, d in self.files.items()), key=lambda r: r["path"])}
        self.save_manifest()

    def save_manifest(self, name="runtime-manifest.json"):
        (self.source / name).write_text(json.dumps(self.manifest), encoding="utf-8")

    def run_stage(self):
        return self.stage.stage(self.source, self.adapter, self.output)

    def test_exact_publication_and_unchanged_idempotence(self):
        first = self.run_stage()
        self.assertEqual(first["status"], "published")
        before = (self.output / self.stage.MANIFEST).read_bytes()
        self.assertEqual(self.run_stage()["status"], "unchanged")
        self.assertEqual((self.output / self.stage.MANIFEST).read_bytes(), before)
        names = {p.relative_to(self.output).as_posix() for p in self.output.rglob("*") if p.is_file()}
        self.assertEqual(names, set(self.files) | {"runtime-manifest.json", "cad_library_adapter.py", self.stage.MANIFEST})
        self.assertEqual((self.output / "cad_library_adapter.py").read_bytes(), self.adapter.read_bytes())
        self.assertFalse(first["runtime_qualified"])
        self.assertFalse(first["production_worker_integrated"])

    def test_replacements_rotate_one_exact_rollback(self):
        self.run_stage()
        old = (self.output / self.stage.MANIFEST).read_bytes()
        self.adapter.write_bytes(b"# next adapter\n")
        self.assertEqual(self.run_stage()["status"], "replaced")
        rollback = self.output.with_name(self.output.name + ".rollback")
        self.assertEqual((rollback / self.stage.MANIFEST).read_bytes(), old)
        self.assertEqual((rollback / "cad_library_adapter.py").read_bytes(), b"# inert adapter fixture\n")
        self.assertEqual(self.run_stage()["status"], "unchanged")
        current = (self.output / self.stage.MANIFEST).read_bytes()
        self.adapter.write_bytes(b"# third adapter\n")
        self.assertEqual(self.run_stage()["status"], "replaced")
        self.assertEqual((rollback / self.stage.MANIFEST).read_bytes(), current)
        self.assertEqual((self.output / "cad_library_adapter.py").read_bytes(), b"# third adapter\n")
        self.assertEqual(list(self.output.parent.glob(".cad-retired-*")), [])

    def test_legacy_output_requires_migration_without_mutation(self):
        self.output.mkdir()
        (self.output / "legacy.py").write_bytes(b"preserve")
        with self.assertRaisesRegex(ValueError, "migration-required"):
            self.run_stage()
        self.assertEqual((self.output / "legacy.py").read_bytes(), b"preserve")

    def test_unknown_rollback_is_preserved(self):
        self.run_stage()
        self.output.with_name(self.output.name + ".rollback").mkdir()
        self.adapter.write_bytes(b"next")
        with self.assertRaisesRegex(ValueError, "rollback"):
            self.run_stage()
        self.assertEqual((self.output / "cad_library_adapter.py").read_bytes(), b"# inert adapter fixture\n")

    def test_missing_output_with_rollback_requires_explicit_recovery(self):
        rollback = self.output.with_name(self.output.name + ".rollback")
        rollback.mkdir()
        (rollback / "foreign.txt").write_bytes(b"preserve")
        with self.assertRaisesRegex(ValueError, "recovery"):
            self.run_stage()
        self.assertEqual((rollback / "foreign.txt").read_bytes(), b"preserve")
        self.assertFalse(self.output.exists())

    def test_corrupt_owned_manifest_and_unlisted_output_file_are_preserved(self):
        self.run_stage()
        marker = self.output / self.stage.MANIFEST
        original = marker.read_bytes()
        value = json.loads(original)
        value["schema_version"] = True
        marker.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaises(ValueError):
            self.run_stage()
        self.assertIs(json.loads(marker.read_bytes())["schema_version"], True)
        marker.write_bytes(original)
        (self.output / "unknown.txt").write_bytes(b"unknown")
        with self.assertRaises(ValueError):
            self.run_stage()
        self.assertEqual((self.output / "unknown.txt").read_bytes(), b"unknown")

    def test_modified_owned_output_is_preserved(self):
        self.run_stage()
        (self.output / "python.exe").write_bytes(b"changed")
        with self.assertRaises(ValueError):
            self.run_stage()
        self.assertEqual((self.output / "python.exe").read_bytes(), b"changed")

    def test_controlled_manifest_retained_without_qualification(self):
        (self.source / "runtime-manifest.json").unlink()
        self.manifest.update(kind="controlled_cad_runtime", platform="win_amd64", python_abi="cp313",
                             runtime_qualified=False)
        for item in self.manifest["files"]:
            item["origin"] = "verified-baseline"
        self.save_manifest("controlled-runtime-manifest.json")
        result = self.run_stage()
        self.assertFalse(result["runtime_qualified"])
        self.assertTrue((self.output / "controlled-runtime-manifest.json").is_file())

    def test_source_unlisted_file_or_empty_directory_rejected(self):
        for name, directory in (("stale.py", False), ("empty", True)):
            with self.subTest(name=name):
                target = self.source / name
                target.mkdir() if directory else target.write_bytes(b"stale")
                with self.assertRaises(ValueError):
                    self.run_stage()
                target.rmdir() if directory else target.unlink()
                self.assertFalse(self.output.exists())

    def test_unsafe_receipt_names_and_namespace_collisions(self):
        for name in ("../escape", "CON.txt", "Lib/../escape", "Lib\\escape", "cad_library_adapter.py",
                     "CAD_LIBRARY_ADAPTER.PY/child", "cad-stage-manifest.json", "python.exe/child", "lib/other"):
            with self.subTest(name=name):
                original = list(self.manifest["files"])
                self.manifest["files"] = sorted(original + [record(name, b"x")], key=lambda r: r["path"])
                self.save_manifest()
                with self.assertRaises(ValueError):
                    self.run_stage()
                self.manifest["files"] = original
        self.assertFalse(self.output.exists())

    def test_hardlinked_source_or_adapter_rejected(self):
        for target in (self.source / "python.exe", self.adapter):
            alias = self.root / "alias"
            os.link(target, alias)
            with self.assertRaises(ValueError):
                self.run_stage()
            alias.unlink()

    def test_duplicate_json_and_ambiguous_manifests_rejected(self):
        (self.source / "runtime-manifest.json").write_bytes(b'{"schema_version":1,"schema_version":1}')
        with self.assertRaises(ValueError):
            self.run_stage()
        self.save_manifest()
        self.save_manifest("controlled-runtime-manifest.json")
        with self.assertRaises(ValueError):
            self.run_stage()

    def test_source_and_output_overlap_rejected(self):
        for output in (self.source, self.source / "stage", self.root):
            with self.subTest(output=output):
                with self.assertRaises(ValueError):
                    self.stage.stage(self.source, self.adapter, output)

    def test_source_change_before_final_recheck_preserves_old_output(self):
        self.run_stage()
        old = (self.output / self.stage.MANIFEST).read_bytes()
        self.adapter.write_bytes(b"new")
        real = self.stage.copy_verified
        changed = False
        def sabotage(source, target, receipt, **kwargs):
            nonlocal changed
            handle = real(source, target, receipt, **kwargs)
            if not changed:
                changed = True
                (self.source / "python.exe").write_bytes(b"changed after copy")
            return handle
        with mock.patch.object(self.stage, "copy_verified", side_effect=sabotage):
            with self.assertRaises((ValueError, OSError)):
                self.run_stage()
        self.assertEqual((self.output / self.stage.MANIFEST).read_bytes(), old)

    def test_publication_failure_restores_previous_output(self):
        self.run_stage()
        old = (self.output / self.stage.MANIFEST).read_bytes()
        self.adapter.write_bytes(b"new")
        real = self.stage.Directory.rename
        def fail_new(handle, destination):
            if handle.path.name.startswith(".cad-stage-"):
                raise OSError("injected publication failure")
            return real(handle, destination)
        with mock.patch.object(self.stage.Directory, "rename", new=fail_new):
            with self.assertRaisesRegex(OSError, "injected"):
                self.run_stage()
        self.assertEqual((self.output / self.stage.MANIFEST).read_bytes(), old)
        self.assertFalse(self.output.with_name(self.output.name + ".rollback").exists())

    def test_stage_root_substitution_is_blocked_by_held_identity(self):
        real = self.stage.copy_verified
        attempted = False
        def substitute(source, target, receipt, **kwargs):
            nonlocal attempted
            handle = real(source, target, receipt, **kwargs)
            if not attempted:
                attempted = True
                private = next(self.output.parent.glob(".cad-stage-*"))
                with self.assertRaises(OSError):
                    private.rename(private.with_name("foreign-move"))
            return handle
        with mock.patch.object(self.stage, "copy_verified", side_effect=substitute):
            self.run_stage()
        self.assertTrue(attempted)

    def test_unknown_file_added_to_private_stage_is_never_deleted(self):
        real = self.stage.copy_verified
        added = False
        def inject(source, target, receipt, **kwargs):
            nonlocal added
            handle = real(source, target, receipt, **kwargs)
            if not added:
                added = True
                private = next(self.output.parent.glob(".cad-stage-*"))
                (private / "foreign.txt").write_bytes(b"foreign")
            return handle
        with mock.patch.object(self.stage, "copy_verified", side_effect=inject):
            with self.assertRaises(ValueError):
                self.run_stage()
        private = next(self.output.parent.glob(".cad-stage-*"))
        self.assertEqual((private / "foreign.txt").read_bytes(), b"foreign")

    def test_no_replace_publication_preserves_output_created_during_staging(self):
        real = self.stage.Directory.rename
        def create_destination(handle, destination):
            if handle.path.name.startswith(".cad-stage-"):
                destination.mkdir()
                (destination / "foreign.txt").write_bytes(b"foreign output")
            return real(handle, destination)
        with mock.patch.object(self.stage.Directory, "rename", new=create_destination):
            with self.assertRaises(OSError):
                self.run_stage()
        self.assertEqual((self.output / "foreign.txt").read_bytes(), b"foreign output")

    def test_foreign_destination_prevents_restore_but_retains_identified_rollback(self):
        self.run_stage()
        previous = (self.output / self.stage.MANIFEST).read_bytes()
        self.adapter.write_bytes(b"new")
        real = self.stage.Directory.rename
        def create_destination(handle, destination):
            if handle.path.name.startswith(".cad-stage-"):
                destination.mkdir()
                (destination / "foreign.txt").write_bytes(b"foreign output")
            return real(handle, destination)
        with mock.patch.object(self.stage.Directory, "rename", new=create_destination):
            with self.assertRaises(OSError):
                self.run_stage()
        self.assertEqual((self.output / "foreign.txt").read_bytes(), b"foreign output")
        rollback = self.output.with_name(self.output.name + ".rollback")
        self.assertEqual((rollback / self.stage.MANIFEST).read_bytes(), previous)

    def test_same_bytes_substitution_during_rename_is_detected_and_preserved(self):
        real = self.stage.Directory.rename
        substituted = False
        def substitute_member(handle, destination):
            nonlocal substituted
            if handle.path.name.startswith(".cad-stage-") and not substituted:
                substituted = True
                original = handle.path / "python.exe"
                original.rename(self.root / "owned-file-retained")
                original.write_bytes(self.files["python.exe"])
            return real(handle, destination)
        with mock.patch.object(self.stage.Directory, "rename", new=substitute_member):
            with self.assertRaisesRegex(ValueError, "substitut"):
                self.run_stage()
        private = next(self.output.parent.glob(".cad-stage-*"))
        self.assertEqual((private / "python.exe").read_bytes(), self.files["python.exe"])
        self.assertEqual((self.root / "owned-file-retained").read_bytes(), self.files["python.exe"])
        self.assertFalse(self.output.exists())

    def test_unknown_directory_added_to_private_stage_is_never_deleted(self):
        real = self.stage.copy_verified
        added = False
        def inject(source, target, receipt, **kwargs):
            nonlocal added
            handle = real(source, target, receipt, **kwargs)
            if not added:
                added = True
                private = next(self.output.parent.glob(".cad-stage-*"))
                (private / "foreign-empty-directory").mkdir()
            return handle
        with mock.patch.object(self.stage, "copy_verified", side_effect=inject):
            with self.assertRaises(ValueError):
                self.run_stage()
        private = next(self.output.parent.glob(".cad-stage-*"))
        self.assertTrue((private / "foreign-empty-directory").is_dir())

    def test_source_new_file_at_final_recheck_is_refused(self):
        real = self.stage.copy_verified
        added = False
        def inject(source, target, receipt, **kwargs):
            nonlocal added
            handle = real(source, target, receipt, **kwargs)
            if not added:
                added = True
                (self.source / "late.py").write_bytes(b"late unknown input")
            return handle
        with mock.patch.object(self.stage, "copy_verified", side_effect=inject):
            with self.assertRaises(ValueError):
                self.run_stage()
        self.assertFalse(self.output.exists())

    def test_manifest_cannot_list_itself(self):
        self.manifest["files"].append(record("runtime-manifest.json", b"ignored"))
        self.manifest["files"].sort(key=lambda r: r["path"])
        self.save_manifest()
        with self.assertRaises(ValueError):
            self.run_stage()

    def test_file_size_limits(self):
        for size in (-1, True, self.stage._receipts.MAX_FILE_BYTES + 1):
            with self.subTest(size=size):
                self.manifest["files"][0]["bytes"] = size
                self.save_manifest()
                with self.assertRaises(ValueError):
                    self.run_stage()

    def test_link_adapter_is_refused(self):
        target = self.root / "real-adapter.py"
        self.adapter.rename(target)
        try:
            self.adapter.symlink_to(target)
        except OSError:
            self.skipTest("Windows symlink privilege unavailable")
        with self.assertRaises(ValueError):
            self.run_stage()

    def test_link_source_directory_is_refused(self):
        target = self.root / "real-baseline"
        self.source.rename(target)
        try:
            self.source.symlink_to(target, target_is_directory=True)
        except OSError:
            self.skipTest("Windows symlink privilege unavailable")
        with self.assertRaises(ValueError):
            self.run_stage()

    def test_aggregate_and_file_count_limits(self):
        for name, limit in (("MAX_TOTAL_BYTES", 1), ("MAX_FILES", 1)):
            with self.subTest(name=name):
                with mock.patch.object(self.stage._receipts, name, limit):
                    with self.assertRaises(ValueError):
                        self.run_stage()
        self.assertFalse(self.output.exists())

    def test_atomic_stage_creation_never_adopts_existing_directory(self):
        identity = "a" * 32
        unknown = self.output.parent / (".cad-stage-" + identity)
        unknown.mkdir()
        (unknown / "foreign.txt").write_bytes(b"foreign")
        with mock.patch.object(self.stage.uuid, "uuid4", return_value=mock.Mock(hex=identity)):
            with self.assertRaises(OSError):
                self.run_stage()
        self.assertEqual((unknown / "foreign.txt").read_bytes(), b"foreign")
        self.assertFalse(self.output.exists())

    def test_short_copy_write_cleans_only_created_stage(self):
        with mock.patch.object(self.stage.Object, "write", side_effect=OSError("Short write")):
            with self.assertRaisesRegex(OSError, "Short write"):
                self.run_stage()
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.output.parent.glob(".cad-stage-*")), [])

    def test_nonwindows_platform_refuses_before_mutation(self):
        with mock.patch.object(self.stage.os, "name", "posix"):
            with self.assertRaisesRegex(ValueError, "Windows"):
                self.run_stage()
        self.assertFalse(self.output.exists())

    def test_unicode_nonbmp_output_uses_correct_windows_rename_length(self):
        self.output = self.output.with_name("cad-runtime-\U0001f600")
        self.assertEqual(self.run_stage()["status"], "published")
        self.assertEqual(self.run_stage()["status"], "unchanged")

    def test_three_thousand_files_replace_without_crt_descriptor_growth(self):
        import msvcrt
        for index in range(3000):
            name = "payload/f%04d.bin" % index
            data = str(index).encode("ascii")
            target = self.source / name
            target.parent.mkdir(exist_ok=True)
            target.write_bytes(data)
            self.files[name] = data
        self.manifest["files"] = sorted((record(n, d) for n, d in self.files.items()), key=lambda r: r["path"])
        self.save_manifest()
        with mock.patch.object(msvcrt, "open_osfhandle", side_effect=AssertionError("retained CRT descriptor")):
            self.run_stage()
            self.adapter.write_bytes(b"second")
            self.run_stage()
            self.adapter.write_bytes(b"third")
            self.run_stage()

    def test_final_hash_insertion_is_detected_by_repeated_inventory(self):
        real = self.stage.Object.receipt
        added = False
        def inject(item, **kwargs):
            nonlocal added
            result = real(item, **kwargs)
            if not added and item.path == self.source / "runtime-manifest.json":
                added = True
                (self.source / "late.txt").write_bytes(b"foreign")
            return result
        with mock.patch.object(self.stage.Object, "receipt", new=inject):
            with self.assertRaises(ValueError):
                self.run_stage()
        self.assertFalse(self.output.exists())

    def test_old_tree_reopen_failure_restores_both_prior_slots(self):
        self.run_stage()
        first = (self.output / self.stage.MANIFEST).read_bytes()
        self.adapter.write_bytes(b"second")
        self.run_stage()
        second = (self.output / self.stage.MANIFEST).read_bytes()
        self.adapter.write_bytes(b"third")
        real = self.stage.Tree.reopen
        failed = False
        def fail(tree):
            nonlocal failed
            if not failed and tree.path == self.output.with_name(self.output.name + ".rollback"):
                failed = True
                raise OSError("injected reopen failure")
            return real(tree)
        with mock.patch.object(self.stage.Tree, "reopen", new=fail):
            with self.assertRaisesRegex(OSError, "injected reopen"):
                self.run_stage()
        self.assertEqual((self.output / self.stage.MANIFEST).read_bytes(), second)
        self.assertEqual((self.output.with_name(self.output.name + ".rollback") / self.stage.MANIFEST).read_bytes(), first)

    def prepare_rotation(self):
        self.run_stage()
        self.adapter.write_bytes(b"second")
        self.run_stage()
        second = (self.output / self.stage.MANIFEST).read_bytes()
        self.adapter.write_bytes(b"third")
        return second

    def test_persistent_new_tree_reopen_failure_restores_original_root_slots_first(self):
        self.prepare_rotation()
        rollback = self.output.with_name(self.output.name + ".rollback")
        def snapshot(path):
            root = self.stage.Directory(path)
            try:
                identity = root.identity
            finally:
                root.close()
            files = {p.relative_to(path).as_posix(): p.read_bytes() for p in path.rglob("*") if p.is_file()}
            return identity, files
        original_output = snapshot(self.output)
        original_rollback = snapshot(rollback)
        new_adapter_sha = hashlib.sha256(b"third").hexdigest()
        real = self.stage.Tree.reopen
        failures = 0
        def fail_new(tree):
            nonlocal failures
            if tree.table and tree.table["cad_library_adapter.py"]["sha256"] == new_adapter_sha:
                failures += 1
                raise OSError("persistent new-tree reopen failure")
            return real(tree)
        with mock.patch.object(self.stage.Tree, "reopen", new=fail_new):
            with self.assertRaisesRegex(OSError, "persistent new-tree"):
                self.run_stage()
        self.assertGreaterEqual(failures, 2)
        self.assertEqual(snapshot(self.output), original_output)
        self.assertEqual(snapshot(rollback), original_rollback)
        journal = self.output.with_name(self.output.name + ".transaction.json")
        self.assertTrue(journal.is_file())
        plan = json.loads(journal.read_bytes())
        private = self.output.parent / plan["names"]["stage"]
        self.assertEqual((private / "cad_library_adapter.py").read_bytes(), b"third")
        self.assertFalse((self.output.parent / plan["names"]["retired"]).exists())
        self.run_stage()
        self.assertFalse(journal.exists())
        self.assertEqual((self.output / "cad_library_adapter.py").read_bytes(), b"third")

    def test_crash_recovery_at_every_rotation_boundary(self):
        for boundary in ("prepared", "rollback-retired", "current-rollback", "new-output", "committed", "retired-removed"):
            with self.subTest(boundary=boundary):
                # Separate bounded output sibling for each interruption state.
                self.output = self.output.with_name("runtime-" + boundary)
                self.adapter.write_bytes(b"first")
                second = self.prepare_rotation()
                def crash(name):
                    if name == boundary:
                        raise SimulatedCrash(name)
                with mock.patch.object(self.stage, "checkpoint", side_effect=crash):
                    with self.assertRaises(SimulatedCrash):
                        self.run_stage()
                journal = self.output.with_name(self.output.name + ".transaction.json")
                self.assertTrue(journal.is_file())
                self.run_stage()
                self.assertEqual((self.output / "cad_library_adapter.py").read_bytes(), b"third")
                rollback = self.output.with_name(self.output.name + ".rollback")
                self.assertEqual((rollback / self.stage.MANIFEST).read_bytes(), second)
                self.assertFalse(journal.exists())
                self.assertEqual(list(self.output.parent.glob(".cad-retired-*")), [])

    def test_committed_partial_retirement_recovers_only_recorded_members(self):
        second = self.prepare_rotation()
        real = self.stage.Object.delete
        crashed = False
        def crash(item):
            nonlocal crashed
            real(item)
            if not crashed and any(p.name.startswith(".cad-retired-") for p in item.path.parents):
                crashed = True
                raise SimulatedCrash("during original retirement")
        with mock.patch.object(self.stage.Object, "delete", new=crash):
            with self.assertRaises(SimulatedCrash):
                self.run_stage()
        self.run_stage()
        self.assertEqual((self.output / "cad_library_adapter.py").read_bytes(), b"third")
        self.assertEqual((self.output.with_name(self.output.name + ".rollback") / self.stage.MANIFEST).read_bytes(), second)
        self.assertEqual(list(self.output.parent.glob(".cad-retired-*")), [])

    def test_foreign_member_in_retired_tree_preserves_record_and_all_slots(self):
        self.prepare_rotation()
        def inject(name):
            if name == "committed":
                retired = next(self.output.parent.glob(".cad-retired-*"))
                (retired / "foreign.txt").write_bytes(b"foreign")
        with mock.patch.object(self.stage, "checkpoint", side_effect=inject):
            with self.assertRaises(ValueError):
                self.run_stage()
        retired = next(self.output.parent.glob(".cad-retired-*"))
        self.assertEqual((retired / "foreign.txt").read_bytes(), b"foreign")
        journal = self.output.with_name(self.output.name + ".transaction.json")
        self.assertTrue(journal.exists())
        with self.assertRaises(ValueError):
            self.run_stage()
        self.assertEqual((retired / "foreign.txt").read_bytes(), b"foreign")
        self.assertTrue(journal.exists())

    def test_recovery_never_discovers_unrecorded_private_siblings(self):
        self.prepare_rotation()
        def crash(name):
            if name == "current-rollback":
                raise SimulatedCrash()
        with mock.patch.object(self.stage, "checkpoint", side_effect=crash):
            with self.assertRaises(SimulatedCrash):
                self.run_stage()
        unknown = self.output.parent / ".cad-stage-unrecorded"
        unknown.mkdir()
        (unknown / "foreign.txt").write_bytes(b"foreign")
        self.run_stage()
        self.assertEqual((unknown / "foreign.txt").read_bytes(), b"foreign")

    def test_torn_transaction_is_preserved_and_refused(self):
        self.prepare_rotation()
        def crash(name):
            if name == "prepared":
                raise SimulatedCrash()
        with mock.patch.object(self.stage, "checkpoint", side_effect=crash):
            with self.assertRaises(SimulatedCrash):
                self.run_stage()
        journal = self.output.with_name(self.output.name + ".transaction.json")
        original = journal.read_bytes()
        journal.write_bytes(original + b"commi")
        with self.assertRaisesRegex(ValueError, "Ambiguous"):
            self.run_stage()
        self.assertEqual(journal.read_bytes(), original + b"commi")

    def test_initial_publication_crash_recovery(self):
        for boundary in ("prepared", "new-output", "committed"):
            with self.subTest(boundary=boundary):
                self.output = self.output.with_name("initial-" + boundary)
                def crash(name):
                    if name == boundary:
                        raise SimulatedCrash()
                with mock.patch.object(self.stage, "checkpoint", side_effect=crash):
                    with self.assertRaises(SimulatedCrash):
                        self.run_stage()
                self.run_stage()
                self.assertEqual((self.output / "cad_library_adapter.py").read_bytes(), self.adapter.read_bytes())
                self.assertFalse(self.output.with_name(self.output.name + ".transaction.json").exists())

    def test_crash_during_abort_cleanup_can_recover_recorded_partial_new_tree(self):
        second = self.prepare_rotation()
        rename = self.stage.Directory.rename
        delete = self.stage.Object.delete
        def fail_new(item, destination):
            if item.path.name.startswith(".cad-stage-") and destination == self.output:
                raise OSError("publication failure")
            return rename(item, destination)
        crashed = False
        def crash_cleanup(item):
            nonlocal crashed
            delete(item)
            if not crashed and any(p.name.startswith(".cad-stage-") for p in item.path.parents):
                crashed = True
                raise SimulatedCrash("during abort cleanup")
        with mock.patch.object(self.stage.Directory, "rename", new=fail_new), mock.patch.object(self.stage.Object, "delete", new=crash_cleanup):
            with self.assertRaises(SimulatedCrash):
                self.run_stage()
        self.assertEqual((self.output / self.stage.MANIFEST).read_bytes(), second)
        self.run_stage()
        self.assertEqual((self.output / "cad_library_adapter.py").read_bytes(), b"third")
        self.assertFalse(self.output.with_name(self.output.name + ".transaction.json").exists())

    def test_same_byte_foreign_retired_member_is_preserved(self):
        self.prepare_rotation()
        retained = self.root / "retained-original"
        replacement = None
        # The retained DELETE handle excludes the CRT's no-delete share mode;
        # use the known fixture bytes for the replacement rather than reopen.
        def substitute_without_read(name):
            nonlocal replacement
            if name == "committed":
                retired = next(self.output.parent.glob(".cad-retired-*"))
                replacement = retired / "python.exe"
                replacement.rename(retained)
                replacement.write_bytes(self.files["python.exe"])
        with mock.patch.object(self.stage, "checkpoint", side_effect=substitute_without_read):
            with self.assertRaises(ValueError):
                self.run_stage()
        self.assertEqual(replacement.read_bytes(), self.files["python.exe"])
        self.assertEqual(retained.read_bytes(), self.files["python.exe"])
        with self.assertRaises(ValueError):
            self.run_stage()
        self.assertTrue(replacement.exists())
        self.assertTrue(retained.exists())

    def test_transaction_name_tampering_refuses_without_discovery(self):
        self.prepare_rotation()
        def crash(name):
            if name == "prepared":
                raise SimulatedCrash()
        with mock.patch.object(self.stage, "checkpoint", side_effect=crash):
            with self.assertRaises(SimulatedCrash):
                self.run_stage()
        journal = self.output.with_name(self.output.name + ".transaction.json")
        value = json.loads(journal.read_bytes())
        value["names"]["stage"] = "../foreign"
        data = json.dumps(value).encode() + b"\n"
        journal.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "names"):
            self.run_stage()
        self.assertEqual(journal.read_bytes(), data)


if __name__ == "__main__":
    unittest.main()
