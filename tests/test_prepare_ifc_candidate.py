"""Synthetic packaging boundaries; these fixtures do not prove a native build."""
import hashlib
import importlib.util
import json
import marshal
import pathlib
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("prepare_ifc_candidate", ROOT / "scripts/prepare_ifc_candidate.py")
candidate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(candidate)


def record(path):
    return {"path": str(path), "bytes": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


class SyntheticCandidateTests(unittest.TestCase):
    def fixture(self, directory):
        base = pathlib.Path(directory)
        root, build, output = base / "workspace", base / "build", base / "candidate"
        root.mkdir()
        build.mkdir()
        lock = json.loads((ROOT / "third_party/ifc-source-lock.json").read_text())
        lock_path = root / "third_party/ifc-source-lock.json"
        lock_path.parent.mkdir()
        lock_path.write_text(json.dumps(lock))
        source = root / lock["source"]["path"]
        files = {"COPYING": b"synthetic GPL", "COPYING.LESSER": b"synthetic LGPL", "VERSION": b"0.8.4\n",
                 "src/ifcopenshell-python/ifcopenshell/__init__.py": b"raise RuntimeError('must not import')\n",
                 "src/ifcopenshell-python/ifcopenshell/util/data.json": b"{}",
                 "src/ifcopenshell-python/ifcopenshell/mvd/__init__.py": b"# synthetic mvd\n",
                 "src/ifcopenshell-python/ifcopenshell/mvd/LICENSE": b"synthetic LGPL3",
                 "src/ifcopenshell-python/ifcopenshell/simple_spf/__init__.py": b"# synthetic spf\n",
                 "src/ifcopenshell-python/ifcopenshell/simple_spf/LICENSE": b"synthetic LGPL2",
                 "src/ifcopenshell-python/ifcopenshell/simple_spf/.gitignore": b"ignored control"}
        for name, data in files.items():
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        prep = {"schema_version": 1, "lock_sha256": record(lock_path)["sha256"],
                "build_qualified": False, "source_closure_qualified": False,
                "source": {**lock["source"], "files": [{**record(source / name), "path": name} for name in sorted(files)]},
                "submodules": [{k: item[k] for k in ("path", "repository", "revision")} for item in lock["submodules"]],
                "swig": {"version": "4.3.1", "archive_sha512": lock["swig"]["sha512"]}}
        prep_path = root / ".deps/ifc-source-preparation.json"
        prep_path.write_text(json.dumps(prep))
        script = root / "scripts/prepare_ifc_source.py"
        script.parent.mkdir()
        script.write_bytes(b"# synthetic source checker\n")
        recipe = root / "scripts/build-ifc-source.ps1"
        recipe.write_bytes(b"# synthetic recipe\n")
        cache = build / "build/CMakeCache.txt"
        cache.parent.mkdir()
        cmake = root / "tools/cmake.exe"
        cmake.parent.mkdir()
        cmake.write_bytes(b"synthetic CMake; never execute")
        python = root / ".deps/cad-runtime/3.13.15"
        python_inputs = {
            "python.exe": b"synthetic interpreter; never execute",
            "libs/python313.lib": b"synthetic import library",
            "include/Python.h": b"synthetic header",
            "include/patchlevel.h": b'#define PY_VERSION "3.13.15"\n',
            "runtime-manifest.json": b'{"schema_version":1,"python_version":"3.13.15"}'}
        for name, data in python_inputs.items():
            path = python / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        wrapper = build / "build/ifcwrap/ifcopenshell_wrapper.py"
        wrapper.parent.mkdir()
        wrapper.write_bytes(b"# synthetic SWIG wrapper\n")
        extension = wrapper.parent / "Release/_ifcopenshell_wrapper.cp313-win_amd64.pyd"
        extension.parent.mkdir()
        pe = bytearray(256)
        pe[:2] = b"MZ"
        struct.pack_into("<I", pe, 0x3c, 128)
        pe[128:132] = b"PE\0\0"
        struct.pack_into("<H", pe, 132, 0x8664)
        struct.pack_into("<H", pe, 150, 0x2000)
        extension.write_bytes(pe)
        configuration = ["-S", str(source / "cmake"), "-B", str(build / "build"),
                         "-G", "Visual Studio 17 2022", "-A", "x64",
                         "-DCMAKE_CONFIGURATION_TYPES=Release", "-DBUILD_IFCPYTHON=ON", "-DBUILD_IFCGEOM=ON",
                         "-DPYTHON_EXECUTABLE=" + str(python / "python.exe"),
                         "-DPYTHON_INCLUDE_DIR=" + str(python / "include"),
                         "-DPYTHON_LIBRARY=" + str(python / "libs/python313.lib"),
                         "-DSCHEMA_VERSIONS=2x3;4;4x1;4x2;4x3;4x3_tc1;4x3_add1;4x3_add2"]
        cache_options = [argument[2:].replace("=", ":UNINITIALIZED=", 1) for argument in configuration if argument.startswith("-D")]
        cache_options += ["CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022", "CMAKE_GENERATOR_PLATFORM:INTERNAL=x64",
                          "CMAKE_COMMAND:INTERNAL=" + str(cmake),
                          "CMAKE_HOME_DIRECTORY:INTERNAL=" + str(source / "cmake"),
                          "CMAKE_CACHEFILE_DIR:INTERNAL=" + str(build / "build")]
        cache.write_text("\n".join(cache_options) + "\n")
        version_log = build / "python-version.stdout.log"
        version_log.write_bytes(b"3.13.15 (synthetic version evidence) [MSC v.1944 64 bit (AMD64)]\n")
        evidence = {"schema_version": 1, "state": "built-unqualified", "workspace": str(root),
                    "build_root": str(build), "configuration": "Release", "build_qualified": False,
                    "source_closure_qualified": False, "product_runtime_replaced": False,
                    "configure_arguments": configuration,
                    "inputs": [record(p) for p in (lock_path, prep_path, script, recipe, cache, cmake, *(python / name for name in python_inputs))],
                    "outputs": [record(extension)],
                    "logs": [{"path": str(version_log), "sha256": record(version_log)["sha256"]}],
                    "commands": [{"name": "source-check", "exit_code": 0, "executable": str(python / "python.exe"), "arguments": ["-I", "-B", str(script), "--offline", "--check"]},
                                 {"name": "configure", "exit_code": 0, "executable": str(cmake), "arguments": configuration},
                                 {"name": "build-release", "exit_code": 0, "executable": str(cmake), "arguments": ["--build", str(build / "build"), "--config", "Release", "--target", "ifcopenshell_wrapper", "--parallel", "4"]},
                                 {"name": "python-version", "exit_code": 0, "executable": str(python / "python.exe"),
                                  "arguments": ["-I", "-B", "-c", 'import sys,struct; assert sys.version_info[:3] == (3,13,15) and struct.calcsize("P") == 8; print(sys.version)'],
                                  "stdout": str(version_log)}]}
        evidence_path = build / "build-evidence.json"
        evidence_path.write_text(json.dumps(evidence))
        return root, build, output, evidence_path, evidence, prep_path

    def stage(self, root, output, evidence_path, **kwargs):
        return candidate.prepare(root, evidence_path, output, **kwargs)

    def derived_fixture(self, directory):
        root, build, output, evidence_path, evidence, prep_path = self.fixture(directory)
        source = root / ".deps/ifc-src"
        lock = json.loads((root / "third_party/ifc-source-lock.json").read_bytes())
        for name, data in {".gitmodules": b"synthetic gitlinks\n",
                           "cmake/CMakeLists.txt": b"synthetic CMake source\n",
                           "src/ifcwrap/utils/typemaps_out.i": b"// original typemaps\n",
                           **{item["path"] + "/fixture.txt": b"selected child\n"
                              for item in lock["submodules"]}}.items():
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        prep = json.loads(prep_path.read_bytes())
        prep["source"]["files"] = [{**record(path), "path": path.relative_to(source).as_posix()}
                                   for path in sorted(source.rglob("*")) if path.is_file()]
        prep["source"]["files"].sort(key=lambda item: item["path"])
        prep["swig"]["path"] = ".deps/ifc-tools/swigwin-4.3.1"
        prep_path.write_text(json.dumps(prep))
        helper = root / "scripts/prepare_ifc_derived_source.py"
        helper.write_bytes((ROOT / "scripts/prepare_ifc_derived_source.py").read_bytes())
        patch_path = root / "third_party/ifc-source/patches/opaque-coordinate-output.i"
        patch_path.parent.mkdir(parents=True)
        patch_path.write_bytes((ROOT / "third_party/ifc-source/patches/opaque-coordinate-output.i").read_bytes())
        derivation_spec = importlib.util.spec_from_file_location("fixture_derivation",
            ROOT / "scripts/prepare_ifc_derived_source.py")
        derivation = importlib.util.module_from_spec(derivation_spec)
        derivation_spec.loader.exec_module(derivation)
        derived_source = build / "source"
        manifest = build / "source-derivation.json"
        derivation.derive(root, derived_source, manifest)
        evidence["source_derivation"] = str(manifest)
        for item in evidence["inputs"]:
            if pathlib.Path(item["path"]) == prep_path:
                item.update(record(prep_path))
        evidence["inputs"].extend(record(path) for path in (helper, patch_path, manifest))
        evidence["commands"].append({"name": "source-derive", "exit_code": 0,
            "executable": str(root / ".deps/cad-runtime/3.13.15/python.exe"),
            "arguments": ["-I", "-B", str(helper), "--workspace", str(root),
                          "--output", str(derived_source), "--manifest", str(manifest)]})
        evidence["configure_arguments"][1] = str(derived_source / "cmake")
        cache = build / "build/CMakeCache.txt"
        cache.write_text(cache.read_text().replace(str(source / "cmake"), str(derived_source / "cmake")))
        for item in evidence["inputs"]:
            if pathlib.Path(item["path"]) == cache:
                item.update(record(cache))
        evidence_path.write_text(json.dumps(evidence))
        return root, build, output, evidence_path, evidence

    def test_derived_source_stage_preserves_reviewed_delta_provenance(self):
        with tempfile.TemporaryDirectory() as directory:
            root, build, output, evidence_path, _ = self.derived_fixture(directory)
            result = self.stage(root, output, evidence_path)
            manifest = build / "source-derivation.json"
            self.assertEqual(result["source_derivation_sha256"], record(manifest)["sha256"])
            self.assertEqual((output / "provenance/source-derivation.json").read_bytes(), manifest.read_bytes())
            self.assertEqual((output / "provenance/source-derivation-verifier.py").read_bytes(),
                             (ROOT / "scripts/prepare_ifc_derived_source.py").read_bytes())
            self.assertFalse(result["build_qualified"])
            self.assertFalse(result["source_closure_qualified"])

    def test_derived_verifier_ignores_timestamp_valid_foreign_bytecode(self):
        with tempfile.TemporaryDirectory() as directory:
            root, build, output, evidence_path, evidence = self.derived_fixture(directory)
            helper = root / "scripts/prepare_ifc_derived_source.py"
            manifest = build / "source-derivation.json"
            value = json.loads(manifest.read_bytes())
            # The fixture acts as the shipped verifier root for this check.
            value["inputs"]["script"]["path"] = str(helper)
            manifest.write_text(json.dumps(value))
            next(item for item in evidence["inputs"] if pathlib.Path(item["path"]) == manifest).update(record(manifest))
            evidence_path.write_text(json.dumps(evidence))
            code = compile("raise RuntimeError('unbound cached bytecode executed')", str(helper), "exec")
            cache = pathlib.Path(importlib.util.cache_from_source(str(helper)))
            cache.parent.mkdir()
            info = helper.stat()
            cache.write_bytes(importlib.util.MAGIC_NUMBER +
                struct.pack("<III", 0, int(info.st_mtime), info.st_size) + marshal.dumps(code))
            # Prove that ordinary loader execution would accept this cache.
            spec = importlib.util.spec_from_file_location("foreign_cache_probe", helper)
            with self.assertRaisesRegex(RuntimeError, "unbound cached bytecode executed"):
                spec.loader.exec_module(importlib.util.module_from_spec(spec))
            with patch.object(candidate, "ROOT", root):
                result = self.stage(root, output, evidence_path)
            self.assertFalse(result["build_qualified"])
            self.assertEqual((output / "provenance/source-derivation-verifier.py").read_bytes(), helper.read_bytes())

    def test_derived_source_tamper_and_wrong_config_rejected_before_output(self):
        for fault in ("source", "pristine", "manifest", "helper", "patch", "wrong_config", "missing_command"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as directory:
                root, build, output, evidence_path, evidence = self.derived_fixture(directory)
                if fault == "source":
                    (build / "source/cmake/CMakeLists.txt").write_bytes(b"foreign source")
                elif fault == "pristine":
                    (root / ".deps/ifc-src/src/ifcwrap/utils/typemaps_out.i").write_bytes(b"foreign original")
                elif fault == "manifest":
                    manifest = build / "source-derivation.json"
                    value = json.loads(manifest.read_bytes())
                    value["product_runtime_replaced"] = 0
                    manifest.write_text(json.dumps(value))
                    next(item for item in evidence["inputs"] if pathlib.Path(item["path"]) == manifest).update(record(manifest))
                elif fault == "helper":
                    (root / "scripts/prepare_ifc_derived_source.py").write_bytes(b"raise RuntimeError('must not execute')")
                elif fault == "patch":
                    (root / "third_party/ifc-source/patches/opaque-coordinate-output.i").write_bytes(b"unreviewed")
                elif fault == "wrong_config":
                    evidence["configure_arguments"][1] = str(root / ".deps/ifc-src/cmake")
                else:
                    evidence["commands"] = [item for item in evidence["commands"] if item["name"] != "source-derive"]
                evidence_path.write_text(json.dumps(evidence))
                with self.assertRaises(ValueError):
                    self.stage(root, output, evidence_path)
                self.assertFalse(output.exists())

    def test_derivation_requires_manifest_and_bound_helper_patch(self):
        for incomplete in ("command-only", "manifest-only"):
            with self.subTest(case=incomplete), tempfile.TemporaryDirectory() as directory:
                root, build, output, evidence_path, evidence, _ = self.fixture(directory)
                if incomplete == "command-only":
                    evidence["commands"].append({"name": "source-derive", "exit_code": 0})
                else:
                    evidence["source_derivation"] = str(build / "source-derivation.json")
                evidence_path.write_text(json.dumps(evidence))
                with self.assertRaises(ValueError):
                    self.stage(root, output, evidence_path)
                self.assertFalse(output.exists())

    def test_synthetic_cmake_commands_require_bound_cache_executable(self):
        for command_name in ("configure", "build-release"):
            with self.subTest(command=command_name), tempfile.TemporaryDirectory() as directory:
                root, _, output, evidence_path, evidence, _ = self.fixture(directory)
                command = next(item for item in evidence["commands"] if item["name"] == command_name)
                command["executable"] = str(root / "tools/foreign.exe")
                evidence_path.write_text(json.dumps(evidence))
                with self.assertRaises(ValueError):
                    self.stage(root, output, evidence_path)
                self.assertFalse(output.exists())

    def test_synthetic_stage_preserves_package_and_records_origins(self):
        with tempfile.TemporaryDirectory() as directory:
            root, build, output, evidence_path, _, _ = self.fixture(directory)
            result = self.stage(root, output, evidence_path)
            self.assertFalse(result["build_qualified"])
            self.assertFalse(result["source_closure_qualified"])
            self.assertFalse(result["cpp_corresponding_source_delivered"])
            self.assertFalse(result["dependency_license_closure_delivered"])
            self.assertTrue((output / "ifcopenshell/mvd/LICENSE").is_file())
            self.assertTrue((output / "ifcopenshell/simple_spf/LICENSE").is_file())
            self.assertTrue((output / "ifcopenshell/util/data.json").is_file())
            self.assertFalse((output / "ifcopenshell/simple_spf/.gitignore").exists())
            self.assertEqual(len(list((output / "ifcopenshell").glob("*.pyd"))), 1)
            for item in result["files"]:
                self.assertEqual(record(output / item["path"])["sha256"], item["sha256"])
                self.assertIn("kind", item["origin"])
            self.assertEqual(json.loads((output / "candidate-manifest.json").read_text()), result)
            wrapper = next(item for item in result["files"] if item["path"] == "ifcopenshell/ifcopenshell_wrapper.py")
            self.assertIs(wrapper["origin"]["bound_in_original_build_evidence"], False)
            self.assertIn("hashed at staging, not in build evidence", (output / "NOTICES.txt").read_text())

    def test_original_build_wrapper_receipt_is_verified_and_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            root, build, output, evidence_path, evidence, _ = self.fixture(directory)
            wrapper = build / "build/ifcwrap/ifcopenshell_wrapper.py"
            evidence["generated_wrapper"] = record(wrapper)
            evidence_path.write_text(json.dumps(evidence))
            result = self.stage(root, output, evidence_path)
            item = next(item for item in result["files"] if item["path"] == "ifcopenshell/ifcopenshell_wrapper.py")
            self.assertIs(item["origin"]["bound_in_original_build_evidence"], True)
            self.assertEqual(item["sha256"], evidence["generated_wrapper"]["sha256"])
            self.assertEqual((output / item["path"]).read_bytes(), wrapper.read_bytes())
            notice = (output / "NOTICES.txt").read_text()
            self.assertIn("bound by the original build evidence", notice)
            self.assertNotIn("not in build evidence", notice)
            for flag in ("build_qualified", "source_closure_qualified", "product_runtime_replaced",
                         "cpp_corresponding_source_delivered", "dependency_license_closure_delivered"):
                self.assertIs(result[flag], False)

    def test_original_build_wrapper_receipt_rejects_tamper_wrong_path_and_schema(self):
        for fault in ("tampered", "missing", "wrong-path", "wrong-bytes", "wrong-hash", "extra-key", "non-object"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as directory:
                root, build, output, evidence_path, evidence, _ = self.fixture(directory)
                wrapper = build / "build/ifcwrap/ifcopenshell_wrapper.py"
                evidence["generated_wrapper"] = record(wrapper)
                if fault == "tampered":
                    wrapper.write_bytes(b"stale or changed wrapper")
                elif fault == "missing":
                    wrapper.unlink()
                elif fault == "wrong-path":
                    evidence["generated_wrapper"]["path"] = str(build / "elsewhere/ifcopenshell_wrapper.py")
                elif fault == "wrong-bytes":
                    evidence["generated_wrapper"]["bytes"] += 1
                elif fault == "wrong-hash":
                    evidence["generated_wrapper"]["sha256"] = "0" * 64
                elif fault == "extra-key":
                    evidence["generated_wrapper"]["claimed_binding"] = True
                else:
                    evidence["generated_wrapper"] = None
                evidence_path.write_text(json.dumps(evidence))
                with self.assertRaises(ValueError):
                    self.stage(root, output, evidence_path)
                self.assertFalse(output.exists())

    def test_synthetic_incomplete_and_failed_builds_write_nothing(self):
        mutations = [lambda e: e.update(state="configured"), lambda e: e.update(build_qualified=True),
                     lambda e: e["commands"][2].update(exit_code=1), lambda e: e["commands"].pop(0),
                     lambda e: e["configure_arguments"].__setitem__(-1, "-DSCHEMA_VERSIONS=4"),
                     lambda e: e["commands"][2]["arguments"].__setitem__(3, "Debug"),
                     lambda e: e["commands"][2]["arguments"].extend(["--clean-first", "--verbose"]),
                     lambda e: e["outputs"][0].update(sha256="0" * 64)]
        for mutate in mutations:
            with self.subTest(mutate=mutate), tempfile.TemporaryDirectory() as directory:
                root, _, output, evidence_path, evidence, _ = self.fixture(directory)
                mutate(evidence)
                evidence_path.write_text(json.dumps(evidence))
                with self.assertRaises(ValueError):
                    self.stage(root, output, evidence_path)
                self.assertFalse(output.exists())

    def test_synthetic_source_changes_and_unreviewed_outputs_rejected(self):
        for target in ("source-change", "source-extra", "extra-extension", "native-wrong-machine", "wrapper-missing"):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as directory:
                root, build, output, evidence_path, _, _ = self.fixture(directory)
                package = root / ".deps/ifc-src/src/ifcopenshell-python/ifcopenshell"
                if target == "source-change":
                    (package / "__init__.py").write_bytes(b"changed")
                elif target == "source-extra":
                    (package / "foreign.py").write_bytes(b"foreign")
                elif target == "extra-extension":
                    (build / "build/ifcwrap/Release/foreign.pyd").write_bytes(b"foreign")
                elif target == "wrapper-missing":
                    (build / "build/ifcwrap/ifcopenshell_wrapper.py").unlink()
                else:
                    extension = build / "build/ifcwrap/Release/_ifcopenshell_wrapper.cp313-win_amd64.pyd"
                    data = bytearray(extension.read_bytes())
                    struct.pack_into("<H", data, 132, 0x14c)
                    extension.write_bytes(data)
                    evidence = json.loads(evidence_path.read_text())
                    evidence["outputs"] = [record(extension)]
                    evidence_path.write_text(json.dumps(evidence))
                with self.assertRaises(ValueError):
                    self.stage(root, output, evidence_path)
                self.assertFalse(output.exists())

    def test_synthetic_fresh_output_overlap_and_reparse_boundaries(self):
        with tempfile.TemporaryDirectory() as directory:
            root, build, output, evidence_path, _, _ = self.fixture(directory)
            output.mkdir()
            foreign = output / "foreign.txt"
            foreign.write_bytes(b"keep")
            for destination in (output, root / "candidate", build / "candidate", root.parent, root.parent / "CON.txt", root.parent / "bad."):
                with self.subTest(destination=destination), self.assertRaises(ValueError):
                    self.stage(root, destination, evidence_path)
            self.assertEqual(foreign.read_bytes(), b"keep")
            real = candidate.is_link
            with patch.object(candidate, "is_link", side_effect=lambda p: pathlib.Path(p) == build or real(p)), self.assertRaises(ValueError):
                self.stage(root, root.parent / "fresh", evidence_path)
            self.assertFalse((root.parent / "fresh").exists())

    def test_synthetic_recipe_snapshot_must_match_recorded_invocation(self):
        with tempfile.TemporaryDirectory() as directory:
            root, build, output, evidence_path, _, _ = self.fixture(directory)
            recipe = root / "scripts/build-ifc-source.ps1"
            snapshot = build / "build-recipe.ps1"
            snapshot.write_bytes(recipe.read_bytes())
            recipe.write_bytes(b"later reviewed recipe")
            with self.assertRaises(ValueError):
                self.stage(root, output, evidence_path)
            snapshot.write_bytes(b"wrong snapshot")
            with self.assertRaises(ValueError):
                self.stage(root, output, evidence_path, recipe_snapshot=snapshot)
            snapshot.write_bytes(b"# synthetic recipe\n")
            self.stage(root, output, evidence_path, recipe_snapshot=snapshot)
            self.assertEqual((output / "provenance/build-recipe.ps1").read_bytes(), snapshot.read_bytes())

    def test_synthetic_forged_preparation_identity_and_collision_rejected(self):
        for fault in ("lock-hash", "source-revision", "submodules", "duplicate-file", "ancestor-case"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as directory:
                root, _, output, evidence_path, evidence, prep_path = self.fixture(directory)
                prep = json.loads(prep_path.read_text())
                if fault == "lock-hash":
                    prep["lock_sha256"] = "0" * 64
                elif fault == "source-revision":
                    prep["source"]["revision"] = "0" * 40
                elif fault == "submodules":
                    prep["submodules"].pop()
                elif fault == "duplicate-file":
                    prep["source"]["files"].append(prep["source"]["files"][0])
                else:
                    item = next(x for x in prep["source"]["files"] if "/util/" in x["path"])
                    prep["source"]["files"].append({**item, "path": item["path"].replace("/util/", "/Util/")})
                prep_path.write_text(json.dumps(prep))
                next(x for x in evidence["inputs"] if x["path"] == str(prep_path)).update(record(prep_path))
                evidence_path.write_text(json.dumps(evidence))
                with self.assertRaises(ValueError):
                    self.stage(root, output, evidence_path)
                self.assertFalse(output.exists())

    def test_synthetic_bounded_validation_precedes_destination_creation(self):
        for bound in ("MAX_FILES", "MAX_TOTAL_BYTES", "MAX_FILE_BYTES", "MAX_JSON_BYTES", "MAX_INPUT_FILE_BYTES", "MAX_INPUT_TOTAL_BYTES"):
            with self.subTest(bound=bound), tempfile.TemporaryDirectory() as directory:
                root, _, output, evidence_path, _, _ = self.fixture(directory)
                with patch.object(candidate, bound, 1), self.assertRaises(ValueError):
                    self.stage(root, output, evidence_path)
                self.assertFalse(output.exists())

    def test_synthetic_duplicate_json_keys_and_unsafe_absolute_paths_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root, _, output, evidence_path, _, _ = self.fixture(directory)
            original = evidence_path.read_text()
            evidence_path.write_text(original.replace('"state": "built-unqualified"', '"state": "failed", "state": "built-unqualified"'))
            with self.assertRaises(ValueError):
                self.stage(root, output, evidence_path)
            self.assertFalse(output.exists())
        for path in ("relative/output", "C:relative", "C:/Build/a:stream", "C:/Build/../output", "C:/Build/NUL.txt",
                     "C:/Build/COM¹.txt", "C:/Build/conout$", "//server/share/candidate", "C:/Build/bad;list"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                candidate.absolute_path(path)

    def test_synthetic_copy_race_preserves_partial_and_foreign_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root, _, output, evidence_path, _, _ = self.fixture(directory)
            real_mkdir = pathlib.Path.mkdir
            source = root / ".deps/ifc-src/src/ifcopenshell-python/ifcopenshell/__init__.py"
            def race(path, *args, **kwargs):
                result = real_mkdir(path, *args, **kwargs)
                if path == output:
                    source.write_bytes(b"changed after validation")
                    (output / "foreign.txt").write_bytes(b"keep")
                return result
            with patch.object(pathlib.Path, "mkdir", race), self.assertRaises(ValueError):
                self.stage(root, output, evidence_path)
            self.assertEqual((output / "foreign.txt").read_bytes(), b"keep")
            self.assertFalse((output / "candidate-manifest.json").exists())

    def test_synthetic_sdk_input_limits_differ_from_payload_limits(self):
        # Real 784 SDK evidence: TKDESTEP.lib 986,667,008 bytes; aggregate inputs
        # 3,595,109,865 bytes. Exercise metadata bounds without allocating GB.
        library = {"path": "synthetic.lib", "bytes": 986_667_008, "sha256": "0" * 64}
        self.assertEqual(candidate.checked_record(library, input_file=True)["bytes"], library["bytes"])
        with self.assertRaises(ValueError):
            candidate.checked_record(library)
        with self.assertRaises(ValueError):
            candidate.checked_record({**library, "bytes": candidate.MAX_INPUT_FILE_BYTES + 1}, input_file=True)
        self.assertGreater(candidate.MAX_INPUT_TOTAL_BYTES, 3_595_109_865)
        with tempfile.TemporaryDirectory() as directory:
            root, _, output, evidence_path, _, _ = self.fixture(directory)
            # Scaled SDK file exceeds the payload per-file bound, but only its
            # input hash is read. It must not be copied into the candidate.
            library = root / "synthetic-sdk.lib"
            library.write_bytes(b"x" * 20_000)
            evidence = json.loads(evidence_path.read_text())
            evidence["inputs"].append(record(library))
            evidence_path.write_text(json.dumps(evidence))
            self.assertGreater(sum(item["bytes"] for item in evidence["inputs"]), 20_000)
            with patch.object(candidate, "MAX_FILE_BYTES", 10_000), patch.object(candidate, "MAX_TOTAL_BYTES", 20_000):
                self.stage(root, output, evidence_path)
            self.assertFalse((output / "synthetic-sdk.lib").exists())

    def test_synthetic_bound_cache_coherence_rejects_claim_only_evidence(self):
        for key, value in (("SCHEMA_VERSIONS", "4"), ("CMAKE_CONFIGURATION_TYPES", "Debug"),
                           ("BUILD_IFCPYTHON", "OFF"), ("BUILD_IFCGEOM", "OFF"),
                           ("CMAKE_GENERATOR", "Ninja"), ("CMAKE_GENERATOR_PLATFORM", "Win32"),
                           ("PYTHON_EXECUTABLE", "C:/foreign/python.exe"),
                           ("PYTHON_LIBRARY", "C:/foreign/python312.lib")):
            with self.subTest(key=key), tempfile.TemporaryDirectory() as directory:
                root, build, output, evidence_path, evidence, _ = self.fixture(directory)
                cache = build / "build/CMakeCache.txt"
                lines = cache.read_text().splitlines()
                lines = [line.split("=", 1)[0] + "=" + value if line.startswith(key + ":") else line for line in lines]
                cache.write_text("\n".join(lines) + "\n")
                next(x for x in evidence["inputs"] if x["path"] == str(cache)).update(record(cache))
                evidence_path.write_text(json.dumps(evidence))
                with self.assertRaises(ValueError):
                    self.stage(root, output, evidence_path)
                self.assertFalse(output.exists())

    def test_synthetic_python_version_and_executable_binding_required(self):
        for fault in ("missing-command", "version-code", "foreign-executable", "source-check-executable", "missing-input", "header", "runtime-version", "version-log"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as directory:
                root, build, output, evidence_path, evidence, _ = self.fixture(directory)
                version_command = evidence["commands"][3]
                if fault == "missing-command":
                    evidence["commands"].pop()
                elif fault == "version-code":
                    version_command["arguments"][3] = "print('3.13.15')"
                elif fault in ("foreign-executable", "source-check-executable"):
                    evidence["commands"][3 if fault == "foreign-executable" else 0]["executable"] = "C:/foreign/python.exe"
                elif fault == "missing-input":
                    evidence["inputs"] = [item for item in evidence["inputs"] if not item["path"].endswith("python313.lib")]
                else:
                    path = (root / ".deps/cad-runtime/3.13.15/include/patchlevel.h" if fault == "header" else
                            root / ".deps/cad-runtime/3.13.15/runtime-manifest.json" if fault == "runtime-version" else
                            build / "python-version.stdout.log")
                    path.write_bytes(b'#define PY_VERSION "3.12.15"\n' if fault == "header" else
                                     b'{"schema_version":1,"python_version":"3.12.15"}' if fault == "runtime-version" else b"3.12.15\n")
                    table = evidence["logs"] if fault == "version-log" else evidence["inputs"]
                    next(x for x in table if x["path"] == str(path)).update(record(path))
                evidence_path.write_text(json.dumps(evidence))
                with self.assertRaises(ValueError):
                    self.stage(root, output, evidence_path)
                self.assertFalse(output.exists())

    def test_synthetic_final_inventory_detects_foreign_hash_and_reparse_races(self):
        for fault in ("foreign-file", "foreign-empty-directory", "changed-copy", "reparse"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as directory:
                root, _, output, evidence_path, _, _ = self.fixture(directory)
                real_mkdir = pathlib.Path.mkdir
                real_is_link = candidate.is_link
                def race(path, *args, **kwargs):
                    result = real_mkdir(path, *args, **kwargs)
                    if path == output:
                        if fault in ("foreign-file", "reparse"):
                            (output / "foreign.txt").write_bytes(b"keep")
                        elif fault == "foreign-empty-directory":
                            real_mkdir(output / "foreign")
                    if fault == "changed-copy" and path == output / "licenses/ifcopenshell":
                        (output / "ifcopenshell/__init__.py").write_bytes(b"changed copy")
                    return result
                def reparse(path):
                    return (fault == "reparse" and path == output / "foreign.txt") or real_is_link(path)
                with patch.object(pathlib.Path, "mkdir", race), patch.object(candidate, "is_link", side_effect=reparse), self.assertRaises(ValueError):
                    self.stage(root, output, evidence_path)
                self.assertFalse((output / "candidate-manifest.json").exists())
                if fault in ("foreign-file", "reparse"):
                    self.assertEqual((output / "foreign.txt").read_bytes(), b"keep")


if __name__ == "__main__":
    unittest.main()
