"""The composer copies bound bytes; it grants no runtime or license clearance."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "controlled_composer", ROOT / "scripts/qualification/compose_controlled_cad_runtime.py")
composer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(composer)


def record(path, data):
    return {"path": path, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


class ControlledComposerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "workspace"
        self.base = self.root / ".deps/cad-runtime/3.13.15"
        self.candidate = Path(self.temp.name) / "candidate"
        self.out = self.root / composer.OUTPUT_RELATIVE
        self.root.mkdir()
        self.base.mkdir(parents=True)
        self.candidate.mkdir()
        versions = {"ifcopenshell": "0.8.3.post2", "ezdxf": "1.4.3",
                    **{f"other{i}": "1" for i in range(9)}}
        assets = [{"name": name, "version": version, "kind": "wheel",
                   "filename": f"{name}-{version}.whl", "sha256": "a" * 64,
                   "url": "https://files.pythonhosted.org/a"} for name, version in versions.items()]
        for kind in ("interpreter", "headers", "development"):
            assets.append({"name": kind, "version": "3.13.15", "kind": kind,
                           "filename": kind + ".zip", "sha256": "b" * 64,
                           "url": "https://www.python.org/a"})
        lock = {"schema_version": 1, "platform": "win_amd64", "python_abi": "cp313",
                "python_version": "3.13.15", "library_versions": versions, "assets": assets}
        lock_record = self.put(self.root, "third_party/cad-runtime-lock.json", lock)
        self.base_files = [self.put(self.base, name, data) for name, data in (
            ("python.exe", b"python"), ("python313._pth", b"python313.zip\n.\nLib/site-packages\n"),
            ("libs/python313.lib", b"python library"), ("include/Python.h", b"python header"),
            ("include/patchlevel.h", b'PY_VERSION "3.13.15"'),
            ("Lib/site-packages/ezdxf/__init__.py", b"ezdxf exact"),
            ("Lib/site-packages/ifcopenshell/__init__.py", b"old"),
            ("Lib/site-packages/ifcopenshell/old-only.py", b"obsolete"),
            ("Lib/site-packages/ifcopenshell-0.8.3.post2.dist-info/METADATA", b"old wheel"),
            ("notices/base.txt", b"base notice"))]
        self.base_manifest = {"schema_version": 1, "lock_sha256": lock_record["sha256"],
            "python_version": "3.13.15", "library_versions": versions, "qualification": "incomplete",
            "production_worker_integrated": False, "files": sorted(self.base_files, key=lambda r: r["path"])}
        self.put(self.base, "runtime-manifest.json", self.base_manifest)
        self.source = {"path": ".deps/ifc-src", "repository": composer.SOURCE_REPOSITORY,
                       "revision": composer.SOURCE_REVISION}
        self.submodules = [{"path": p, "repository": url, "revision": rev, "purpose": "source"}
                           for p, url, rev in composer.SUBMODULES]
        self.candidate_files = []
        self.preparation_files = []
        for name, data in (("__init__.py", b'__version__ = version = "0.0.0"\n'),
                           ("mvd/__init__.py", b"mvd"), ("mvd/LICENSE", b"mvd notice"),
                           ("simple_spf/__init__.py", b"spf"), ("simple_spf/LICENSE", b"spf notice")):
            source_name = composer.PACKAGE_SOURCE + "/" + name
            owner = next((s for s in self.submodules if source_name.startswith(s["path"] + "/")), self.source)
            origin_name = source_name[len(owner["path"]) + 1:] if owner is not self.source else source_name
            self.add_candidate("ifcopenshell/" + name, data, {"kind": "locked-source",
                "repository": owner["repository"], "revision": owner["revision"], "path": origin_name})
            self.preparation_files.append(record(source_name, data))
        for name in ("COPYING", "COPYING.LESSER", "VERSION"):
            data = ("notice " + name).encode()
            self.add_candidate("licenses/ifcopenshell/" + name, data,
                {"kind": "locked-source", "repository": self.source["repository"],
                 "revision": self.source["revision"], "path": name})
            self.preparation_files.append(record(name, data))
        self.native_data, self.wrapper_data = b"fixture native bytes", b"fixture generated wrapper"
        self.build_root = "C:/private/build"
        self.old_workspace = "C:/private/workspace"
        self.visual_studio = "C:/private/visual-studio"
        self.git_root = "C:/private/git"
        self.native_origin = self.build_root + "/build/ifcwrap/Release/" + composer.EXTENSION_NAME
        self.wrapper_origin = self.build_root + "/build/ifcwrap/ifcopenshell_wrapper.py"
        self.add_candidate("ifcopenshell/" + composer.EXTENSION_NAME, self.native_data,
                           {"kind": "bound-build-output", "path": self.native_origin})
        self.add_candidate("ifcopenshell/ifcopenshell_wrapper.py", self.wrapper_data,
            {"kind": "generated-build-output", "path": self.wrapper_origin,
             "bound_in_original_build_evidence": False})
        source_lock = {"schema_version": 1, "platform": "win_amd64", "source": self.source,
                       "submodules": self.submodules, "build_qualified": False, "source_closure_qualified": False}
        self.add_evidence("source-lock.json", source_lock, self.old_workspace + "/third_party/ifc-source-lock.json")
        prep = {"schema_version": 1, "build_qualified": False, "source_closure_qualified": False,
                "lock_sha256": self.get_candidate("provenance/source-lock.json")["sha256"],
                "source": {**self.source, "files": sorted(self.preparation_files, key=lambda r: r["path"])},
                "submodules": [{k: s[k] for k in ("path", "repository", "revision")} for s in self.submodules]}
        self.add_evidence("source-preparation.json", prep, self.old_workspace + "/.deps/ifc-source-preparation.json")
        self.add_evidence("build-recipe.ps1", b"# fixture portable recipe", self.old_workspace + "/scripts/build-ifc-source.ps1")
        self.add_evidence("source-derivation-verifier.py", b"# fixture verifier", self.old_workspace + "/scripts/prepare_ifc_derived_source.py")
        self.add_evidence("opaque-coordinate-output.i", b"patch", self.old_workspace + "/third_party/ifc-source/patches/opaque-coordinate-output.i")
        original = record("src/ifcwrap/utils/typemaps_out.i", b"original typemap")
        derived = record(original["path"], b"derived typemap")
        self.preparation_files.append(original)
        prep["source"]["files"] = sorted(self.preparation_files, key=lambda r: r["path"])
        prep_record = self.get_candidate("provenance/source-preparation.json")
        prep_record.update(self.put(self.candidate, prep_record["path"], prep))
        derivation = {"schema_version": 1, "build_qualified": False, "source_closure_qualified": False,
            "product_runtime_replaced": False, "source_revision": self.source["revision"],
            "workspace": self.old_workspace, "source_path": self.old_workspace + "/.deps/ifc-src",
            "output_path": self.build_root + "/source",
            "source_repository": self.source["repository"], "manifest_path": self.build_root + "/source-derivation.json",
            "inputs": {label: self.evidence_record(name) for label, name in (
                ("lock", "source-lock.json"), ("preparation", "source-preparation.json"),
                ("script", "source-derivation-verifier.py"), ("patch", "opaque-coordinate-output.i"))},
            "modified_file": {"path": original["path"], "original_bytes": original["bytes"],
                "original_sha256": original["sha256"], "derived_bytes": derived["bytes"],
                "derived_sha256": derived["sha256"]},
            "files": sorted([derived if r["path"] == original["path"] else copy.deepcopy(r)
                             for r in self.preparation_files], key=lambda r: r["path"])}
        self.add_evidence("source-derivation.json", derivation, self.build_root + "/source-derivation.json")
        self.build = {"schema_version": 1, "state": "built-unqualified", "configuration": "Release",
            "build_qualified": False, "source_closure_qualified": False, "product_runtime_replaced": False,
            "workspace": self.old_workspace, "build_root": self.build_root,
            "source_derivation": self.build_root + "/source-derivation.json",
            "inputs": [self.evidence_record(n) for n in ("source-lock.json", "source-preparation.json",
                "source-derivation.json", "build-recipe.ps1", "source-derivation-verifier.py", "opaque-coordinate-output.i")],
            "outputs": [record(self.native_origin, self.native_data)],
            "commands": [], "configure_arguments": []}
        self.make_build_contract()
        self.add_evidence("build-evidence.json", self.build, self.build_root + "/build-evidence.json")
        self.add_candidate("NOTICES.txt", b"unqualified local candidate notice", {"kind": "generated-stage-notice"})
        self.manifest = {"schema_version": 1, "purpose": "isolated-source-built-python-import-candidate",
            "platform": "win_amd64", "python_abi": "cp313", "configuration": "Release",
            "schemas": composer.SCHEMAS, "source": self.source, "submodules": self.submodules,
            **{f: False for f in composer.CANDIDATE_FLAGS},
            "build_evidence_sha256": self.get_candidate("provenance/build-evidence.json")["sha256"],
            "source_derivation_sha256": self.get_candidate("provenance/source-derivation.json")["sha256"],
            "files": sorted(self.candidate_files, key=lambda r: r["path"])}
        self.save_manifest()
        for constant, value in (("EXPECTED_CANDIDATE_FILES", len(self.candidate_files)),
                                ("EXTENSION_SHA256", hashlib.sha256(self.native_data).hexdigest()),
                                ("WRAPPER_SHA256", hashlib.sha256(self.wrapper_data).hexdigest())):
            patch = mock.patch.object(composer, constant, value)
            patch.start()
            self.addCleanup(patch.stop)

    def put(self, root, name, data):
        if isinstance(data, dict):
            data = (json.dumps(data, sort_keys=True) + "\n").encode()
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return record(name, data)

    def add_candidate(self, name, data, origin):
        self.candidate_files.append({**self.put(self.candidate, name, data), "origin": origin})

    def add_evidence(self, name, data, path):
        self.add_candidate("provenance/" + name, data, {"kind": "local-evidence", "path": path})

    def get_candidate(self, name):
        return next(r for r in self.candidate_files if r["path"] == name)

    def evidence_record(self, name):
        r = self.get_candidate("provenance/" + name)
        return {"path": r["origin"]["path"], "bytes": r["bytes"], "sha256": r["sha256"]}

    def save_manifest(self):
        self.put(self.candidate, "candidate-manifest.json", self.manifest)

    def make_build_contract(self):
        roots = {name: self.old_workspace + ("/" + suffix if suffix else "")
                 for name, suffix in composer.WORKSPACE_ROOTS.items()}
        roots.update(build=self.build_root, derived_source=self.build_root + "/source",
                     upstream_source=self.old_workspace + "/.deps/ifc-src",
                     visual_studio=self.visual_studio, git_tool=self.git_root)
        self.private_roots = roots
        options = {**composer.SCALAR_CMAKE_OPTIONS,
            **{name: ";".join(roots[root] + ("/" + path if path else "") for root, path in values)
               for name, values in composer.PATH_CMAKE_OPTIONS.items()}}
        self.build["configure_arguments"] = ["-S", roots["derived_source"] + "/cmake", "-B",
            roots["build"] + "/build", "-G", "Visual Studio 17 2022", "-A", "x64"] + [
                "-D" + name + "=" + value for name, value in options.items()]
        cmake = roots["visual_studio"] + "/" + composer.CMAKE_TOOL_PATH
        python = roots["python"] + "/python.exe"
        self.build["commands"] = [{"name": name, "exit_code": 0, "executable": executable,
                                   "arguments": arguments} for name, executable, arguments in (
            ("configure", cmake, self.build["configure_arguments"]),
            ("build-release", cmake, ["--build", roots["build"] + "/build", "--config", "Release",
                                     "--target", "ifcopenshell_wrapper", "--parallel", "2"]),
            ("source-derive", python, ["-I", "-B", roots["workspace"] + "/scripts/prepare_ifc_derived_source.py",
                "--workspace", roots["workspace"], "--output", roots["derived_source"],
                "--manifest", roots["build"] + "/source-derivation.json"]),
            ("vcpkg-head", roots["git_tool"] + "/git.exe", ["rev-parse", "HEAD"]))]
        by_origin = {r["origin"].get("path"): r for r in self.candidate_files if r["path"].startswith("provenance/")}
        self.build["inputs"] = []
        for root, path, role in composer.INPUT_LAYOUT:
            origin = roots[root] + "/" + path
            if origin in by_origin:
                r = by_origin[origin]
                data = (self.candidate / r["path"]).read_bytes()
            elif root == "python":
                data = (self.base / path).read_bytes()
            else:
                data = ("fixture " + role + " " + path).encode()
            self.build["inputs"].append(record(origin, data))
            if root in composer.WORKSPACE_ROOTS:
                suffix = composer.WORKSPACE_ROOTS[root]
                self.put(self.root, (suffix + "/" if suffix else "") + path, data)

    def save_build(self):
        r = self.get_candidate("provenance/build-evidence.json")
        r.update(self.put(self.candidate, r["path"], self.build))
        self.manifest["build_evidence_sha256"] = r["sha256"]
        self.save_manifest()

    def run_compose(self):
        return composer.compose(self.root, self.base, self.candidate, self.out)

    def refuse(self):
        with self.assertRaises((ValueError, OSError)):
            self.run_compose()
        self.assertFalse(self.out.exists())

    def test_replace_entire_ifc_keep_baseline_and_notice_bytes(self):
        original = (self.base / "runtime-manifest.json").read_bytes()
        result = self.run_compose()
        self.assertEqual(result["kind"], "controlled_cad_runtime")
        self.assertEqual(result["ifcopenshell"]["upstream_version_informative_only"], "0.0.0")
        self.assertNotIn("ifcopenshell", result["library_versions"])
        self.assertFalse((self.out / "Lib/site-packages/ifcopenshell/old-only.py").exists())
        self.assertFalse((self.out / "Lib/site-packages/ifcopenshell-0.8.3.post2.dist-info").exists())
        self.assertEqual((self.out / "Lib/site-packages/ezdxf/__init__.py").read_bytes(), b"ezdxf exact")
        for r in self.candidate_files:
            if r["path"].startswith("licenses/") or r["path"] == "NOTICES.txt":
                self.assertEqual((self.out / "notices/ifcopenshell" / r["path"]).read_bytes(),
                                 (self.candidate / r["path"]).read_bytes())
            elif r["path"].startswith("provenance/"):
                self.assertFalse((self.out / "notices/ifcopenshell" / r["path"]).exists())
        self.assertEqual((self.base / "runtime-manifest.json").read_bytes(), original)
        self.assertTrue((self.base / "Lib/site-packages/ifcopenshell/old-only.py").exists())
        for field in composer.RESULT_FLAGS:
            self.assertIs(result[field], False)
        self.assertNotIn("C:/private", json.dumps(result))
        for path in (self.out / "receipts").glob("*.json"):
            self.assertNotIn("C:/private", path.read_text())
        build_receipt = json.loads((self.out / "receipts/build.json").read_text())
        self.assertEqual(build_receipt["original_build_input_count"], len(self.build["inputs"]))
        self.assertEqual(len(build_receipt["all_original_build_input_receipts"]), len(self.build["inputs"]))
        provenance_bindings = {row["name"]: composer.checked_record(row)
                               for row in build_receipt["bound_provenance_inputs"]}
        self.assertEqual(set(provenance_bindings), set(composer.PROVENANCE_NAMES))
        self.assertEqual(provenance_bindings["build-evidence.json"], build_receipt["original_receipt"])
        self.assertEqual(build_receipt["configure_arguments_receipt"]["encoding"], "canonical-json-object-arguments")
        self.assertEqual(result["files"], sorted(result["files"], key=lambda r: r["path"]))
        self.assertEqual(json.loads((self.out / composer.MANIFEST).read_text()), result)

    def test_base_tamper_even_in_removed_ifc_refused(self):
        self.put(self.base, "Lib/site-packages/ifcopenshell/old-only.py", b"tampered")
        self.refuse()

    def test_base_extra_file_refused(self):
        self.put(self.base, "extra.txt", b"extra")
        self.refuse()

    def test_wrong_lock_digest_refused(self):
        self.base_manifest["lock_sha256"] = "0" * 64
        self.put(self.base, "runtime-manifest.json", self.base_manifest)
        self.refuse()

    def test_candidate_tamper_refused(self):
        self.put(self.candidate, "ifcopenshell/ifcopenshell_wrapper.py", b"stale")
        self.refuse()

    def test_self_consistent_stale_wrapper_refused(self):
        r = self.get_candidate("ifcopenshell/ifcopenshell_wrapper.py")
        r.update(self.put(self.candidate, r["path"], b"stale"))
        self.save_manifest()
        self.refuse()

    def test_self_consistent_stale_extension_refused(self):
        r = self.get_candidate("ifcopenshell/" + composer.EXTENSION_NAME)
        r.update(self.put(self.candidate, r["path"], b"stale"))
        self.save_manifest()
        self.refuse()

    def test_candidate_extra_file_or_empty_directory_refused(self):
        self.put(self.candidate, "surprise.txt", b"extra")
        self.refuse()
        (self.candidate / "surprise.txt").unlink()
        (self.candidate / "empty").mkdir()
        self.refuse()

    def test_candidate_receipt_rejects_unknown_keys_and_qualified_state(self):
        self.manifest["invented"] = True
        self.save_manifest()
        self.refuse()
        del self.manifest["invented"]
        self.manifest["build_qualified"] = True
        self.save_manifest()
        self.refuse()

    def test_candidate_case_directory_collision_refused(self):
        self.manifest["files"].append({**record("IfcOpenShell/extra.py", b"x"),
                                      "origin": {"kind": "generated-stage-notice"}})
        self.save_manifest()
        self.refuse()

    def test_rejects_unsafe_paths(self):
        for path in ("../escape", "A/../escape", "C:/escape", "A\\x", "A//x", "a/CON.txt",
                     "a/x.", "a/x ", "a/x\x7f", "a/.git/x", "a/__pycache__/x"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                composer.relative_path(path)

    def test_build_output_disagreement_refused(self):
        self.build["outputs"][0]["sha256"] = "0" * 64
        r = self.get_candidate("provenance/build-evidence.json")
        r.update(self.put(self.candidate, r["path"], self.build))
        self.manifest["build_evidence_sha256"] = r["sha256"]
        self.save_manifest()
        self.refuse()

    def test_no_version_spoofing(self):
        self.put(self.candidate, "ifcopenshell/__init__.py", b'__version__ = "0.8.3.post2"\n')
        self.refuse()

    def test_existing_target_empty_or_nonempty_preserved(self):
        self.out.mkdir()
        with self.assertRaises(ValueError):
            self.run_compose()
        self.assertTrue(self.out.is_dir())
        self.put(self.out, "keep.txt", b"prior target")
        with self.assertRaises(ValueError):
            self.run_compose()
        self.assertEqual((self.out / "keep.txt").read_bytes(), b"prior target")

    def test_late_input_drift_refused_before_publication(self):
        original = composer.copy_verified
        changed = False
        def copy_and_drift(source, destination, evidence):
            nonlocal changed
            original(source, destination, evidence)
            if not changed:
                changed = True
                self.put(self.base, "python.exe", b"late drift")
        with mock.patch.object(composer, "copy_verified", side_effect=copy_and_drift):
            self.refuse()

    def test_late_output_drift_refused(self):
        original = composer.copy_verified
        def copy_and_tamper(source, destination, evidence):
            original(source, destination, evidence)
            destination.write_bytes(b"tampered output")
        with mock.patch.object(composer, "copy_verified", side_effect=copy_and_tamper):
            self.refuse()

    def test_late_target_creation_preserved(self):
        original = composer.copy_verified
        def copy_and_reserve(source, destination, evidence):
            original(source, destination, evidence)
            if not self.out.exists():
                self.out.mkdir()
                self.put(self.out, "foreign.txt", b"foreign")
        with mock.patch.object(composer, "copy_verified", side_effect=copy_and_reserve):
            with self.assertRaises(ValueError):
                self.run_compose()
        self.assertEqual((self.out / "foreign.txt").read_bytes(), b"foreign")

    def test_output_must_be_exact_separate_sibling(self):
        for output in (self.base, self.root / ".deps/cad-runtime/elsewhere", self.candidate / "out"):
            with self.subTest(output=output), self.assertRaises(ValueError):
                composer.compose(self.root, self.base, self.candidate, output)

    def test_duplicate_json_key_refused(self):
        self.put(self.candidate, "candidate-manifest.json", b'{"schema_version":1,"schema_version":1}')
        self.refuse()

    def test_hardlinked_candidate_file_refused(self):
        import os
        alias = Path(self.temp.name) / "notice-alias"
        os.link(self.candidate / "NOTICES.txt", alias)
        self.refuse()

    def test_aggregate_file_bound_refused(self):
        with mock.patch.object(composer, "MAX_TOTAL_BYTES", 1):
            self.refuse()

    def test_receipt_path_collision_refused(self):
        self.put(self.base, "receipts/build.json", b"foreign receipt")
        self.base_manifest["files"].append(record("receipts/build.json", b"foreign receipt"))
        self.base_manifest["files"].sort(key=lambda r: r["path"])
        self.put(self.base, "runtime-manifest.json", self.base_manifest)
        self.refuse()

    def test_native_rename_refusal_preserves_foreign_target(self):
        def refuse_rename(stage, target):
            target.mkdir()
            self.put(target, "foreign.txt", b"foreign")
            raise FileExistsError("foreign target")
        with mock.patch.object(composer.os, "rename", side_effect=refuse_rename):
            with self.assertRaises(FileExistsError):
                self.run_compose()
        self.assertEqual((self.out / "foreign.txt").read_bytes(), b"foreign")

    def test_duplicate_config_override_refused(self):
        self.build["configure_arguments"].append("-DSCHEMA_VERSIONS=other")
        r = self.get_candidate("provenance/build-evidence.json")
        r.update(self.put(self.candidate, r["path"], self.build))
        self.manifest["build_evidence_sha256"] = r["sha256"]
        self.save_manifest()
        self.refuse()

    def test_omitted_prepared_source_refused(self):
        preparation = json.loads((self.candidate / "provenance/source-preparation.json").read_text())
        missing = record(composer.PACKAGE_SOURCE + "/missing.py", b"missing source")
        preparation["source"]["files"].append(missing)
        preparation["source"]["files"].sort(key=lambda r: r["path"])
        prep = self.get_candidate("provenance/source-preparation.json")
        prep.update(self.put(self.candidate, prep["path"], preparation))
        derivation = json.loads((self.candidate / "provenance/source-derivation.json").read_text())
        derivation["files"].append(missing)
        derivation["files"].sort(key=lambda r: r["path"])
        derivation["inputs"]["preparation"] = self.evidence_record("source-preparation.json")
        deriv = self.get_candidate("provenance/source-derivation.json")
        deriv.update(self.put(self.candidate, deriv["path"], derivation))
        self.build["inputs"] = [self.evidence_record(n) for n in ("source-lock.json", "source-preparation.json",
            "source-derivation.json", "build-recipe.ps1", "source-derivation-verifier.py", "opaque-coordinate-output.i")]
        build = self.get_candidate("provenance/build-evidence.json")
        build.update(self.put(self.candidate, build["path"], self.build))
        self.manifest["build_evidence_sha256"] = build["sha256"]
        self.manifest["source_derivation_sha256"] = deriv["sha256"]
        self.save_manifest()
        self.refuse()

    def test_controlled_manifest_self_collision_refused(self):
        r = record(composer.MANIFEST, b"foreign manifest")
        self.put(self.base, r["path"], b"foreign manifest")
        self.base_manifest["files"].append(r)
        self.base_manifest["files"].sort(key=lambda r: r["path"])
        self.put(self.base, "runtime-manifest.json", self.base_manifest)
        self.refuse()

    def test_receipt_selected_text_cannot_emit_machine_paths(self):
        self.build["commands"].append({"name": "C:/private/secret", "exit_code": 0})
        r = self.get_candidate("provenance/build-evidence.json")
        r.update(self.put(self.candidate, r["path"], self.build))
        self.manifest["build_evidence_sha256"] = r["sha256"]
        self.save_manifest()
        self.refuse()

    def test_current_workspace_recipe_drift_refused(self):
        self.put(self.root, "scripts/build-ifc-source.ps1", b"changed recipe")
        self.refuse()

    def test_current_manifest_and_checker_drift_refused(self):
        for path in ("scripts/prepare_ifc_source.py", "scripts/prepare_ifc_derived_source.py",
                     "third_party/ifc-source/vcpkg.json", "third_party/ifc-source/support/vcpkg.json",
                     "third_party/ifc-source/triplets/x64-windows-ifc-static.cmake"):
            original = (self.root / path).read_bytes()
            with self.subTest(path=path):
                self.put(self.root, path, b"drift")
                self.refuse()
                self.put(self.root, path, original)

    def test_current_python_must_match_historical_inputs_even_with_new_base_receipt(self):
        self.put(self.base, "python.exe", b"changed python")
        next(r for r in self.base_manifest["files"] if r["path"] == "python.exe").update(record("python.exe", b"changed python"))
        self.put(self.base, "runtime-manifest.json", self.base_manifest)
        self.refuse()

    def test_late_workspace_checker_drift_refused(self):
        original = composer.copy_verified
        def copy_and_drift(source, destination, evidence):
            original(source, destination, evidence)
            self.put(self.root, "scripts/prepare_ifc_source.py", b"late checker drift")
        with mock.patch.object(composer, "copy_verified", side_effect=copy_and_drift):
            self.refuse()

    def test_typed_split_and_unknown_cmake_overrides_refused(self):
        original = copy.deepcopy(self.build["configure_arguments"])
        for addition in (["-DBUILD_IFCPYTHON:BOOL=OFF"], ["-D", "BUILD_IFCPYTHON=OFF"],
                         ["-Dbuild_ifcpython=OFF"], ["-U", "BUILD_IFCPYTHON"],
                         ["-C", "C:/private/override.cmake"], ["--preset", "other"],
                         ["-DCMAKE_TOOLCHAIN_FILE=C:/private/toolchain.cmake"]):
            with self.subTest(addition=addition):
                self.build["configure_arguments"] = original + addition
                self.build["commands"][0]["arguments"] = self.build["configure_arguments"]
                self.save_build()
                self.refuse()

    def test_portable_recipe_config_and_all_input_identities_are_delivered(self):
        result = self.run_compose()
        build = json.loads((self.out / "receipts/build.json").read_text())
        self.assertEqual(len(build["inputs"]), 57)
        self.assertTrue(all(set(r) == {"root", "path", "role", "bytes", "sha256"} for r in build["inputs"]))
        self.assertIn("-S", build["configure_arguments"])
        self.assertIn("${derived_source}/cmake", build["configure_arguments"])
        self.assertIn("-DPYTHON_EXECUTABLE=${python}/python.exe", build["configure_arguments"])
        self.assertIn("-DCMAKE_PREFIX_PATH=${occt_sdk};${support_sdk}", build["configure_arguments"])
        for artifact in build["recipe_artifacts"]:
            self.assertEqual((self.out / artifact["path"]).read_bytes(),
                             (self.root / artifact["restore_path"]).read_bytes())
        source = json.loads((self.out / "receipts/source-preparation.json.receipt.json").read_text())
        derived = json.loads((self.out / "receipts/source-derivation.json").read_text())
        self.assertEqual(source["source_files"], sorted(self.preparation_files, key=lambda r: r["path"]))
        self.assertEqual(len(derived["derived_source_files"]), len(self.preparation_files))
        self.assertNotIn("C:/private", json.dumps(result))

    def test_unknown_build_input_identity_refused(self):
        self.build["inputs"][-1]["path"] = "C:/private/unrecognized-file"
        self.save_build()
        self.refuse()

    def test_foreign_substituted_stage_is_preserved(self):
        original = composer.copy_verified
        substituted = []
        def copy_and_substitute(source, destination, evidence):
            original(source, destination, evidence)
            if not substituted:
                stage = next(p for p in destination.parents if p.name.startswith(".controlled-cad-stage-"))
                stage.rename(stage.with_name(stage.name + "-saved"))
                stage.mkdir()
                self.put(stage, "foreign.txt", b"foreign stage")
                substituted.append(stage)
        with mock.patch.object(composer, "copy_verified", side_effect=copy_and_substitute):
            self.refuse()
        self.assertEqual((substituted[0] / "foreign.txt").read_bytes(), b"foreign stage")

    def test_supported_single_typed_split_cmake_form_has_portable_semantics(self):
        arguments = self.build["configure_arguments"]
        index = arguments.index("-DBUILD_IFCPYTHON=ON")
        arguments[index:index + 1] = ["-D", "BUILD_IFCPYTHON:BOOL=ON"]
        self.build["commands"][0]["arguments"] = arguments
        self.save_build()
        self.run_compose()
        build = json.loads((self.out / "receipts/build.json").read_text())
        self.assertIn("-DBUILD_IFCPYTHON=ON", build["configure_arguments"])

    def test_lowercase_cmake_variable_cannot_replace_actual_case_sensitive_control(self):
        arguments = self.build["configure_arguments"]
        arguments[arguments.index("-DBUILD_IFCPYTHON=ON")] = "-Dbuild_ifcpython=ON"
        self.build["commands"][0]["arguments"] = arguments
        self.save_build()
        self.refuse()

    def test_explicit_receipt_bound_external_sdk_roots_are_portable(self):
        changes = {"occt_sdk": "C:/generic/occt/x64-windows-ifc-static",
                   "support_sdk": "C:/generic/support/x64-windows-ifc-static",
                   "occt_store": "C:/generic/occt", "support_store": "C:/generic/support"}
        roots = {**self.private_roots, **changes}
        self.build["sdk_roots"] = {"kernel": roots["occt_sdk"], "support": roots["support_sdk"]}
        for name, components in composer.PATH_CMAKE_OPTIONS.items():
            index = next(i for i, argument in enumerate(self.build["configure_arguments"])
                         if argument.startswith("-D" + name + "="))
            self.build["configure_arguments"][index] = "-D" + name + "=" + ";".join(
                roots[root] + ("/" + path if path else "") for root, path in components)
        for (root, path, _), receipt in zip(composer.INPUT_LAYOUT, self.build["inputs"]):
            receipt["path"] = roots[root] + "/" + path
        self.build["commands"][0]["arguments"] = self.build["configure_arguments"]
        self.save_build()
        result = self.run_compose()
        build = json.loads((self.out / "receipts/build.json").read_text())
        self.assertIn("-KernelRoot", build["recipe_arguments"])
        self.assertIn("${occt_sdk}", build["recipe_arguments"])
        self.assertNotIn("C:/generic", json.dumps(result))
        self.assertNotIn("C:/generic", json.dumps(build))

    def test_selected_sdk_status_cannot_escape_controlled_sibling(self):
        self.build["sdk_roots"] = {"kernel": self.private_roots["occt_sdk"],
            "support": self.private_roots["support_sdk"]}
        next(r for r in self.build["inputs"] if r["path"].endswith("/.deps/ifc-kernel/vcpkg/status"))["path"] = "C:/foreign/vcpkg/status"
        self.save_build()
        self.refuse()

    def test_source_inventory_receipt_is_bound_to_original_and_copy_bytes(self):
        result = self.run_compose()
        for name, receipt in result["receipts"].items():
            path = self.out / receipt["path"]
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), receipt["sha256"])
            derivative = json.loads(path.read_text())
            self.assertEqual(derivative["original_receipt"], receipt["original_receipt"])
        recipe = self.out / "receipts/recipes/build-ifc-source.ps1"
        self.assertEqual(recipe.read_bytes(), (self.candidate / "provenance/build-recipe.ps1").read_bytes())

    def add_bound_wrapper_receipt(self):
        self.build["generated_wrapper"] = record(self.wrapper_origin, self.wrapper_data)
        self.get_candidate("ifcopenshell/ifcopenshell_wrapper.py")["origin"]["bound_in_original_build_evidence"] = True
        self.save_build()

    def test_bound_generated_wrapper_fact_and_original_receipt_are_preserved(self):
        self.add_bound_wrapper_receipt()
        result = self.run_compose()
        self.assertIs(result["ifcopenshell"]["generated_wrapper_bound_in_original_build_evidence"], True)
        wrapper = json.loads((self.out / "receipts/generated-wrapper.json").read_text())
        self.assertIs(wrapper["bound_in_original_build_evidence"], True)
        self.assertEqual(wrapper["original_build_generated_wrapper_receipt"]["sha256"],
                         self.build["generated_wrapper"]["sha256"])
        self.assertEqual(wrapper["original_build_evidence_receipt"],
                         result["receipts"]["build"]["original_receipt"])
        self.assertNotIn("C:/private", json.dumps(wrapper))
        self.assertNotIn("C:/private", json.dumps(result))
        for flag in composer.RESULT_FLAGS:
            self.assertIs(result[flag], False)

    def test_legacy_generated_wrapper_fact_remains_explicitly_false(self):
        result = self.run_compose()
        self.assertIs(result["ifcopenshell"]["generated_wrapper_bound_in_original_build_evidence"], False)
        wrapper = json.loads((self.out / "receipts/generated-wrapper.json").read_text())
        self.assertIs(wrapper["bound_in_original_build_evidence"], False)
        self.assertIsNone(wrapper["original_build_generated_wrapper_receipt"])

    def test_generated_wrapper_marker_cannot_claim_or_hide_original_binding(self):
        for field_present, marker in ((False, True), (True, False), (True, 1), (False, 0)):
            with self.subTest(field_present=field_present, marker=marker):
                if field_present:
                    self.build["generated_wrapper"] = record(self.wrapper_origin, self.wrapper_data)
                else:
                    self.build.pop("generated_wrapper", None)
                self.get_candidate("ifcopenshell/ifcopenshell_wrapper.py")["origin"]["bound_in_original_build_evidence"] = marker
                self.save_build()
                self.refuse()

    def test_generated_wrapper_original_receipt_mismatch_and_schema_refused(self):
        self.add_bound_wrapper_receipt()
        for fault in ("hash", "bytes", "path", "extra", "null"):
            with self.subTest(fault=fault):
                self.build["generated_wrapper"] = record(self.wrapper_origin, self.wrapper_data)
                if fault == "hash":
                    self.build["generated_wrapper"]["sha256"] = "0" * 64
                elif fault == "bytes":
                    self.build["generated_wrapper"]["bytes"] += 1
                elif fault == "path":
                    self.build["generated_wrapper"]["path"] = self.build_root + "/foreign/ifcopenshell_wrapper.py"
                elif fault == "extra":
                    self.build["generated_wrapper"]["claimed"] = True
                else:
                    self.build["generated_wrapper"] = None
                self.save_build()
                self.refuse()

    def test_bound_wrapper_current_byte_tamper_refused(self):
        self.add_bound_wrapper_receipt()
        self.put(self.candidate, "ifcopenshell/ifcopenshell_wrapper.py", b"tampered wrapper")
        self.refuse()


if __name__ == "__main__":
    unittest.main()
