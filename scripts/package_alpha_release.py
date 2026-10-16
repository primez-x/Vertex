"""Wrap a verified offline bundle in a tester-friendly portable alpha ZIP.

The payload retains its strict bundle manifest, source kit, dependency inputs,
and notices. Launcher and feedback files live outside that immutable payload.
This command neither installs nor publishes the release.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import tempfile
import zipfile

import stage_offline_bundle as bundle_tools


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def package(bundle: Path, output: Path, build_id: str, source_commit: str) -> Path:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.-]{0,79}", build_id):
        raise ValueError("build ID must contain only letters, digits, dots and hyphens")
    if not re.fullmatch(r"[0-9a-f]{40}", source_commit):
        raise ValueError("source commit must be a full lowercase Git SHA")
    bundle_tools.verify_bundle(bundle)
    manifest = json.loads((bundle / "offline-bundle-manifest.json").read_text(encoding="utf-8"))
    if not manifest.get("dependency_source_kit"):
        raise ValueError("alpha requires the matching dependency source kit")
    guide = bundle / "source-kit/docs/alpha-testing.md"
    if not guide.is_file():
        raise ValueError("alpha tester guide must be part of the verified source kit")
    name = f"Vertex-{build_id}-windows-x64"
    output.mkdir(parents=True, exist_ok=True)
    archive_path = output / (name + ".zip")
    checksum_path = output / (name + ".zip.sha256")
    if archive_path.exists() or checksum_path.exists():
        raise ValueError("release output already exists; choose a new build ID or output directory")
    launcher = ('@echo off\r\nsetlocal\r\n'
                'set "QT_PLUGIN_PATH="\r\nset "QT_QPA_PLATFORM_PLUGIN_PATH="\r\n'
                'set "QML_IMPORT_PATH="\r\nset "QML2_IMPORT_PATH="\r\n'
                'set "PATH=%SystemRoot%\\System32;%SystemRoot%"\r\n'
                'start "" /D "%~dp0payload\\bin" "%~dp0payload\\bin\\vertex.exe" %*\r\n')
    verify = ('@echo off\r\n'
              'powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass '
              '-File "%~dp0payload\\verify-offline-bundle.ps1" -Root "%~dp0payload" '
              '-ManifestName runtime-manifest.json\r\n'
              'if errorlevel 1 (echo Package verification failed.) else (echo Package verification passed.)\r\n'
              'pause\r\n')
    feedback = (f'Vertex alpha feedback\nBuild ID: {build_id}\nSource commit: {source_commit}\n'
                'Windows version (Settings > System > About):\n'
                'Steps to reproduce:\n  1.\n  2.\n  3.\n'
                'Expected result:\nActual result:\nHow often (always / sometimes / once):\n'
                'Screenshot (optional):\nProject file (optional):\n\n'
                'Send one report per issue through the channel provided by the project owner.\n'
                'Remove personal or client data from attachments before sharing.\n')
    extras = {"Start-Vertex.cmd": launcher.encode("utf-8"),
              "Verify-Package.cmd": verify.encode("utf-8"),
              "START-HERE.txt": guide.read_bytes(), "FEEDBACK.txt": feedback.encode("utf-8")}
    files = [("payload/" + row["path"], bundle / row["path"])
             for row in manifest["files"] if row.get("install") is True]
    files.extend(("payload/" + path, bundle / path)
                 for path in ("runtime-manifest.json", "verify-offline-bundle.ps1"))
    # Source trees can have long paths. Keep the complete verified source and
    # setup bundle compressed so the normal Explorer extraction needs only
    # short runtime paths. It is not necessary to extract this to run Vertex.
    with tempfile.TemporaryDirectory(prefix=".vertex-alpha-", dir=output) as temporary:
        source_archive = Path(temporary) / "SOURCE-BUNDLE.zip"
        with zipfile.ZipFile(source_archive, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            for row in manifest["files"]:
                archive.write(bundle / row["path"], row["path"])
            archive.write(bundle / "offline-bundle-manifest.json", "offline-bundle-manifest.json")
        files.append(("SOURCE-BUNDLE.zip", source_archive))
        _write_archive(archive_path, name, build_id, source_commit, files, extras)
    # Recheck the input after compression, then CRC-check the actual archive.
    bundle_tools.verify_bundle(bundle)
    with zipfile.ZipFile(archive_path) as archive:
        bad = archive.testzip()
        if bad:
            raise ValueError(f"archive CRC failed: {bad}")
    with checksum_path.open("x", encoding="ascii") as stream:
        stream.write(f"{digest(archive_path)}  {archive_path.name}\n")
    return archive_path


def _write_archive(archive_path, name, build_id, source_commit, files, extras):
    records = [{"path": path, "sha256": digest(source), "size": source.stat().st_size}
               for path, source in files]
    records.extend({"path": path, "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}
                   for path, data in extras.items())
    release = {"schema_version": 1, "build_id": build_id, "source_commit": source_commit,
               "platform": "windows-x64", "channel": "alpha", "signed": False,
               "production_qualified": False, "clean_machine_qualified": False,
               "files": sorted(records, key=lambda row: row["path"]),
               "boundary": "Portable alpha for feedback. Source and dependency inputs are included; production and distribution qualification remain incomplete."}
    extras["alpha-release.json"] = (json.dumps(release, indent=2, sort_keys=True) + "\n").encode("utf-8")
    # Exclusive creation prevents accidental replacement of a delivered build.
    with zipfile.ZipFile(archive_path, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for path, source in sorted(files):
            archive.write(source, name + "/" + path,
                          compress_type=zipfile.ZIP_STORED if path.endswith(".zip") else zipfile.ZIP_DEFLATED)
        for path, data in sorted(extras.items()):
            info = zipfile.ZipInfo(name + "/" + path, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--build-id", required=True)
    parser.add_argument("--source-commit", required=True)
    args = parser.parse_args()
    print(package(args.bundle, args.output, args.build_id, args.source_commit))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
