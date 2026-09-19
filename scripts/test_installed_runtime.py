#!/usr/bin/env python3
"""Observe a hidden installed Windows runtime with developer search paths removed.

This is a sampled developer-machine smoke check, not a clean-machine, network
isolation, registry-isolation, or production qualification test. No SDK paths
are added and no app is launched merely by importing this module. Each
workspace is exercised through a save/reopen pair so the evidence directory
contains the source and reopened .bldproj artifacts as well as screenshots.
The architectural workspace is captured once for each supported market profile
(residential and light-commercial); the measurement workspace uses the
residential profile.
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import struct
import subprocess
import sys
import threading
import time
import uuid

try:
    from performance_report import performance_evidence
except ModuleNotFoundError:
    # Also support file-based test discovery from outside the repository.
    _performance_spec = importlib.util.spec_from_file_location(
        "performance_report", Path(__file__).with_name("performance_report.py"))
    _performance_module = importlib.util.module_from_spec(_performance_spec)
    _performance_spec.loader.exec_module(_performance_module)
    performance_evidence = _performance_module.performance_evidence


REQUIRED_MODULES = {
    "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
    "vcruntime140.dll", "vcruntime140_1.dll", "qwindows.dll",
}
PROJECT_MAGIC = b"SQLite format 3\0"
MAX_CAPTURE_BYTES = 64 * 1024
TIMEOUT_SECONDS = 15.0


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def windows_key(path: str | Path) -> str:
    return str(PureWindowsPath(str(path))).casefold()


def validate_module_paths(observed: list[str], declared: dict[str, set[str]]) -> dict:
    """Portable path-only check. Non-packaged modules are explicitly unqualified."""
    required_seen: set[str] = set()
    packaged = []
    other = []
    errors = []
    for path in sorted(set(observed), key=windows_key):
        name = PureWindowsPath(path).name.casefold()
        if name not in declared:
            other.append(path)
            # Required names are never allowed to pass as unclaimed OS modules.
            if name in REQUIRED_MODULES:
                errors.append(f"required module not declared in runtime manifest: {path}")
            continue
        if windows_key(path) not in {windows_key(item) for item in declared[name]}:
            errors.append(f"packaged module loaded from an undeclared path: {path}")
            continue
        packaged.append(path)
        if name in REQUIRED_MODULES:
            required_seen.add(name)
    missing = sorted(REQUIRED_MODULES - required_seen)
    if missing:
        errors.append("required installed modules not observed: " + ", ".join(missing))
    return {"packaged_module_paths": packaged, "other_module_paths_unqualified": other,
            "required_modules_observed": sorted(required_seen), "errors": errors}


def isolated_environment(source: dict[str, str], windows: Path, private: Path) -> tuple[dict, dict]:
    removed = sorted(key for key in source if key.upper().startswith(("QT", "QML")))
    replaced = {"PATH", "APPDATA", "LOCALAPPDATA", "TEMP", "TMP"}
    env = {key: value for key, value in source.items()
           if key not in removed and key.upper() not in replaced}
    overrides = {
        "PATH": str(windows / "System32") + ";" + str(windows),
        "APPDATA": str(private / "Roaming"), "LOCALAPPDATA": str(private / "Local"),
        "TEMP": str(private / "Temp"), "TMP": str(private / "Temp"),
        "QT_QPA_PLATFORM": "windows", "QT_SCALE_FACTOR": "1",
    }
    env.update(overrides)
    return env, {"overrides": overrides, "removed_variable_names": removed,
                 "other_environment_inherited": True, "registry_isolated": False}


def load_runtime_manifest(root: Path) -> tuple[dict, dict[str, set[str]], dict[str, dict]]:
    path = root / "runtime-manifest.json"
    if path.stat().st_size > 16 * 1024 * 1024:
        raise ValueError("runtime manifest exceeds 16 MiB")
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    if value.get("manifest_kind") != "runtime" or not isinstance(value.get("files"), list):
        raise ValueError("invalid runtime manifest kind/files")
    declared: dict[str, set[str]] = {}
    records = {}
    for row in value["files"]:
        relative = row.get("path")
        if not isinstance(relative, str) or not relative or "\\" in relative or ":" in relative:
            raise ValueError("invalid installed manifest path")
        parts = PurePosixPath(relative).parts
        if relative.startswith("/") or any(part in (".", "..") for part in relative.split("/")):
            raise ValueError(f"unsafe installed manifest path: {relative}")
        target = root.joinpath(*parts).resolve(strict=True)
        if not target.is_relative_to(root) or not target.is_file():
            raise ValueError(f"manifest file escapes install root or is not a file: {relative}")
        key = windows_key(target)
        if key in records:
            raise ValueError(f"duplicate installed manifest path: {relative}")
        digest = row.get("sha256")
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError(f"invalid manifest hash: {relative}")
        records[key] = {"path": target, "sha256": digest}
        declared.setdefault(target.name.casefold(), set()).add(str(target))
    missing = REQUIRED_MODULES - declared.keys()
    if missing:
        raise ValueError("runtime manifest does not declare: " + ", ".join(sorted(missing)))
    return value, declared, records


def png_evidence(path: Path) -> dict:
    with path.open("rb") as stream:
        header = stream.read(24)
    size = path.stat().st_size
    if size <= 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or header[8:16] != b"\0\0\0\rIHDR":
        raise ValueError(f"missing/non-PNG smoke output: {path}")
    width, height = struct.unpack(">II", header[16:24])
    if not width or not height:
        raise ValueError(f"zero-sized smoke PNG: {path}")
    return {"path": str(path), "bytes": size, "width": width, "height": height,
            "sha256": sha256_file(path), "validation": "PNG signature and IHDR dimensions only"}


def project_evidence(path: Path) -> dict:
    """Validate and fingerprint one persisted Vertex .bldproj artifact.

    The smoke harness intentionally checks only the stable SQLite container
    signature here.  The application itself proves semantic reopen by opening
    the source artifact in a second process; this helper records the bytes and
    hash for later production-evidence binding without pretending that a
    header check is a complete project validation.
    """
    with path.open("rb") as stream:
        header = stream.read(len(PROJECT_MAGIC))
    size = path.stat().st_size
    if size <= len(PROJECT_MAGIC) or header != PROJECT_MAGIC:
        raise ValueError(f"missing/non-SQLite Vertex project: {path}")
    return {"path": str(path), "bytes": size, "sha256": sha256_file(path),
            "validation": "SQLite header; semantic reopen performed by a subsequent smoke process"}


class WindowsModules:
    """Read modules through an owned query handle; never attach to other PIDs."""

    def __init__(self, pid: int):
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.psapi = ctypes.WinDLL("psapi", use_last_error=True)
        self.kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        self.kernel.OpenProcess.restype = wintypes.HANDLE
        self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel.CloseHandle.restype = wintypes.BOOL
        self.psapi.EnumProcessModulesEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE),
                                                   wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
        self.psapi.EnumProcessModulesEx.restype = wintypes.BOOL
        self.psapi.GetModuleFileNameExW.argtypes = [wintypes.HANDLE, wintypes.HMODULE,
                                                    wintypes.LPWSTR, wintypes.DWORD]
        self.psapi.GetModuleFileNameExW.restype = wintypes.DWORD
        self.handle = self.kernel.OpenProcess(0x0400 | 0x0010, False, pid)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())

    def close(self) -> None:
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None

    def snapshot(self) -> list[str]:
        capacity = 1024
        while capacity <= 8192:
            modules = (wintypes.HMODULE * capacity)()
            needed = wintypes.DWORD()
            if not self.psapi.EnumProcessModulesEx(self.handle, modules, ctypes.sizeof(modules),
                                                   ctypes.byref(needed), 0x03):
                raise ctypes.WinError(ctypes.get_last_error())
            if needed.value <= ctypes.sizeof(modules):
                break
            capacity = (needed.value + ctypes.sizeof(wintypes.HMODULE) - 1) // ctypes.sizeof(wintypes.HMODULE)
        else:
            raise RuntimeError("module enumeration exceeds 8192-module limit")
        result = []
        for module in modules[:needed.value // ctypes.sizeof(wintypes.HMODULE)]:
            buffer = ctypes.create_unicode_buffer(32768)
            length = self.psapi.GetModuleFileNameExW(self.handle, module, buffer, len(buffer))
            if not length or length >= len(buffer):
                raise ctypes.WinError(ctypes.get_last_error())
            result.append(buffer.value)
        return result


class BoundedCapture:
    def __init__(self, stream):
        self.stream = stream
        self.data = bytearray()
        self.total = 0
        self.thread = threading.Thread(target=self.drain, daemon=True)
        self.thread.start()

    def drain(self) -> None:
        try:
            while chunk := self.stream.read(4096):
                self.total += len(chunk)
                self.data.extend(chunk[:max(0, MAX_CAPTURE_BYTES - len(self.data))])
        finally:
            self.stream.close()

    def evidence(self) -> dict:
        self.thread.join(timeout=1.0)
        return {"text": self.data.decode("utf-8", errors="replace"),
                "bytes_seen": self.total, "capture_limit_bytes": MAX_CAPTURE_BYTES,
                "truncated": self.total > MAX_CAPTURE_BYTES, "drain_complete": not self.thread.is_alive()}


def _capture_suffix(capture_label: str | None) -> str:
    if capture_label is None or capture_label == "":
        return ""
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,31}", capture_label):
        raise ValueError(f"unsafe smoke capture label: {capture_label}")
    return "-" + capture_label


def _hashes(evidence: dict, key: str) -> list[str]:
    return [item.get("sha256") for item in evidence.get(key, [])
            if isinstance(item, dict) and isinstance(item.get("sha256"), str)]


def compare_output_evidence(source_run: dict, reopened_run: dict) -> dict:
    """Compare source and reopened smoke artifacts without hiding missing evidence."""
    source_project = source_run.get("project") or {}
    reopened_project = reopened_run.get("project") or {}
    project_hash_match = bool(source_project.get("sha256")) and (
        source_project.get("sha256") == reopened_project.get("sha256"))
    source_screenshots = _hashes(source_run, "screenshots")
    reopened_screenshots = _hashes(reopened_run, "screenshots")
    screenshot_hashes_match = bool(source_screenshots) and (
        source_screenshots == reopened_screenshots)
    source_models = _hashes(source_run, "native_3d")
    reopened_models = _hashes(reopened_run, "native_3d")
    native_3d_hashes_match = source_models == reopened_models and (
        bool(source_models) or not source_models and not reopened_models)
    # Selection and focus are presentation state, so a screenshot can change
    # across reopen even when the persisted document and native model output
    # are identical. Keep that diagnostic visible without failing the semantic
    # round-trip gate on a transient inspector highlight.
    stable_hashes_match = project_hash_match and native_3d_hashes_match
    return {
        "project_hash_match": project_hash_match,
        "screenshot_hashes_match": screenshot_hashes_match,
        "native_3d_hashes_match": native_3d_hashes_match,
        "stable_hashes_match": stable_hashes_match,
        "all_hashes_match": project_hash_match and screenshot_hashes_match and native_3d_hashes_match,
        "source_screenshot_hashes": source_screenshots,
        "reopened_screenshot_hashes": reopened_screenshots,
        "source_native_3d_hashes": source_models,
        "reopened_native_3d_hashes": reopened_models,
    }


def run_workspace(executable: Path, workspace: str, run_root: Path, env: dict,
                  declared: dict[str, set[str]], records: dict[str, dict], *,
                  market: str = "residential",
                  project_output: Path | None = None,
                  project_input: Path | None = None,
                  capture_label: str | None = None) -> dict:
    if market not in {"residential", "light-commercial"}:
        raise ValueError(f"unsupported smoke market: {market}")
    suffix = _capture_suffix(capture_label)
    image = run_root / f"{workspace}-{market}{suffix}.png"
    performance_path = run_root / f"{workspace}-{market}{suffix}-performance.json"
    outputs = [image]
    args = [str(executable), "--smoke", "--smoke-assistance-disabled", "--smoke-market", market,
            "--smoke-workspace", workspace, "--smoke-output", str(image),
            "--smoke-performance-output", str(performance_path)]
    if workspace == "architectural":
        model = run_root / f"{workspace}-{market}{suffix}-model.png"
        outputs.append(model)
        args.extend(["--smoke-3d-output", str(model)])
    if project_input is not None:
        args.extend(["--smoke-project-input", str(project_input)])
    if project_output is not None:
        args.extend(["--smoke-project-output", str(project_output)])
    result = {"workspace": workspace, "market": market, "arguments": args,
              "capture_label": capture_label,
              "timeout_seconds": TIMEOUT_SECONDS,
              "module_poll_interval_seconds": 0.02, "errors": [], "screenshots": [],
              "native_3d": [],
              "timed_out": False, "module_samples": 0, "module_sample_errors": [],
              "assistance_disabled_requested": True}
    process = None
    monitor = None
    captures = []
    seen: set[str] = set()
    start = time.monotonic()
    try:
        process = subprocess.Popen(args, cwd=executable.parent, env=env, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   creationflags=subprocess.CREATE_NO_WINDOW, shell=False)
        result["pid"] = process.pid
        captures = [BoundedCapture(process.stdout), BoundedCapture(process.stderr)]
        monitor = WindowsModules(process.pid)
        while process.poll() is None:
            if time.monotonic() - start >= TIMEOUT_SECONDS:
                result["timed_out"] = True
                result["errors"].append("task-owned smoke process exceeded 15 seconds")
                break
            try:
                seen.update(monitor.snapshot())
                result["module_samples"] += 1
            except OSError as error:
                # DLL lists can change during startup and shutdown. Keep bounded
                # diagnostic samples; absence of required modules still fails.
                if len(result["module_sample_errors"]) < 10:
                    result["module_sample_errors"].append(str(error))
            time.sleep(0.02)
    except Exception as error:
        result["errors"].append(f"{type(error).__name__}: {error}")
    finally:
        if monitor is not None:
            monitor.close()
        if process is not None:
            if process.poll() is None:
                process.kill()  # Only the child this invocation just launched.
            try:
                result["exit_code"] = process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                result["exit_code"] = None
                result["errors"].append("task-owned child did not exit after termination")
        result["elapsed_seconds"] = round(time.monotonic() - start, 3)
        for name, capture in zip(("stdout", "stderr"), captures):
            result[name] = capture.evidence()
            if not result[name]["drain_complete"]:
                result["errors"].append(f"{name} pipe did not close after child exit")
    if result.get("exit_code") != 0:
        result["errors"].append(f"smoke exit code was {result.get('exit_code')}")
    module_check = validate_module_paths(list(seen), declared)
    result["module_paths"] = sorted(seen, key=windows_key)
    result.update({key: value for key, value in module_check.items() if key != "errors"})
    result["errors"].extend(module_check["errors"])
    result["observed_packaged_hashes"] = []
    for path in module_check["packaged_module_paths"]:
        try:
            row = records[windows_key(path)]
            digest = sha256_file(row["path"])
            result["observed_packaged_hashes"].append({"path": path, "sha256": digest})
            if digest != row["sha256"]:
                result["errors"].append(f"observed packaged file does not match manifest hash: {path}")
        except OSError as error:
            result["errors"].append(str(error))
    for index, path in enumerate(outputs):
        try:
            evidence = png_evidence(path)
            result["screenshots"].append(evidence)
            if workspace == "architectural" and index == 1:
                result["native_3d"].append(evidence)
        except (OSError, ValueError) as error:
            result["errors"].append(str(error))
    if project_output is not None:
        try:
            result["project"] = project_evidence(project_output)
        except (OSError, ValueError) as error:
            result["errors"].append(str(error))
    if project_input is not None:
        result["project_input"] = str(project_input)
    try:
        result["performance"] = performance_evidence(performance_path)
    except (OSError, ValueError) as error:
        result["errors"].append(str(error))
    result["passed"] = not result["errors"]
    return result


def windows_directory() -> Path:
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetWindowsDirectoryW.argtypes = [wintypes.LPWSTR, wintypes.UINT]
    kernel.GetWindowsDirectoryW.restype = wintypes.UINT
    buffer = ctypes.create_unicode_buffer(32768)
    length = kernel.GetWindowsDirectoryW(buffer, len(buffer))
    if not length or length >= len(buffer):
        raise ctypes.WinError(ctypes.get_last_error())
    return Path(buffer.value)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install-root", type=Path, required=True)
    parser.add_argument("--evidence-root", type=Path, required=True)
    args = parser.parse_args(argv)
    # A new directory guarantees that old PNGs cannot satisfy a failed run.
    run_root = args.evidence_root.resolve() / ("run-" + time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8])
    run_root.mkdir(parents=True)
    report = {"schema_version": 2, "kind": "installed-runtime-smoke", "passed": False,
              "network_denied": False, "clean_machine": False, "production_qualified": False,
              "boundary": "Sampled installed-runtime developer-machine smoke; no network denial, clean-machine, registry isolation, or output-fidelity qualification.",
              "evidence_root": str(run_root), "runs": [], "errors": []}
    try:
        if os.name != "nt" or ctypes.sizeof(ctypes.c_void_p) != 8:
            raise RuntimeError("64-bit Windows Python is required for 64-bit process module inspection")
        install = args.install_root.resolve(strict=True)
        report["install_root"] = str(install)
        _, declared, records = load_runtime_manifest(install)
        executable = install / "bin" / "vertex.exe"
        row = records.get(windows_key(executable))
        if row is None:
            raise ValueError("installed executable is not declared at bin/vertex.exe")
        digest = sha256_file(executable)
        if digest != row["sha256"]:
            raise ValueError("installed executable hash does not match runtime manifest")
        report["executable"] = {"path": str(executable), "sha256": digest}
        report["runtime_manifest_sha256"] = sha256_file(install / "runtime-manifest.json")
        private = run_root / "private-profile"
        for child in ("Roaming", "Local", "Temp"):
            (private / child).mkdir(parents=True)
        env, limits = isolated_environment(dict(os.environ), windows_directory(), private)
        report["environment"] = limits
        report["cwd"] = str(executable.parent)
        for workspace in ("measurement", "architectural"):
            markets = ("residential", "light-commercial") if workspace == "architectural" else ("residential",)
            for market in markets:
                stem = f"{workspace}-{market}"
                source_project = run_root / f"{stem}-source.bldproj"
                reopened_project = run_root / f"{stem}-reopened.bldproj"
                source_run = run_workspace(executable, workspace, run_root, env, declared, records,
                                           market=market, project_output=source_project,
                                           capture_label="source")
                report["runs"].append(source_run)
                reopened_run = run_workspace(executable, workspace, run_root, env, declared, records,
                                             market=market, project_input=source_project,
                                             project_output=reopened_project,
                                             capture_label="reopened")
                report["runs"].append(reopened_run)
                comparison = compare_output_evidence(source_run, reopened_run)
                report.setdefault("projects", []).append({
                    "workspace": workspace,
                    "market": market,
                    "source": source_run.get("project"),
                    "reopened": reopened_run.get("project"),
                    "reopen_process_exit_code": reopened_run.get("exit_code"),
                    "output_comparison": comparison,
                })
                if not comparison["stable_hashes_match"]:
                    report["errors"].append(
                        f"{workspace}/{market} source and reopened stable output hashes differ")
        report["passed"] = all(run["passed"] for run in report["runs"])
        report["passed"] = report["passed"] and not report["errors"]
    except Exception as error:
        report["errors"].append(f"{type(error).__name__}: {error}")
    report_path = run_root / "report.json"
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "report": str(report_path)}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
