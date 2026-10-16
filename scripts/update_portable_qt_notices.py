"""Refresh explicit portable Qt notice entries from the reviewed component catalog.

No SDK discovery or license selection is performed. Other allowlist entries are
preserved; every owner/notice pair is included at a stable destination.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path, PurePosixPath


def refresh(catalog: dict, allowlist: dict) -> dict:
    owners = {"qtbase", "qtpdf", "qtsvg"}
    components = {row["id"]: row for row in catalog["components"] if row["id"] in owners}
    if components.keys() != owners:
        raise ValueError("component catalog must declare qtbase, qtpdf and qtsvg")
    entries = [row for row in allowlist["entries"]
               if not (row["kind"] == "notice" and row["inventory_entry"] in owners)]
    destinations = {row["destination"].casefold() for row in entries}
    sources = set()
    for owner in sorted(owners):
        for path in components[owner]["notice_paths"]:
            relative = PurePosixPath(path)
            if relative.is_absolute() or ".." in relative.parts or ":" in path or "\\" in path:
                raise ValueError("Qt notice path must be a safe catalog-relative path")
            if path.startswith(".deps/qt-notices/"):
                destination = "licenses/qt-notices/" + path.removeprefix(".deps/qt-notices/")
            elif path.startswith(".deps/qt/") and "/sbom/" in path:
                destination = f"licenses/{owner}-{relative.name}"
            elif path.startswith("third_party/notices/"):
                destination = "licenses/" + path.removeprefix("third_party/notices/")
            else:
                raise ValueError(f"unreviewed Qt notice path mapping: {path}")
            # Staging requires explicit notice coverage for each component,
            # including notices shared by multiple Qt modules.
            if path in sources:
                destination = f"licenses/{owner}/" + destination.removeprefix("licenses/")
            sources.add(path)
            if destination.casefold() in destinations:
                raise ValueError(f"duplicate package destination: {destination}")
            destinations.add(destination.casefold())
            entries.append({"kind": "notice", "inventory_entry": owner,
                            "path": path, "destination": destination})
    return {**allowlist, "entries": entries}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    target = args.root / "packaging/portable-allowlist.json"
    current = json.loads(target.read_text(encoding="utf-8"))
    catalog = json.loads((args.root / "third_party/distribution-components.json").read_text(encoding="utf-8"))
    updated = refresh(catalog, current)
    if args.check:
        if current != updated:
            parser.exit(1, "Portable Qt notice entries need refreshing.\n")
    else:
        target.write_text(json.dumps(updated, indent=2) + "\n", encoding="utf-8")
    print(f"Portable allowlist: {len(updated['entries'])} entries; Qt notices match catalog")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
