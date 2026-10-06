"""Inert rebuild safety fixtures. No CMake, compiler, network or SDK mutation."""
import hashlib
import gzip
import importlib.util
import io
import json
import os
import pathlib
import tarfile
import tempfile
import unittest
from unittest.mock import patch
from contextlib import ExitStack

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "rebuild_ifc_boost_thread", ROOT / "scripts/rebuild_ifc_boost_thread.py")
recipe = importlib.util.module_from_spec(spec)
if spec.loader and pathlib.Path(spec.origin).exists():
    spec.loader.exec_module(recipe)


def archive(entries):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode="w:gz") as output:
        for name, data, kind in entries:
            item = tarfile.TarInfo(name)
            item.type = kind
            item.size = len(data) if kind == tarfile.REGTYPE else 0
            output.addfile(item, io.BytesIO(data) if item.size else None)
    return stream.getvalue()


class BoostThreadRebuildTests(unittest.TestCase):
    def fixture(self, parent, stack):
        kit, sdk, tools = parent / "kit", parent / "sdk", parent / "tools"
        for path in (kit, sdk, tools):
            path.mkdir()
        entries = [("thread-boost-1.86.0/" + name, b"// inert source\n", tarfile.REGTYPE)
                   for name in sorted(recipe.SOURCES)]
        entries += [("thread-boost-1.86.0/CMakeLists.txt", b"# inert\n", tarfile.REGTYPE),
                    ("thread-boost-1.86.0/include/boost/thread.hpp", b"header\n", tarfile.REGTYPE)]
        archive_data = archive(entries)
        files = {recipe.ARCHIVE: archive_data,
                 recipe.THREAD_RECIPE + "/portfile.cmake": b"historical thread recipe\n",
                 recipe.THREAD_RECIPE + "/vcpkg.json": b'{"version":"1.86.0"}',
                 recipe.HELPER_RECIPE + "/boost-install.cmake": b"historical helper\n"}
        records = []
        for name, data in files.items():
            target = kit / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            records.append({"path": name, "bytes": len(data),
                            "sha256": hashlib.sha256(data).hexdigest()})
        manifest = {"schema_version": 1, "files": sorted(records, key=lambda x: x["path"]),
                    "packages": [
                        {"sdk": "support", "name": "boost-thread", "version": "1.86.0",
                         "architecture": "x64-windows-ifc-static",
                         "recipe": {"path": recipe.THREAD_RECIPE},
                         "resources": [{"name": "boostorg/thread", "path": recipe.ARCHIVE,
                                        "sha512": hashlib.sha512(archive_data).hexdigest(),
                                        "url": "git+https://github.com/boostorg/thread@boost-1.86.0"}]},
                        {"sdk": "support", "name": "vcpkg-boost",
                         "recipe": {"path": recipe.HELPER_RECIPE}}]}
        manifest_data = json.dumps(manifest).encode()
        (kit / recipe.MANIFEST).write_bytes(manifest_data)
        sdk_files = {"include/boost/thread.hpp": b"header\n", "include/other.hpp": b"other\n",
                     "share/boost/cmake-build/BoostRoot.cmake": b"# root\n",
                     "share/boost/cmake-build/BoostInstall.cmake": b"# install\n",
                     "share/boost/BoostConfig.cmake": b"# config\n",
                     "lib/boost_thread-vc143-mt-x64-1_86.lib": b"original library",
                     "lib/dependency.lib": b"dependency",
                     "debug/lib/dependency.lib": b"debug dependency"}
        for name, data in sdk_files.items():
            target = sdk / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        pins = {name: hashlib.sha256(sdk_files[name]).hexdigest() for name in recipe.SDK_PINS}
        for constant, value in (("MANIFEST_SHA256", hashlib.sha256(manifest_data).hexdigest()),
                                ("ARCHIVE_SHA256", hashlib.sha256(archive_data).hexdigest()),
                                ("ARCHIVE_SHA512", hashlib.sha512(archive_data).hexdigest()),
                                ("SDK_PINS", pins), ("THREAD_HEADER_COUNT", 1)):
            stack.enter_context(patch.object(recipe, constant, value))
        for name in ("cmake.exe", "cl.exe"):
            (tools / name).write_bytes(b"inert fixture; never execute")
        return {"source_kit": kit, "support_sdk": sdk, "output": parent / "out",
                "cmake": tools / "cmake.exe", "compiler": tools / "cl.exe",
                "vs_instance": tools, "inputs_quiescent": True}

    def test_prepare_only_hashes_complete_closure_and_never_runs_tools(self):
        self.assertTrue(hasattr(recipe, "rebuild"), "Missing isolated rebuild recipe")
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            args = self.fixture(pathlib.Path(temp), stack)
            stack.enter_context(patch.object(recipe, "run_command", side_effect=AssertionError("native execution")))
            result = recipe.rebuild(**args, prepare_only=True)
            self.assertEqual(result["status"], "prepared")
            self.assertFalse(result["build_qualified"])
            self.assertFalse(result["source_closure_qualified"])
            self.assertFalse(result["product_runtime_replaced"])
            self.assertEqual(len(result["sdk_files"]), 8)
            self.assertIn("debug/lib/dependency.lib", {r["path"] for r in result["sdk_files"]})
            self.assertEqual(len(result["thread_headers"]), 1)
            self.assertEqual(result["commands"][-1][-4:], ["--target", "boost_thread", "--parallel", "4"])
            self.assertTrue((args["output"] / "evidence/inputs" / recipe.ARCHIVE).is_file())
            self.assertFalse((args["output"] / "build").exists())
            for record in result["source_files"]:
                self.assertEqual(record["sha256"], hashlib.sha256(
                    (args["output"] / "source" / record["path"]).read_bytes()).hexdigest())

    def test_existing_output_and_input_overlap_refused_without_writes(self):
        self.assertTrue(hasattr(recipe, "rebuild"), "Missing output isolation")
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            args = self.fixture(pathlib.Path(temp), stack)
            for output in (args["source_kit"] / "new", args["support_sdk"] / "new",
                           args["vs_instance"] / "new", args["source_kit"]):
                with self.subTest(output=output), self.assertRaises(ValueError):
                    recipe.rebuild(**{**args, "output": output}, prepare_only=True)
                if output.name == "new":
                    self.assertFalse(output.exists())
            args["output"].mkdir()
            witness = args["output"] / "witness"
            witness.write_bytes(b"preserve")
            with self.assertRaises(ValueError):
                recipe.rebuild(**args, prepare_only=True)
            self.assertEqual(witness.read_bytes(), b"preserve")

    def test_changed_header_or_archive_or_manifest_fails_closed(self):
        self.assertTrue(hasattr(recipe, "rebuild"), "Missing exact input binding")
        for changed in ("header", "archive", "manifest"):
            with self.subTest(changed=changed), tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
                args = self.fixture(pathlib.Path(temp), stack)
                path = {"header": args["support_sdk"] / "include/boost/thread.hpp",
                        "archive": args["source_kit"] / recipe.ARCHIVE,
                        "manifest": args["source_kit"] / recipe.MANIFEST}[changed]
                path.write_bytes(path.read_bytes() + b"changed")
                with self.assertRaises(ValueError):
                    recipe.rebuild(**args, prepare_only=True)
                self.assertFalse((args["output"] / "evidence/rebuild.json").exists())

    def test_failed_command_keeps_streams_exitcode_and_partial_evidence(self):
        self.assertTrue(hasattr(recipe, "rebuild"), "Missing failure receipts")
        class FailedChild:
            stdout = io.BytesIO(b"fixture stdout\n")
            stderr = io.BytesIO(b"fixture stderr\n")
            returncode = 7
            def poll(self):
                return 7
            def wait(self):
                return 7
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            args = self.fixture(pathlib.Path(temp), stack)
            stack.enter_context(patch.object(recipe.subprocess, "Popen", return_value=FailedChild()))
            with self.assertRaises(ValueError):
                recipe.rebuild(**args, prepare_only=False)
            receipt = json.loads((args["output"] / "evidence/rebuild-failed.json").read_bytes())
            self.assertEqual(receipt["status"], "failed")
            self.assertEqual(receipt["command_results"][0]["exit_code"], 7)
            self.assertEqual((args["output"] / "evidence/logs/00.stdout.log").read_bytes(), b"fixture stdout\n")
            self.assertFalse((args["output"] / "evidence/rebuild.json").exists())
            self.assertTrue((args["output"] / "source/libs/thread/src/future.cpp").is_file())

    def native_fixture(self, args, *, mutate=None, foreign_compiler=False, missing_library=False):
        class Child:
            def __init__(self, argv, **kwargs):
                self.stdout, self.stderr = io.BytesIO(b"inert tool stdout\n"), io.BytesIO(b"")
                self.returncode = 0
                output = args["output"]
                if "-S" in argv:
                    identity = output / "build/CMakeFiles/4.0/CMakeCXXCompiler.cmake"
                    identity.parent.mkdir(parents=True)
                    compiler = args["compiler"] if not foreign_compiler else args["cmake"]
                    identity.write_text(f'set(CMAKE_CXX_COMPILER "{compiler.as_posix()}")\n'
                                        'set(CMAKE_CXX_COMPILER_VERSION "19.44.35228.0")\n')
                    project = output / "build/libs/thread/boost_thread.vcxproj"
                    project.parent.mkdir(parents=True)
                    project.write_text('<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003"><ItemGroup>' +
                                       ''.join(f'<ClCompile Include="{(output / "source/libs/thread" / name).as_posix()}" />'
                                               for name in sorted(recipe.SOURCES)) + '</ItemGroup></Project>')
                if "--build" in argv:
                    if not missing_library:
                        target = output / "build/stage/lib/Release/boost_thread-vc143-mt-x64-1_86.lib"
                        target.parent.mkdir(parents=True)
                        target.write_bytes(b"rebuilt inert library")
                    if mutate:
                        mutate()
            def poll(self):
                return 0
            def wait(self):
                return 0
        return Child

    def test_built_receipt_binds_library_tools_generated_sources_and_logs(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            args = self.fixture(pathlib.Path(temp), stack)
            stack.enter_context(patch.object(recipe.subprocess, "Popen", side_effect=self.native_fixture(args)))
            result = recipe.rebuild(**args)
            self.assertEqual(result["status"], "built")
            self.assertFalse(result["build_qualified"])
            self.assertEqual(result["library"]["sha256"], hashlib.sha256(b"rebuilt inert library").hexdigest())
            self.assertEqual(len(result["command_results"]), 4)
            self.assertEqual(result["generated_build"]["sources"], sorted(recipe.SOURCES))
            for record in result["command_results"]:
                self.assertEqual(record["exit_code"], 0)
                for log in record["streams"]:
                    self.assertEqual(log["sha256"], hashlib.sha256(
                        (args["output"] / log["path"]).read_bytes()).hexdigest())
            self.assertEqual((args["support_sdk"] / "lib/boost_thread-vc143-mt-x64-1_86.lib").read_bytes(),
                             b"original library")

    def test_post_build_mutation_missing_output_and_wrong_compiler_fail(self):
        for failure in ("source", "sdk", "missing", "compiler"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
                args = self.fixture(pathlib.Path(temp), stack)
                changed = args["output"] / "source/libs/thread/src/future.cpp" if failure == "source" else args["support_sdk"] / "lib/dependency.lib"
                mutate = (lambda: changed.write_bytes(b"changed")) if failure in {"source", "sdk"} else None
                stack.enter_context(patch.object(recipe.subprocess, "Popen", side_effect=self.native_fixture(
                    args, mutate=mutate, foreign_compiler=failure == "compiler", missing_library=failure == "missing")))
                with self.assertRaises((ValueError, OSError)):
                    recipe.rebuild(**args)
                self.assertFalse((args["output"] / "evidence/rebuild.json").exists())
                self.assertEqual(json.loads((args["output"] / "evidence/rebuild-failed.json").read_bytes())["status"], "failed")

    def test_command_log_limit_retains_bounded_log_and_refuses_success(self):
        class Child:
            def __init__(self, *argv, **kwargs):
                self.stdout, self.stderr = io.BytesIO(b"x" * 100), io.BytesIO(b"")
            def poll(self):
                return 0
            def wait(self):
                return 0
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            args = self.fixture(pathlib.Path(temp), stack)
            stack.enter_context(patch.object(recipe, "MAX_LOG", 8))
            stack.enter_context(patch.object(recipe.subprocess, "Popen", side_effect=Child))
            with self.assertRaises(ValueError):
                recipe.rebuild(**args)
            self.assertEqual((args["output"] / "evidence/logs/00.stdout.log").read_bytes(), b"xxxxxxxx")
            receipt = json.loads((args["output"] / "evidence/rebuild-failed.json").read_bytes())
            self.assertIsNotNone(receipt["command_results"][0]["error"])

    def test_generic_cli_output_policy_and_unacknowledged_freeze_refused(self):
        for unsafe in (pathlib.Path("relative"), pathlib.Path("C:/Users/Matt/out"),
                       pathlib.Path("C:/Build/with space"), pathlib.Path("//server/share/out")):
            with self.subTest(unsafe=unsafe), self.assertRaises(ValueError):
                recipe.generic_output(unsafe)
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            args = self.fixture(pathlib.Path(temp), stack)
            with self.assertRaises(ValueError):
                recipe.rebuild(**{**args, "inputs_quiescent": False}, prepare_only=True)
            self.assertFalse(args["output"].exists())

    def test_reparse_point_on_input_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            path = pathlib.Path(temp) / "input"
            path.write_bytes(b"source")
            original = path.lstat()
            class ReparseStat:
                st_file_attributes = 0x400
                st_mode = original.st_mode
            with patch.object(pathlib.Path, "lstat", return_value=ReparseStat()):
                with self.assertRaises(ValueError):
                    recipe.file_record(path, "input")

    def test_environment_flags_are_removed_and_launch_error_is_retained(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            args = self.fixture(pathlib.Path(temp), stack)
            stack.enter_context(patch.dict(os.environ, {"CL": "/evil", "CMAKE_TOOLCHAIN_FILE": "evil", "CXX": "evil"}))
            observed = {}
            def fail_launch(argv, **kwargs):
                observed.update(kwargs["env"])
                raise OSError("inert launch refusal")
            stack.enter_context(patch.object(recipe.subprocess, "Popen", side_effect=fail_launch))
            with self.assertRaises(ValueError):
                recipe.rebuild(**args)
            self.assertFalse({"CL", "CMAKE_TOOLCHAIN_FILE", "CXX"} & set(observed))
            receipt = json.loads((args["output"] / "evidence/rebuild-failed.json").read_bytes())
            self.assertIsNone(receipt["command_results"][0]["exit_code"])
            self.assertIn("inert launch refusal", receipt["command_results"][0]["error"])

    def test_unsafe_archive_names_and_types_never_extract(self):
        self.assertTrue(hasattr(recipe, "extract_thread"), "Missing bounded archive extraction")
        with tempfile.TemporaryDirectory() as temp:
            for bad in ("../escape", "C:/escape", "x\\escape", "x/aux.txt",
                        "x/name.", "x/a:stream", "x/./a", "x//a"):
                with self.subTest(bad=bad), self.assertRaises(ValueError):
                    recipe.extract_thread(archive([(bad, b"bad", tarfile.REGTYPE)]),
                                          pathlib.Path(temp) / "out")
            for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.FIFOTYPE,
                         tarfile.CHRTYPE):
                with self.subTest(kind=kind), self.assertRaises(ValueError):
                    recipe.extract_thread(archive([("thread-boost-1.86.0/a", b"", kind)]),
                                          pathlib.Path(temp) / "out")
            self.assertFalse((pathlib.Path(temp) / "out").exists())

    def test_case_collisions_are_refused_before_any_output(self):
        self.assertTrue(hasattr(recipe, "extract_thread"), "Missing archive collision guard")
        with tempfile.TemporaryDirectory() as temp:
            output = pathlib.Path(temp) / "out"
            for names in (("a", "A"), ("Dir/a", "dir/b"), ("a", "a/b"), ("a", "a")):
                with self.subTest(names=names), self.assertRaises(ValueError):
                    recipe.extract_thread(archive([
                        ("thread-boost-1.86.0/" + name, b"x", tarfile.REGTYPE)
                        for name in names]), output)
            self.assertFalse(output.exists())

    def test_tar_metadata_expansion_is_bounded_before_output(self):
        self.assertTrue(hasattr(recipe, "MAX_TAR"), "Missing bound on expanded tar metadata")
        with tempfile.TemporaryDirectory() as temp, patch.object(recipe, "MAX_TAR", 1024):
            output = pathlib.Path(temp) / "out"
            with self.assertRaises(ValueError):
                recipe.extract_thread(gzip.compress(b"\0" * 2048), output)
            self.assertFalse(output.exists())

    def test_generated_project_duplicate_compile_entries_refused(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            args = self.fixture(pathlib.Path(temp), stack)
            child = self.native_fixture(args)
            def duplicated(argv, **kwargs):
                process = child(argv, **kwargs)
                if "-S" in argv:
                    project = args["output"] / "build/libs/thread/boost_thread.vcxproj"
                    data = project.read_text()
                    entry = f'<ClCompile Include="{(args["output"] / "source/libs/thread/src/future.cpp").as_posix()}" />'
                    project.write_text(data.replace('</ItemGroup>', entry + '</ItemGroup>'))
                return process
            stack.enter_context(patch.object(recipe.subprocess, "Popen", side_effect=duplicated))
            with self.assertRaises(ValueError):
                recipe.rebuild(**args)
            self.assertFalse((args["output"] / "evidence/rebuild.json").exists())

    def test_file_hardlink_refused(self):
        self.assertTrue(hasattr(recipe, "file_record"), "Missing hardlink guard")
        with tempfile.TemporaryDirectory() as temp:
            original = pathlib.Path(temp) / "input"
            original.write_bytes(b"source")
            alias = pathlib.Path(temp) / "alias"
            os.link(original, alias)
            with self.assertRaises(ValueError):
                recipe.file_record(original, "input")


if __name__ == "__main__":
    unittest.main()
