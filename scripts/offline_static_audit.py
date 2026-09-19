#!/usr/bin/env python3
"""Audit static PE imports for a direct application network dependency.

The audit separates direct imports of the application entry points from
transitive imports carried by third-party runtime modules. A clean result is
static evidence only; it does not prove dynamic loading or a network-denied
runtime execution.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path, PureWindowsPath
import sys
from collections.abc import Mapping


# These modules provide network transports or URL/connection helpers. They
# are forbidden on the application entry points, but may appear in a bundled
# third-party module when the application does not invoke that capability.
NETWORK_IMPORTS = frozenset({
    "dnsapi.dll",
    "iphlpapi.dll",
    "mpr.dll",
    "netapi32.dll",
    "qt6network.dll",
    "urlmon.dll",
    "winhttp.dll",
    "wininet.dll",
    "ws2_32.dll",
    "wsock32.dll",
})
DEFAULT_APPLICATIONS = ("vertex.exe", "vertex-cli.exe", "vertex-planegcs.dll")


def _unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_import_report(path: Path) -> dict:
    if path.stat().st_size > 64 * 1024 * 1024:
        raise ValueError("PE import report exceeds 64 MiB")
    value = json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=_unique)
    if not isinstance(value, Mapping):
        raise ValueError("PE import report must be an object")
    modules = value.get("modules")
    entry_points = value.get("entry_points")
    if not isinstance(modules, list) or not modules:
        raise ValueError("PE import report modules must be a nonempty list")
    if not isinstance(entry_points, list) or not entry_points:
        raise ValueError("PE import report entry_points must be a nonempty list")
    return dict(value)


def _module_name(path: str) -> str:
    return PureWindowsPath(path).name.casefold()


def audit_report(report: Mapping, applications=DEFAULT_APPLICATIONS) -> dict:
    errors: list[str] = []
    modules = report.get("modules")
    entry_points = report.get("entry_points")
    if not isinstance(modules, list) or not modules:
        errors.append("modules must be a nonempty list")
        modules = []
    if not isinstance(entry_points, list) or not entry_points:
        errors.append("entry_points must be a nonempty list")
        entry_points = []

    by_name: dict[str, Mapping] = {}
    by_path: set[str] = set()
    transitive: set[str] = set()
    unresolved: list[str] = []
    for index, module in enumerate(modules):
        context = f"modules[{index}]"
        if not isinstance(module, Mapping):
            errors.append(f"{context} must be an object")
            continue
        raw_path = module.get("path")
        if not isinstance(raw_path, str) or not raw_path.strip():
            errors.append(f"{context}.path must be a nonempty string")
            continue
        path_key = raw_path.casefold()
        if path_key in by_path:
            errors.append(f"duplicate module path: {raw_path}")
        by_path.add(path_key)
        name = _module_name(raw_path)
        if name in by_name:
            errors.append(f"duplicate module basename: {name}")
        by_name[name] = module
        imports = module.get("imports")
        if not isinstance(imports, list):
            errors.append(f"{context}.imports must be a list")
            continue
        for import_index, imported in enumerate(imports):
            if not isinstance(imported, Mapping):
                errors.append(f"{context}.imports[{import_index}] must be an object")
                continue
            name_value = imported.get("name")
            kind = imported.get("kind")
            if name_value is not None and not isinstance(name_value, str):
                errors.append(f"{context}.imports[{import_index}].name must be a string")
            if kind == "unresolved":
                unresolved.append(f"{name} -> {name_value or '<unnamed>'}")
            if isinstance(name_value, str) and name_value.casefold() in NETWORK_IMPORTS:
                transitive.add(name_value.casefold())

    expected = tuple(dict.fromkeys(str(name).casefold() for name in applications))
    application_rows = []
    direct_forbidden: dict[str, list[str]] = {}
    for name in expected:
        module = by_name.get(name)
        if module is None:
            errors.append(f"application entry point is missing from import report: {name}")
            continue
        direct = []
        for imported in module.get("imports", []):
            if isinstance(imported, Mapping) and isinstance(imported.get("name"), str):
                if imported["name"].casefold() in NETWORK_IMPORTS:
                    direct.append(imported["name"].casefold())
        direct = sorted(set(direct))
        direct_forbidden[name] = direct
        if direct:
            errors.append(f"application entry point directly imports network library: {name}: {', '.join(direct)}")
        application_rows.append({
            "name": name,
            "path": str(module.get("path")),
            "direct_network_imports": direct,
            "import_count": len(module.get("imports", [])) if isinstance(module.get("imports"), list) else 0,
        })

    declared_entry_points = []
    for index, entry in enumerate(entry_points):
        if not isinstance(entry, str) or not entry.strip():
            errors.append(f"entry_points[{index}] must be a nonempty string")
            continue
        declared_entry_points.append(_module_name(entry))
    if len(declared_entry_points) != len(set(declared_entry_points)):
        errors.append("entry_points contain duplicate module names")
    declared_set = set(declared_entry_points)
    for name in expected:
        if name not in declared_set:
            errors.append(f"application entry point is not declared in import report: {name}")
    for name in sorted(declared_set):
        if name not in by_name:
            errors.append(f"declared entry point has no module record: {name}")
    if unresolved:
        errors.extend("unresolved static import: " + item for item in sorted(unresolved))

    return {
        "schema_version": "1.0",
        "audit_status": "pass" if not errors else "blocked",
        "static_network_audit_passed": not errors,
        "application_entry_points": application_rows,
        "network_import_policy": {
            "forbidden_direct_imports": sorted(NETWORK_IMPORTS),
            "transitive_network_imports_observed": sorted(transitive),
        },
        "declared_entry_points": sorted(set(declared_entry_points)),
        "errors": sorted(set(errors)),
        "boundary": (
            "Static direct-import review only; dynamic loading, network-denied execution, "
            "clean-machine behavior, and service/entitlement absence remain separate gates."
        ),
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--imports", type=Path, required=True,
                        help="release-imports.json produced by inspect-runtime.ps1")
    parser.add_argument("--application", action="append", dest="applications",
                        help="application module basename to audit (repeatable)")
    args = parser.parse_args(argv)
    try:
        report = audit_report(load_import_report(args.imports),
                              tuple(args.applications) if args.applications else DEFAULT_APPLICATIONS)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(json.dumps({"schema_version": "1.0", "audit_status": "blocked",
                          "static_network_audit_passed": False, "errors": [str(error)]},
                         indent=2, sort_keys=True))
        return 2
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["static_network_audit_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
