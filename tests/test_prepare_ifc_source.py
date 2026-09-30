"""Controlled offline fixtures: no upstream code, Git mutations or network."""
import hashlib
import importlib.util
import io
import json
import os
import pathlib
import shutil
import stat
import tempfile
import unittest
import zipfile
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("prepare_ifc_source", ROOT / "scripts/prepare_ifc_source.py")
ifc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ifc)


class IfcPreparationTests(unittest.TestCase):
    def real_repository(self, root):
        source = root / ".deps/ifc-src"
        source.mkdir(parents=True)
        ifc.run_git(source, "init")
        ifc.run_git(source, "remote", "add", "origin",
                    "https://github.com/IfcOpenShell/IfcOpenShell.git")
        (source / "fixture.txt").write_bytes(b"locked source\n")
        ifc.run_git(source, "add", "fixture.txt")
        ifc.run_git(source, "-c", "user.name=Vertex fixture", "-c",
                    "user.email=fixture@invalid", "commit", "-m", "Locked fixture")
        return source, {"path": ifc.SOURCE_PATH,
                        "repository": "https://github.com/IfcOpenShell/IfcOpenShell.git",
                        "revision": ifc.run_git(source, "rev-parse", "HEAD")}

    @unittest.skipUnless(shutil.which("git"), "Real Git source integrity regression")
    def test_real_git_rejects_changed_bytes_hidden_by_index_flags(self):
        for flag in ("--assume-unchanged", "--skip-worktree"):
            with self.subTest(flag=flag), tempfile.TemporaryDirectory() as directory:
                source, item = self.real_repository(pathlib.Path(directory))
                ifc.run_git(source, "update-index", flag, "fixture.txt")
                (source / "fixture.txt").write_bytes(b"unlocked source\n")
                self.assertEqual(ifc.run_git(source, "status", "--porcelain=v1"), "")
                with self.assertRaisesRegex(ValueError, "index"):
                    ifc.verify_repository(source, item, source)
                self.assertEqual((source / "fixture.txt").read_bytes(), b"unlocked source\n")

    @unittest.skipUnless(shutil.which("git"), "Real Git replacement-object regression")
    def test_real_git_replacement_cannot_hide_changed_locked_source(self):
        with tempfile.TemporaryDirectory() as directory:
            source, item = self.real_repository(pathlib.Path(directory))
            (source / "fixture.txt").write_bytes(b"replacement source\n")
            ifc.run_git(source, "add", "fixture.txt")
            ifc.run_git(source, "-c", "user.name=Vertex fixture", "-c",
                        "user.email=fixture@invalid", "commit", "-m", "Replacement fixture")
            replacement = ifc.run_git(source, "rev-parse", "HEAD")
            ifc.run_git(source, "update-ref", "HEAD", item["revision"])
            ifc.run_git(source, "replace", item["revision"], replacement)
            with self.assertRaisesRegex(ValueError, "dirty"):
                ifc.verify_repository(source, item, source)
            self.assertEqual((source / "fixture.txt").read_bytes(), b"replacement source\n")

    def fixture(self, root):
        lock = json.loads((ROOT / "third_party/ifc-source-lock.json").read_text())
        source = root / ".deps/ifc-src"
        source.mkdir(parents=True)
        records = {source: lock["source"]}
        for item in lock["submodules"]:
            path = source / item["path"]
            path.mkdir(parents=True)
            records[path] = item
        for path in records:
            (path / ".git").mkdir()
            (path / "fixture.txt").write_bytes(b"source fixture\n")
        (source / ".gitmodules").write_text("\n".join(
            f'[submodule "{s["path"]}"]\n\tpath = {s["path"]}\n\turl = {s["repository"]}'
            for s in lock["submodules"]))
        cache = root / ".cache/ifc-source"
        cache.mkdir(parents=True)
        archive = cache / lock["swig"]["filename"]
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr("swigwin-4.3.1/swig.exe", b"inert exe fixture")
            output.writestr("swigwin-4.3.1/Lib/python/python.swg", b"inert swig fixture")
        lock["swig"]["sha512"] = hashlib.sha512(archive.read_bytes()).hexdigest()
        lock_path = root / "fixture-lock.json"
        lock_path.write_text(json.dumps(lock))
        faults = {}

        def git(path, *args):
            item = records[path]
            command = tuple(args)
            if command == ("rev-parse", "--show-toplevel"):
                return str(path)
            if command == ("rev-parse", "--absolute-git-dir"):
                return str(faults.get("gitdir", path / ".git"))
            if command == ("rev-parse", "HEAD"):
                return faults.get("head", item["revision"])
            if command == ("config", "--local", "--no-includes", "--get-all", "remote.origin.url"):
                return faults.get("origin", item["repository"])
            if args[0] == "status":
                return faults.get("dirty", "")
            if command == ("ls-files", "-v", "-z"):
                return faults.get("index_flags", "H fixture.txt\0")
            if command == ("ls-tree", "-r", "-z", "HEAD"):
                if path == source:
                    return "".join(f'160000 commit {s["revision"]}\t{s["path"]}\0' for s in lock["submodules"])
                return ""
            if args[0] == "ls-tree":
                sub = next(s for s in lock["submodules"] if s["path"] == args[-1])
                return f'160000 commit {faults.get("gitlink", sub["revision"])}\t{sub["path"]}'
            raise AssertionError(f"Unexpected Git operation: {command}")
        return lock_path, archive, faults, git

    def test_offline_roundtrip_records_inputs_without_qualification(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            lock, archive, _, git = self.fixture(root)
            with patch.object(ifc, "run_git", side_effect=git), patch.object(ifc.urllib.request, "urlopen", side_effect=AssertionError("network")):
                result = ifc.prepare(root, lock, offline=True)
                self.assertEqual(result, ifc.prepare(root, lock, offline=True, check=True))
            self.assertFalse(result["build_qualified"])
            self.assertFalse(result["source_closure_qualified"])
            self.assertEqual(result["source"]["revision"], "ff3c5b849eee2ef6343b537c885b971ae6bba452")
            self.assertEqual(len(result["submodules"]), 3)
            self.assertEqual(result["swig"]["archive_sha512"], hashlib.sha512(archive.read_bytes()).hexdigest())
            self.assertEqual((root / ".deps/ifc-tools/swigwin-4.3.1/swig.exe").read_bytes(), b"inert exe fixture")

    def test_git_identity_dirty_and_gitlink_fail_before_publication(self):
        for fault, value in (("head", "0" * 40), ("origin", "https://example.com/foreign.git"),
                             ("dirty", "?? foreign.txt"), ("gitlink", "1" * 40), ("gitdir", "C:/foreign/git")):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                lock, _, faults, git = self.fixture(root)
                faults[fault] = value
                with patch.object(ifc, "run_git", side_effect=git), self.assertRaises(ValueError):
                    ifc.prepare(root, lock, offline=True)
                self.assertFalse((root / ".deps/ifc-source-preparation.json").exists())
                self.assertFalse((root / ".deps/ifc-tools/swigwin-4.3.1").exists())

    def test_offline_missing_and_corrupt_inputs_are_preserved(self):
        for missing in (False, True):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                lock, archive, _, git = self.fixture(root)
                if missing:
                    archive.unlink()
                else:
                    archive.write_bytes(b"foreign cache")
                with patch.object(ifc, "run_git", side_effect=git), self.assertRaises(ValueError):
                    ifc.prepare(root, lock, offline=True)
                if not missing:
                    self.assertEqual(archive.read_bytes(), b"foreign cache")

    def test_foreign_source_and_tool_files_never_overwritten(self):
        for target in (".deps/ifc-src/.git", ".deps/ifc-tools/swigwin-4.3.1"):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                lock, _, _, git = self.fixture(root)
                path = root / target
                if path.name == ".git":
                    path.rmdir()
                    (path.parent / "foreign.txt").write_bytes(b"keep")
                else:
                    path.mkdir(parents=True)
                    (path / "foreign.txt").write_bytes(b"keep")
                with patch.object(ifc, "run_git", side_effect=git), self.assertRaises(ValueError):
                    ifc.prepare(root, lock, offline=True)
                self.assertEqual((path.parent if path.name == ".git" else path).joinpath("foreign.txt").read_bytes(), b"keep")

    def test_archive_table_rejects_windows_paths_links_and_collisions(self):
        cases = [["../escape"], ["swigwin-4.3.1/CON.txt"], ["swigwin-4.3.1/a:stream"],
                 ["swigwin-4.3.1/A/x", "swigwin-4.3.1/a/y"],
                 ["swigwin-4.3.1/file", "swigwin-4.3.1/file/child"], ["wrongroot/swig.exe"]]
        for entries in cases:
            with self.subTest(entries=entries), tempfile.TemporaryDirectory() as directory:
                stream = io.BytesIO()
                with zipfile.ZipFile(stream, "w") as archive:
                    for name in entries:
                        archive.writestr(name, b"x")
                target = pathlib.Path(directory) / "new"
                with self.assertRaises(ValueError):
                    ifc.extract_swig(stream.getvalue(), target)
                self.assertFalse(target.exists())
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, "w") as archive:
            item = zipfile.ZipInfo("swigwin-4.3.1/swig.exe")
            item.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive.writestr(item, "somewhere")
        with tempfile.TemporaryDirectory() as directory, self.assertRaises(ValueError):
            ifc.extract_swig(stream.getvalue(), pathlib.Path(directory) / "new")

    def test_expansion_bound_and_immutable_verified_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            lock_path, archive, _, _ = self.fixture(root)
            asset = json.loads(lock_path.read_text())["swig"]
            data = ifc.verified_swig_bytes(archive, asset)
            archive.write_bytes(b"changed after verification")
            with patch.object(ifc, "MAX_EXPANSION", 1), self.assertRaises(ValueError):
                ifc.extract_swig(data, root / "bounded")
            ifc.extract_swig(data, root / "valid")
            self.assertEqual((root / "valid/swig.exe").read_bytes(), b"inert exe fixture")

    def test_forged_installed_manifest_and_source_snapshot_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            lock, _, _, git = self.fixture(root)
            with patch.object(ifc, "run_git", side_effect=git):
                ifc.prepare(root, lock, offline=True)
                target = root / ".deps/ifc-tools/swigwin-4.3.1/swig.exe"
                target.write_bytes(b"modified")
                # A self-signed installed table must never replace archive proof.
                manifest = root / ".deps/ifc-source-preparation.json"
                data = json.loads(manifest.read_text())
                next(x for x in data["swig"]["files"] if x["path"] == "swig.exe")["sha256"] = hashlib.sha256(b"modified").hexdigest()
                manifest.write_text(json.dumps(data))
                with self.assertRaises(ValueError):
                    ifc.prepare(root, lock, offline=True, check=True)
                self.assertEqual(target.read_bytes(), b"modified")

    def test_reparse_input_is_rejected_without_following_it(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            lock, archive, _, git = self.fixture(root)
            real = ifc.is_link
            with patch.object(ifc, "run_git", side_effect=git), patch.object(ifc, "is_link", side_effect=lambda p: pathlib.Path(p) == archive or real(p)), self.assertRaises(ValueError):
                ifc.prepare(root, lock, offline=True)

    def test_source_config_cannot_execute_filters_or_include_other_config(self):
        for config in ('[filter "poison"]\nclean = execute-source\n', '[include]\npath = outside.config\n',
                       '\ufeff[filter "poison"]\nclean = execute-source\n', '\ufeff[include]\npath = outside.config\n'):
            with self.subTest(config=config), tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                lock, _, _, git = self.fixture(root)
                (root / ".deps/ifc-src/.git/config").write_text(config, encoding="utf-8")
                with patch.object(ifc, "run_git", side_effect=git), self.assertRaises(ValueError):
                    ifc.prepare(root, lock, offline=True)

    def test_check_never_recreates_missing_repository_or_tools(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            lock, _, _, git = self.fixture(root)
            with patch.object(ifc, "run_git", side_effect=git):
                ifc.prepare(root, lock, offline=True)
                path = root / ".deps/ifc-src/src/svgfill/.git"
                path.rmdir()
                with self.assertRaises(ValueError):
                    ifc.prepare(root, lock, offline=True, check=True)
                self.assertFalse(path.exists())

    def test_worktree_config_and_commondir_rejected_before_repository_status(self):
        for child in (False, True):
            for filename, content in (("config.worktree", '[filter "poison"]\nclean = execute-source\n'),
                                      ("config.worktree", '[includeIf "gitdir:*"]\npath = foreign\n'),
                                      ("config.worktree", '\ufeff[filter "poison"]\nclean = execute-source\n'),
                                      ("config.worktree", '\ufeff[includeIf "gitdir:*"]\npath = foreign\n'),
                                      ("commondir", "../../foreign-metadata\n")):
                with self.subTest(child=child, filename=filename, content=content), tempfile.TemporaryDirectory() as directory:
                    root = pathlib.Path(directory)
                    lock, _, _, git = self.fixture(root)
                    repository = root / ".deps/ifc-src"
                    if child:
                        repository /= "src/svgfill"
                    (repository / ".git" / filename).write_text(content, encoding="utf-8")
                    def guarded(path, *args):
                        if path == repository:
                            self.fail("Git read unsafe repository metadata before preflight")
                        if args[0] == "status":
                            if "--ignore-submodules=all" not in args:
                                self.fail("Git status may execute unsafe repository configuration")
                        return git(path, *args)
                    with patch.object(ifc, "run_git", side_effect=guarded), self.assertRaises(ValueError):
                        ifc.prepare(root, lock, offline=True)
                    self.assertEqual((repository / ".git" / filename).read_text(encoding="utf-8"), content)

    def test_hidden_index_flags_rejected_before_status(self):
        for record in ("h fixture.txt\0", "S missing.txt\0", "s hidden.txt\0"):
            with self.subTest(record=record), tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                lock, _, faults, git = self.fixture(root)
                faults["index_flags"] = record
                def guarded(path, *args):
                    if args[0] == "status":
                        self.fail("Git status cannot prove bytes hidden by index flags")
                    return git(path, *args)
                with patch.object(ifc, "run_git", side_effect=guarded), self.assertRaises(ValueError):
                    ifc.prepare(root, lock, offline=True)

    def test_unselected_gitlink_content_is_preserved_and_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            lock, _, _, git = self.fixture(root)
            source = root / ".deps/ifc-src"
            foreign = source / "test/input"
            foreign.mkdir(parents=True)
            (foreign / "foreign.txt").write_bytes(b"keep")
            def with_foreign(path, *args):
                result = git(path, *args)
                if path == source and args == ("ls-tree", "-r", "-z", "HEAD"):
                    result += "160000 commit " + "f" * 40 + "\ttest/input\0"
                return result
            with patch.object(ifc, "run_git", side_effect=with_foreign), self.assertRaises(ValueError):
                ifc.prepare(root, lock, offline=True)
            self.assertEqual((foreign / "foreign.txt").read_bytes(), b"keep")

    def test_git_invocation_disables_object_replacement(self):
        with patch.object(ifc.subprocess, "run") as run:
            run.return_value.returncode = 0
            run.return_value.stdout = "fixture\n"
            self.assertEqual(ifc.run_git(ROOT, "rev-parse", "HEAD"), "fixture")
        command = run.call_args.args[0]
        self.assertIn("--no-replace-objects", command)
        self.assertEqual(run.call_args.kwargs["env"]["GIT_NO_REPLACE_OBJECTS"], "1")

    def test_normal_submodule_gitfile_keeps_explicit_child_verification(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            lock, _, _, git = self.fixture(root)
            source = root / ".deps/ifc-src"
            child = source / "src/svgfill"
            (child / ".git").rmdir()
            metadata = source / ".git/modules/src/svgfill"
            metadata.mkdir(parents=True)
            (metadata / "config.worktree").write_text('[core]\nignorecase = true\n')
            (child / ".git").write_text("gitdir: " + os.path.relpath(metadata, child) + "\n")
            checked_children = set()
            def with_gitfile(path, *args):
                if path == child and args == ("rev-parse", "--absolute-git-dir"):
                    return str(metadata)
                if args[0] == "status":
                    self.assertIn("--ignore-submodules=all", args)
                    checked_children.add(path)
                return git(path, *args)
            with patch.object(ifc, "run_git", side_effect=with_gitfile):
                ifc.prepare(root, lock, offline=True)
            self.assertIn(child, checked_children)
            self.assertEqual((child / ".git").read_text(), "gitdir: " + os.path.relpath(metadata, child) + "\n")


if __name__ == "__main__":
    unittest.main()
