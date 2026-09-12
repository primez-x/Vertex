"""Validate the production contract and report evidence-backed release gaps."""

import argparse
import collections
import hashlib
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]


def source_fingerprint(root):
    digest = hashlib.sha256()
    paths = []
    for directory in ("include", "src", "tests", "scripts", "third_party", "assets", "cmake",
                      "docs", "packaging"):
        for path in (root / directory).rglob("*"):
            if (path.is_file() and "__pycache__" not in path.parts and path.suffix != ".pyc" and
                    path.relative_to(root).as_posix() != "docs/requirements/acceptance-evidence.json"):
                paths.append(path)
    paths.extend(root / name for name in ("CMakeLists.txt", "CMakePresets.json", "vcpkg.json", ".gitattributes") if (root / name).is_file())
    for path in sorted(paths):
        digest.update(path.relative_to(root).as_posix().encode("utf-8") + b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def validate_contract(ledger, gates):
    errors = []
    rows = ledger.get("requirements", [])
    ids = [row.get("id") for row in rows]
    if not ids or len(set(ids)) != len(ids) or any(not item for item in ids):
        errors.append("Requirement IDs must be nonempty and unique")
    mappings = gates.get("requirement_to_gate", {})
    declared = gates.get("gates", {})
    if set(mappings) != set(ids):
        errors.append("Every requirement must map to exactly one declared gate")
    if not declared or any(not gate.get("required") for gate in declared.values()):
        errors.append("All production gates must be required")
    if any(gate not in declared for gate in mappings.values()):
        errors.append("A requirement references an unknown gate")
    release_rule = gates.get("release_rule")
    if not isinstance(release_rule, dict):
        errors.append("A release_rule object is required")
    else:
        if release_rule.get("type") != "all_required_gates":
            errors.append("release_rule must use the all_required_gates policy")
        required_gate_ids = release_rule.get("required_gate_ids")
        declared_gate_ids = set(declared)
        valid_gate_id_list = (isinstance(required_gate_ids, list) and
                              all(isinstance(item, str) and item for item in required_gate_ids))
        if (not valid_gate_id_list or
                len(required_gate_ids) != len(set(required_gate_ids)) or
                set(required_gate_ids) != declared_gate_ids):
            errors.append("release_rule required_gate_ids must list every declared gate exactly once")
        for flag in ("alternatives_allowed", "parity_only_exit", "architecture_only_exit",
                     "internal_checkpoint_is_production"):
            if release_rule.get(flag) is not False:
                errors.append(f"release_rule {flag} must be false")
        if not isinstance(release_rule.get("pass_condition"), str) or not release_rule["pass_condition"].strip():
            errors.append("release_rule pass_condition must be a nonempty string")
    for identity, gate in declared.items():
        if "requirement_ids" in gate:
            members = gate["requirement_ids"]
            expected = {requirement for requirement, owner in mappings.items() if owner == identity}
            if len(members) != len(set(members)) or set(members) != expected:
                errors.append(f"{identity}: gate member list disagrees with the authoritative mapping")
    for row in rows:
        identity = row.get("id", "unknown")
        if row.get("implementation_status") not in {"not_started", "in_progress", "verified"}:
            errors.append(f"{identity}: unknown implementation status; scope cannot be deferred")
        if row.get("evidence_status") not in {"documented", "needs_evidence"}:
            errors.append(f"{identity}: unknown evidence status")
        if not row.get("requirement") or not row.get("acceptance"):
            errors.append(f"{identity}: requirement and acceptance test are required")
        if row.get("package") not in range(1, 10):
            errors.append(f"{identity}: invalid internal package")
        if row.get("evidence_status") == "needs_evidence" and not row.get("blocker"):
            errors.append(f"{identity}: missing external-evidence explanation")
    return errors


def release_gaps(ledger, gates, evidence, root):
    gaps = validate_contract(ledger, gates)
    root = root.resolve()
    current_source = source_fingerprint(root)
    for row in ledger["requirements"]:
        identity = row["id"]
        if row["implementation_status"] != "verified":
            gaps.append(f"{identity}: {row['implementation_status']}")
            continue
        if row.get("blocker") or row["evidence_status"] == "needs_evidence":
            gaps.append(f"{identity}: external evidence is still unresolved")
        record = evidence.get(identity, {})
        if record.get("source_tree_sha256") != current_source:
            gaps.append(f"{identity}: acceptance evidence does not match the current source and test tree")
        artifacts = record.get("artifacts", [])
        if record.get("result") != "pass" or not artifacts:
            gaps.append(f"{identity}: missing passing acceptance evidence")
            continue
        for artifact in artifacts:
            path = (root / artifact.get("path", "")).resolve()
            if not path.is_relative_to(root) or not path.is_file():
                gaps.append(f"{identity}: invalid evidence artifact path")
                continue
            with path.open("rb") as source:
                actual = hashlib.file_digest(source, "sha256").hexdigest()
            if actual != artifact.get("sha256"):
                gaps.append(f"{identity}: acceptance evidence changed or lacks a hash")
    return gaps


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", action="store_true", help="Fail unless every requirement has passing, unchanged acceptance evidence")
    parser.add_argument("--contract", action="store_true",
                        help="Validate the requirement and gate schema without evaluating release acceptance")
    parser.add_argument("--json", action="store_true", help="Print the complete machine-readable report")
    args = parser.parse_args()
    directory = ROOT / "docs/requirements"
    ledger = json.loads((directory / "apex-parity.json").read_text(encoding="utf-8"))
    gates = json.loads((directory / "production-gates.json").read_text(encoding="utf-8"))
    evidence_path = directory / "acceptance-evidence.json"
    evidence = json.loads(evidence_path.read_text(encoding="utf-8")) if evidence_path.exists() else {}
    errors = validate_contract(ledger, gates)
    gaps = release_gaps(ledger, gates, evidence, ROOT)
    counts = dict(collections.Counter(row["implementation_status"] for row in ledger["requirements"]))
    report = {"contract_errors": errors, "requirement_count": len(ledger["requirements"]),
              "source_tree_sha256": source_fingerprint(ROOT),
              "required_gate_count": len(gates["gates"]), "implementation_counts": counts,
              "production_accepted": not gaps, "release_gaps": gaps}
    if args.json:
        print(json.dumps(report, indent=2))
    elif args.contract:
        print(f"Requirement contract: {'PASS' if not errors else f'BLOCKED ({len(errors)} errors)'}")
        for error in errors:
            print(error)
    else:
        print(f"{report['requirement_count']} requirements; {report['required_gate_count']} mandatory gates; {counts}")
        print("Production acceptance: " + ("PASS" if not gaps else f"BLOCKED ({len(gaps)} gaps)"))
        for error in errors:
            print(error)
    return 1 if errors else 2 if args.release and gaps else 0


if __name__ == "__main__":
    raise SystemExit(main())
