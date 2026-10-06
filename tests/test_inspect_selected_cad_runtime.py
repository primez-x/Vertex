"""Small inert fixtures exercise identity admission, never native imports."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("inspector", ROOT / "scripts/inspect_selected_cad_runtime.py")
inspector = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(inspector)
c = inspector.receipts


class InspectorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.workspace = Path(self.temp.name) / "workspace"
        self.runtime = self.workspace / "runtime"
        self.runtime.mkdir(parents=True)
        self.lock = json.loads((ROOT / c.LOCK_RELATIVE).read_text())
        self.lock_receipt = c.checked_record(self.put(self.workspace, c.LOCK_RELATIVE, self.lock))
        self.files = [self.put(self.runtime, inspector.EXTENSION, b"inert native"),
                      self.put(self.runtime, inspector.WRAPPER, b"inert wrapper")]
        for key, record in (("EXTENSION_SHA256", self.files[0]), ("WRAPPER_SHA256", self.files[1])):
            patch = mock.patch.object(c, key, record["sha256"])
            patch.start()
            self.addCleanup(patch.stop)
        self.value = {"schema_version": 1, "lock_sha256": self.lock_receipt["sha256"],
            "python_version": "3.13.15", "library_versions": self.lock["library_versions"],
            "qualification": "incomplete", "production_worker_integrated": False}
        self.name = "runtime-manifest.json"

    def put(self, root, name, value):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(value if isinstance(value, bytes) else json.dumps(value).encode())
        return {"path": name, **c.digest(path)}

    def save(self):
        self.value["files"] = sorted(self.files, key=lambda r: r["path"])
        self.put(self.runtime, self.name, self.value)

    def inspect(self):
        self.save()
        return inspector.inspect(self.runtime, self.workspace)

    def controlled(self):
        self.name = c.MANIFEST
        source = {"path": ".deps/ifc-src", "repository": c.SOURCE_REPOSITORY, "revision": c.SOURCE_REVISION}
        modules = [{"path": p, "repository": u, "revision": r} for p, u, r in c.SUBMODULES]
        original = [{"path": "ifcopenshell/" + Path(r["path"]).name, **c.checked_record(r)} for r in self.files]
        provenance = {n: {"bytes": 1, "sha256": "a" * 64} for n in c.PROVENANCE_NAMES}
        original += [{"path": "provenance/" + n, **r} for n, r in provenance.items()]
        wrapper_original = {"root": "build", "path": "build/ifcwrap/ifcopenshell_wrapper.py",
                            **c.checked_record(self.files[1])}
        self.payloads = {
            "baseline": {"python_version": "3.13.15", "library_versions": self.lock["library_versions"],
                         "lock_receipt": self.lock_receipt},
            "candidate": {"source": source, "submodules": modules, "schemas": c.SCHEMAS,
                          "file_count": len(original), "original_files": sorted(original, key=lambda r: r["path"])},
            "build": {"native_output": self.files[0], "configuration": "Release", "schemas": c.SCHEMAS,
                      "generated_wrapper_bound_in_original_build_evidence": True,
                      "original_build_generated_wrapper_receipt": wrapper_original,
                      "bound_provenance_inputs": [{"name": n, **r} for n, r in provenance.items()]},
            "derivation": {"source": source, "submodules": modules},
            "generated_wrapper": {"path": inspector.WRAPPER, "bound_in_original_build_evidence": True,
                "bound_by_candidate_staging_receipt": True, "original_build_generated_wrapper_receipt": wrapper_original,
                "original_build_evidence_receipt": provenance["build-evidence.json"]}}
        self.payloads.update({n: {"evidence_name": n, "source": source,
            "content_role": "original_provenance_hash_only", "source_closure_qualified": False}
            for n in c.PROVENANCE_NAMES if n not in {"build-evidence.json", "source-derivation.json"}})
        self.refs = {}
        for key, payload in self.payloads.items():
            original_receipt = (c.checked_record(self.files[1]) if key == "generated_wrapper" else
                provenance[{"build": "build-evidence.json", "derivation": "source-derivation.json"}.get(key, key)]
                if key in {"build", "derivation", *c.PROVENANCE_NAMES} else {"bytes": 1, "sha256": "b" * 64})
            payload.update(schema_version=1, kind="portable_controlled_cad_receipt", original_receipt=original_receipt,
                           original_retained_privately=True, qualification_conferred=False)
        self.value = {"schema_version": 1, "kind": "controlled_cad_runtime", "platform": "win_amd64",
            "python_version": "3.13.15", "python_abi": "cp313", "ezdxf_version": "1.4.3",
            "library_versions": {k: v for k, v in self.lock["library_versions"].items() if k != "ifcopenshell"},
            "qualification": "incomplete", **{k: False for k in c.RESULT_FLAGS},
            "ifcopenshell": {"source": source, "submodules": modules, "schemas": c.SCHEMAS,
                "upstream_version_informative_only": "0.0.0", "wheel_identity_claimed": False,
                "extension": copy.deepcopy(self.files[0]), "generated_wrapper_bound_in_original_build_evidence": True}}
        self.files = [{**r, "origin": "source-built-ifcopenshell"} for r in self.files]
        self.refresh_receipts()

    def refresh_receipts(self):
        self.files = [r for r in self.files if r["origin"] != "portable-derived-receipt"]
        for key, payload in self.payloads.items():
            record = self.put(self.runtime, "receipts/" + key + ".json", payload)
            self.files.append({**record, "origin": "portable-derived-receipt"})
            self.refs[key] = {**record, "original_receipt": payload["original_receipt"]}
        self.value["receipts"] = self.refs

    def test_baseline_actual_byte_identity(self):
        result = self.inspect()
        self.assertEqual(result["kind"], "baseline")
        self.assertEqual(result["ifc_extension_sha256"], self.files[0]["sha256"])
        self.assertNotIn("source_revision", result)
        self.assertNotIn(str(self.workspace), json.dumps(result))

    def test_controlled_actual_byte_identity(self):
        self.controlled()
        self.assertEqual(self.inspect()["source_revision"], c.SOURCE_REVISION)

    def test_unknown_controlled_manifest_version_refused(self):
        self.controlled()
        for version in (True, "1", 2):
            with self.subTest(version=version):
                self.value["schema_version"] = version
                with self.assertRaises(ValueError):
                    self.inspect()

    def test_both_manifests_refused(self):
        self.save()
        self.put(self.runtime, c.MANIFEST, {})
        with self.assertRaises(ValueError):
            inspector.inspect(self.runtime, self.workspace)

    def test_duplicate_json_refused(self):
        self.put(self.runtime, self.name, b'{"files":[],"files":[]}')
        with self.assertRaises(ValueError):
            inspector.inspect(self.runtime, self.workspace)

    def test_baseline_lock_or_qualification_refused(self):
        for key, value in (("lock_sha256", "f" * 64), ("production_worker_integrated", True)):
            with self.subTest(key=key):
                original = self.value[key]
                self.value[key] = value
                with self.assertRaises(ValueError): self.inspect()
                self.value[key] = original

    def test_collision_and_stale_hash_refused(self):
        self.files.append({**self.files[1], "path": inspector.WRAPPER.upper()})
        with self.assertRaises(ValueError): self.inspect()
        self.files.pop()
        (self.runtime / inspector.WRAPPER).write_bytes(b"changed")
        with self.assertRaises(ValueError): self.inspect()

    def test_controlled_wheel_pin_and_flags_refused(self):
        self.controlled()
        for key, value in (("wheel_identity_claimed", True), ("upstream_version_informative_only", "0.8.3"),
                           ("generated_wrapper_bound_in_original_build_evidence", False)):
            old = self.value["ifcopenshell"][key]
            self.value["ifcopenshell"][key] = value
            with self.assertRaises(ValueError): self.inspect()
            self.value["ifcopenshell"][key] = old
        with mock.patch.object(c, "EXTENSION_SHA256", "f" * 64):
            with self.assertRaises(ValueError): self.inspect()
        self.value["production_qualified"] = True
        with self.assertRaises(ValueError): self.inspect()

    def test_self_consistent_unbound_provenance_refused(self):
        self.controlled()
        self.payloads["build"]["bound_provenance_inputs"][0]["sha256"] = "c" * 64
        self.refresh_receipts()
        with self.assertRaises(ValueError): self.inspect()

    def test_self_consistent_wrapper_evidence_refused(self):
        self.controlled()
        self.payloads["generated_wrapper"]["bound_in_original_build_evidence"] = False
        self.refresh_receipts()
        with self.assertRaises(ValueError): self.inspect()

    def test_stale_wheel_metadata_refused(self):
        self.controlled()
        self.files.append({**self.put(self.runtime, "Lib/site-packages/IfcOpenShell-0.0.0.egg-info/PKG-INFO", b"stale"),
                           "origin": "source-built-ifcopenshell"})
        with self.assertRaises(ValueError): self.inspect()

    def test_unlisted_file_and_hardlink_refused(self):
        extra = self.runtime / "extra"
        extra.write_bytes(b"unlisted")
        with self.assertRaises(ValueError): self.inspect()
        extra.unlink()
        os.link(self.runtime / inspector.WRAPPER, extra)
        with self.assertRaises(ValueError): self.inspect()

    def test_self_consistent_source_provenance_refused(self):
        self.controlled()
        self.payloads["source-lock.json"]["source"] = {"revision": "wrong"}
        self.refresh_receipts()
        with self.assertRaises(ValueError): self.inspect()


if __name__ == "__main__":
    unittest.main()
