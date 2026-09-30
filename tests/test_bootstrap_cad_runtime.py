import importlib.util
import io
import json
import pathlib
import sys
import tarfile
import tempfile
import unittest
import zipfile
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("bootstrap_cad_runtime", ROOT / "scripts/bootstrap_cad_runtime.py")
cad = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cad)


class CadRuntimeTests(unittest.TestCase):
    def fixture(self, root):
        cache = root / ".deps/downloads/cad-runtime"
        cache.mkdir(parents=True)
        assets = []
        def asset(name, kind, entries):
            path = cache / name
            with zipfile.ZipFile(path, "w") as archive:
                for filename, data in entries.items():
                    archive.writestr(filename, data)
            record = dict(name=name, version="3.13.15", kind=kind, filename=name,
                url="https://www.python.org/"+name, sha256=cad.digest(path))
            assets.append(record)
        asset("python.zip", "interpreter", {"python313._pth": "import site", "python313.zip": "stdlib"})
        path = cache / "headers.tar.xz"
        with tarfile.open(path, "w:xz") as archive:
            data = b"/* pinned public header */"
            item = tarfile.TarInfo("Python-3.13.15/Include/Python.h"); item.size = len(data)
            archive.addfile(item, io.BytesIO(data))
        assets.append(dict(name="headers", version="3.13.15", kind="headers", filename=path.name,
            url="https://www.python.org/headers.tar.xz", sha256=cad.digest(path)))
        asset("dev.zip", "development", {"tools/include/pyconfig.h": "Windows", "tools/libs/python313.lib": "ABI"})
        asset("ifcopenshell.whl", "wheel", {"ifcopenshell/__init__.py": "version='locked'"})
        asset("ezdxf.whl", "wheel", {"ezdxf/__init__.py": "__version__='locked'"})
        for item in assets:
            if item["kind"] == "wheel": item["name"] = item["filename"].removesuffix(".whl")
        lock = dict(schema_version=1, platform="win_amd64", python_version="3.13.15", python_abi="cp313",
            assets=assets, library_versions={"ifcopenshell": "3.13.15", "ezdxf": "3.13.15"})
        path = root / "lock.json"; path.write_text(json.dumps(lock))
        return path, root / ".deps/cad-runtime/fixture"

    def test_offline_roundtrip_and_poisoned_manifest_reject(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(cad.urllib.request, "urlopen", side_effect=AssertionError("network")):
            root = pathlib.Path(directory); lock, destination = self.fixture(root)
            first = cad.bootstrap(root, lock, destination, offline=True)
            self.assertEqual(first, cad.bootstrap(root, lock, destination, offline=True, check=True))
            self.assertNotIn("import site", (destination/"python313._pth").read_text())
            self.assertFalse(first["production_worker_integrated"])
            (destination/"Lib/site-packages/ezdxf/__init__.py").write_text("malicious")
            # Re-signing the file table cannot substitute for locked archive proof.
            forged = cad.inventory(destination, json.loads(lock.read_text()), cad.digest(lock))
            (destination/"runtime-manifest.json").write_text(json.dumps(forged))
            with self.assertRaisesRegex(ValueError, "verified archive contents"):
                cad.bootstrap(root, lock, destination, offline=True, check=True)
            self.assertEqual((destination/"Lib/site-packages/ezdxf/__init__.py").read_text(), "malicious")

    def test_cache_corruption_and_missing_archive_do_not_publish(self):
        for missing in (False, True):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory); lock, destination = self.fixture(root)
                cached = root/".deps/downloads/cad-runtime/python.zip"
                if missing: cached.unlink()
                else: cached.write_bytes(b"tampered")
                with patch.object(cad.urllib.request, "urlopen", side_effect=AssertionError("network")), self.assertRaises(ValueError):
                    cad.bootstrap(root, lock, destination, offline=True)
                self.assertFalse(destination.exists())

    def test_replacement_between_cache_check_and_extraction_rejects(self):
        with tempfile.TemporaryDirectory() as directory:
            root=pathlib.Path(directory); lock,destination=self.fixture(root)
            original=cad.obtain
            def replace_after_verification(asset,downloads,offline):
                path=original(asset,downloads,offline)
                if asset["kind"]=="interpreter": path.write_bytes(b"replaced")
                return path
            with patch.object(cad,"obtain",side_effect=replace_after_verification), self.assertRaisesRegex(ValueError,"Extraction bytes"):
                cad.bootstrap(root,lock,destination,offline=True)
            self.assertFalse(destination.exists())

    def test_interrupted_download_cleans_only_owned_scratch_and_can_retry(self):
        with tempfile.TemporaryDirectory() as directory:
            downloads=pathlib.Path(directory); saved=downloads/"archive.zip"; payload=b"expected"
            unrelated=downloads/"archive.zip.stale.partial"; unrelated.write_bytes(b"leave alone")
            asset=dict(filename=saved.name,url="https://www.python.org/archive.zip",sha256=cad.hashlib.sha256(payload).hexdigest())
            with patch.object(cad.urllib.request,"urlopen",side_effect=OSError("network failure")), self.assertRaises(OSError):
                cad.obtain(asset,downloads,False)
            self.assertEqual(list(downloads.iterdir()),[unrelated])
            with patch.object(cad.urllib.request,"urlopen",return_value=io.BytesIO(payload)):
                self.assertEqual(cad.obtain(asset,downloads,False).read_bytes(),payload)
            self.assertEqual(unrelated.read_bytes(),b"leave alone")

    def test_windows_paths_collisions_and_links_rejected_before_writes(self):
        for name in ("../escape", "C:/escape", "foo:stream", "CON.txt", "a./b", "a\\b", "./bad", "a//b", "a\x01b"):
            with self.subTest(name=name), self.assertRaises(ValueError): cad.member_path(name)
        with tempfile.TemporaryDirectory() as directory:
            root=pathlib.Path(directory); path=root/"input.zip"; target=root/"output"
            with zipfile.ZipFile(path,"w") as archive:
                archive.writestr("valid.txt","okay"); archive.writestr("VALID.txt","collision")
            with self.assertRaises(ValueError): cad.unpack_zip(path,target,set())
            self.assertFalse(target.exists())
            with zipfile.ZipFile(path,"w") as archive:
                link=zipfile.ZipInfo("link"); link.external_attr=(0o120777 << 16)
                archive.writestr(link,"../escape")
            with self.assertRaises(ValueError): cad.unpack_zip(path,target,set())

    def test_outside_destination_and_mismatched_abi_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root=pathlib.Path(directory); lock,destination=self.fixture(root)
            with self.assertRaises(ValueError): cad.bootstrap(root,lock,root/"outside",offline=True)
            data=json.loads(lock.read_text()); data["assets"][0]["version"]="3.12.9"; lock.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError,"match exactly"): cad.bootstrap(root,lock,destination,offline=True)

    @unittest.skipUnless(sys.platform == "win32", "Windows junction boundary")
    def test_real_windows_dependency_junction_cannot_redirect_bootstrap(self):
        import _winapi
        if not hasattr(_winapi,"CreateJunction"): self.skipTest("No junction helper")
        with tempfile.TemporaryDirectory() as directory:
            owned=pathlib.Path(directory); root=owned/"project"; root.mkdir()
            outside=owned/"outside"; outside.mkdir(); marker=outside/"sentinel"; marker.write_text("preserve")
            _winapi.CreateJunction(str(outside),str(root/".deps"))
            # Link rejection precedes lock reads and all cache/destination writes.
            with self.assertRaisesRegex(ValueError,"cross a link"):
                cad.bootstrap(root,root/"missing-lock.json",root/".deps/cad-runtime/runtime",offline=True)
            self.assertEqual(list(outside.iterdir()),[marker])


if __name__ == "__main__": unittest.main()
