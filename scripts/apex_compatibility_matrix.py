"""Validate supplied Apex comparison evidence; never parse native formats or certify."""

import argparse
import hashlib
import json
import pathlib
import re
import sys

REQUIREMENTS = (
    "APX-COMPAT-001", "APX-COMPAT-002", "APX-NATIVE-AX5-001",
    "APX-NATIVE-AX7-001", "APX-NATIVE-LEGACY-001",
)
FIELDS = (
    "geometry", "curves", "classifications", "dimensions", "labels", "symbols",
    "imagery", "metadata", "calculations", "print_export",
)


def load_manifest(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate key: {key}")
            result[key] = value
        return result
    return json.loads(pathlib.Path(path).read_text(encoding="utf-8"), object_pairs_hook=unique)


def validate_and_build(manifest, *, root):
    """Return deterministic evidence checks. Even complete evidence is not an audit."""
    root = pathlib.Path(root).resolve()
    errors, comparisons = [], []

    def text(value, context):
        if not isinstance(value, str) or not value.strip():
            errors.append(f"{context}: nonempty string required")
            return False
        return True

    def obj(value, context):
        if not isinstance(value, dict):
            errors.append(f"{context}: object required")
            return {}
        return value

    def file(entry, context):
        entry = obj(entry, context)
        raw, expected = entry.get("path"), entry.get("sha256")
        if not text(raw, context + ".path"):
            return None
        win = pathlib.PureWindowsPath(raw)
        path = pathlib.PurePosixPath(raw.replace("\\", "/"))
        if win.drive or win.root or path.is_absolute() or ".." in path.parts or "\x00" in raw or ":" in raw:
            errors.append(f"{context}: unsafe path")
            return None
        selected = (root / path).resolve()
        if not selected.is_relative_to(root) or not selected.is_file():
            errors.append(f"{context}: missing file or path escapes evidence root")
            return None
        if not isinstance(expected, str) or not re.fullmatch(r"[a-fA-F0-9]{64}", expected):
            errors.append(f"{context}: sha256 required (64 hex digits)")
            return None
        digest = hashlib.sha256()
        with selected.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        actual = digest.hexdigest()
        if actual != expected.lower():
            errors.append(f"{context}: file hash differs from declared sha256")
        return {"path": path.as_posix(), "sha256": actual}

    manifest = obj(manifest, "manifest")
    if manifest.get("schema_version") != "1.0":
        errors.append("schema_version: expected 1.0")
    editions = obj(manifest.get("editions"), "editions")
    for edition in ("Standard", "Pro"):
        descriptor = obj(editions.get(edition), f"editions.{edition}")
        for key in ("version", "build", "settings", "observed_behavior"):
            text(descriptor.get(key), f"editions.{edition}.{key}")
        file(descriptor.get("inventory_evidence"), f"editions.{edition}.inventory_evidence")
        modules = descriptor.get("modules")
        if not isinstance(modules, list):
            errors.append(f"editions.{edition}.modules: explicit list required (empty means none observed)")
            continue
        names = set()
        for index, module in enumerate(modules):
            context = f"editions.{edition}.modules[{index}]"
            module = obj(module, context)
            name = module.get("name")
            if text(name, context + ".name"):
                if name in names:
                    errors.append(context + ": duplicate module name")
                names.add(name)
            if type(module.get("enabled")) is not bool:
                errors.append(context + ".enabled: boolean required")
            for key in ("version", "observed_behavior"):
                text(module.get(key), context + "." + key)
            file(module.get("evidence"), context + ".evidence")

    fixtures = manifest.get("fixtures")
    if not isinstance(fixtures, list) or not fixtures:
        errors.append("fixtures: nonempty list required")
        fixtures = []
    coverage, ids, edition_coverage = set(), set(), set()
    for index, fixture in enumerate(fixtures):
        context = f"fixtures[{index}]"
        fixture = obj(fixture, context)
        identity = fixture.get("id")
        if text(identity, context + ".id"):
            if identity in ids:
                errors.append(context + ": duplicate fixture id")
            ids.add(identity)
        requirement = fixture.get("requirement")
        if not isinstance(requirement, str) or requirement not in REQUIREMENTS:
            errors.append(context + ": unsupported requirement")
        else:
            coverage.add(requirement)
        edition = fixture.get("edition")
        if not isinstance(edition, str) or edition not in ("Standard", "Pro"):
            errors.append(context + ": explicit Standard or Pro edition required")
        else:
            edition_coverage.add(edition)
        module_names = fixture.get("modules")
        if not isinstance(module_names, list) or not all(isinstance(name, str) and name.strip() for name in module_names):
            errors.append(context + ".modules: explicit list of module names required")
        elif isinstance(edition, str) and isinstance(editions.get(edition), dict):
            declared = editions[edition].get("modules", [])
            enabled = {module.get("name") for module in declared
                       if isinstance(module, dict) and isinstance(module.get("name"), str)
                       and module.get("enabled") is True} if isinstance(declared, list) else set()
            for name in sorted(set(module_names) - enabled):
                errors.append(context + f": module not recorded as enabled: {name}")
        for key in ("provenance", "permissions", "native_version", "operation", "settings", "observed_behavior", "loss_report"):
            text(fixture.get(key), context + "." + key)
        required_version = {"APX-NATIVE-AX5-001": "v5", "APX-NATIVE-AX7-001": "v7"}.get(requirement) if isinstance(requirement, str) else None
        if required_version and fixture.get("native_version") != required_version:
            errors.append(context + f": native_version must be {required_version}")
        source = file(fixture.get("native_source"), context + ".native_source")
        replacement = file(fixture.get("replacement_project"), context + ".replacement_project")
        expected = file(fixture.get("expected_output"), context + ".expected_output")
        observed = file(fixture.get("observed_output"), context + ".observed_output")
        fields = obj(fixture.get("fields"), context + ".fields")
        for field in FIELDS:
            observation = obj(fields.get(field), context + ".fields." + field)
            for key in ("expected", "observed", "reason"):
                text(observation.get(key), context + ".fields." + field + "." + key)
            if observation.get("classification") not in ("match", "mismatch", "unsupported", "transformed", "not_applicable"):
                errors.append(context + ".fields." + field + ": explicit classification required")
        comparisons.append({
            "id": identity if isinstance(identity, str) else "", "requirement": requirement,
            "edition": edition, "native_source": source, "replacement_project": replacement,
            "expected_output": expected, "observed_output": observed,
            "output_hash_match": bool(expected and observed and expected["sha256"] == observed["sha256"]),
            "fields": fields, "observed_behavior": fixture.get("observed_behavior"),
            "loss_report": fixture.get("loss_report"),
            "metadata": {key: fixture.get(key) for key in
                         ("provenance", "permissions", "native_version", "operation", "settings", "modules")},
        })
    for requirement in sorted(set(REQUIREMENTS) - coverage):
        errors.append(f"missing fixture row: {requirement}")
    for edition in sorted({"Standard", "Pro"} - edition_coverage):
        errors.append(f"missing edition fixture: {edition}")
    return {
        "schema_version": "1.0", "audit_status": "incomplete", "compatibility_passed": False,
        "reason": "Evidence comparison only; no native parsing, independent behavior verification, or compatibility certification.",
        "evidence_complete": not errors, "errors": sorted(errors),
        "editions": editions,
        "comparisons": sorted(comparisons, key=lambda item: item["id"]),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=pathlib.Path)
    parser.add_argument("--root", type=pathlib.Path, required=True, help="Root containing explicitly selected evidence files")
    args = parser.parse_args(argv)
    try:
        report = validate_and_build(load_manifest(args.manifest), root=args.root)
    except (ValueError, OSError) as error:
        print(json.dumps({"audit_status": "incomplete", "evidence_complete": False,
                          "compatibility_passed": False, "errors": [str(error)]}, sort_keys=True))
        return 2
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["evidence_complete"] else 1


if __name__ == "__main__":
    sys.exit(main())
