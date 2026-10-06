"""Generate/check the delivery crosswalk, without certifying product behavior.

Source acceptance text remains authoritative. Only explicitly recorded delivery
states survive regeneration; a source's historical 'verified' label is not a
current focused test, installed test, or final acceptance.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile

APEX = "docs/requirements/apex-parity.json"
GATES = "docs/requirements/production-gates.json"
PINC = "docs/requirements/pincsketch-4.3-vertex-comparison.md"
PIN_INVENTORY = "docs/requirements/pincsketch-4.3-feature-inventory.md"
PIN_CHECKLIST = "docs/requirements/pincsketch-vertex-user-checklist.md"
MANUAL = "docs/requirements/vertex-manual-testing-checklist.md"
FEEDBACK = "docs/delivery/user-feedback.md"
REGISTRY = "docs/delivery/requirements.json"
PLAN_DIRS = ("docs/plans", "docs/superpowers/plans")
PROGRESS_FIELDS = ("implementation", "focused_verification", "installed_verification",
                   "final_acceptance", "delivery_notes", "fixture_refs", "evidence_refs", "qualification_history")
DEPENDENCIES = {
    "D00": [], "D01": [], "D02": [], "D03": [], "D04": ["D03"],
    "D05": ["D03", "D04"], "D06": ["D03", "D04"],
    "D07": ["D03", "D09"], "D08": ["D03", "D04", "D06", "D09"],
    "D09": ["D02", "D03"],
    "D10": [f"D{number:02}" for number in range(1, 10)],
}
PINC_OWNERS = {1: "D04", 2: "D05", 3: "D04", 4: "D05", 5: "D08", 6: "D08",
               7: "D05", 8: "D05", 9: "D04", 10: "D05", 11: "D08", 12: "D05", 13: "D10"}


def pin_owner(number: int) -> str:
    if number in set(range(1, 35)) | {39, 40, 128} | set(range(46, 52)) | set(range(55, 58)) | set(range(111, 120)):
        return "D04"
    if number in {91, 92, 93}:
        return "D06"
    if number in set(range(120, 125)) | {127}:
        return "D03"
    if number in {125, 126} | set(range(129, 135)):
        return "D08"
    return "D05"


def read(root: Path, path: str) -> str:
    return (root / path).read_text(encoding="utf-8")


def anchor(path: str, line: int) -> str:
    return f"{path}:L{line}"


def clauses(text: str) -> list[str]:
    # Keep source wording, including lists, numbers and qualification boundaries.
    return [part.strip() for part in re.split(r";\s+|(?<=[.!?])\s+(?=[A-Z])", text) if part.strip()]


def digest(value: dict) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False,
                                    separators=(",", ":")).encode("utf-8")).hexdigest()


def product_source_fingerprint(root: Path) -> str:
    """Conservative product/test/build inputs; no generated output or delivery state."""
    paths = set()
    for folder in ("src", "include", "assets", "resources", "cmake", "tests", "scripts"):
        paths.update(path for path in (root / folder).rglob("*") if path.is_file()
                     and "__pycache__" not in path.parts and path.suffix not in (".pyc", ".pyo"))
    for folder in ("third_party", "packaging"):
        paths.update(path for path in (root / folder).glob("*") if path.is_file())
    paths.update(root / name for name in ("CMakeLists.txt", "CMakePresets.json", "docs/user-guide.html")
                 if (root / name).is_file())
    result = hashlib.sha256()
    for path in sorted(paths, key=lambda path: path.relative_to(root).as_posix()):
        result.update(path.relative_to(root).as_posix().encode("utf-8") + b"\0")
        result.update(hashlib.sha256(path.read_bytes()).digest())
    return result.hexdigest()


def definition_fingerprint(item: dict, document: dict) -> str:
    fields = ("id", "source_kind", "title", "expected_result", "required_variants", "assertions",
              "remaining_behavior", "owner_package", "package_dependencies", "required_gate",
              "required_behavior", "original_source_behavior", "steps", "full_title")
    semantics = {field: item.get(field) for field in fields}
    semantics["package_pass_condition"] = next(package["pass_condition"] for package in document["packages"]
                                               if package["id"] == item["owner_package"])
    return digest(semantics)


def applicability_binding(item: dict, document: dict) -> dict:
    return {"definition_fingerprint": item["definition_fingerprint"],
            "source_fingerprint": document["product_source_fingerprint"],
            "build_binding": document["current_build_binding"]}


def row(identifier: str, kind: str, title: str, expected: str, owner: str,
        sources: list[str], *, remaining: str | None = None) -> dict:
    return {
        "id": identifier, "source_kind": kind, "title": title,
        "source_refs": sources, "owner_package": owner,
        "package_dependencies": DEPENDENCIES[owner],
        "remaining_behavior": remaining or expected,
        "required_variants": clauses(expected), "expected_result": expected,
        "assertions": clauses(expected), "test_refs": None, "fixture_refs": None,
        "historical_evidence_refs": None, "evidence_refs": None,
        "implementation": {"status": "not_started", "notes": None},
        "focused_verification": {"status": "not_run", "evidence_refs": None, "notes": None, "binding": None},
        "installed_verification": {"status": "not_run", "evidence_refs": None, "notes": None, "binding": None},
        "final_acceptance": {"status": "not_accepted", "evidence_refs": None,
                             "observed_by": None, "notes": None, "binding": None},
        "qualification_history": [],
        "delivery_notes": None,
    }


def package_for(section: str) -> str:
    """Accountability by workflow; dependencies retain cross-package obligations."""
    name = section.lower()
    if any(term in name for term in ("optional assistance", "extraction", "ocr")):
        return "D07"
    if any(term in name for term in ("saving, revisions", "project folders", "comparing saved revisions")):
        return "D03"
    if any(term in name for term in ("printing", "export", "cad fixtures", "import", "pinc-project")):
        return "D08"
    if any(term in name for term in ("real job", "performance", "accessibility", "distribution")):
        return "D10"
    if any(term in name for term in ("arrange the workspace", "layers", "visibility", "navigation",
                                     "building objects", "buildings", "doors", "windows", "walls",
                                     "wall-", "physical-room", "physical-wall", "architectural", "remodel",
                                     "3d views", "schedules", "saved-view", "appearance", "plan label")):
        return "D06"
    if any(term in name for term in ("reference plans", "symbols", "library", "text, labels",
                                     "appraisal reports", "survey and georeferencing")):
        return "D05"
    return "D04"


def parse_manual(root: Path) -> list[dict]:
    text = read(root, MANUAL)
    matches = list(re.finditer(r"^- \[ \] \*\*(U[0-9]{3}a?)\s+\W\s+(.+?)\*\*", text, re.M))
    result = []
    for index, match in enumerate(matches):
        block = text[match.end():matches[index + 1].start() if index + 1 < len(matches) else len(text)]
        expected = re.search(r"^  - Expected: (.+)$", block, re.M)
        if expected is None:
            raise ValueError(f"manual scenario {match[1]} has no expected result")
        steps = re.search(r"^  - Steps: (.+)$", block, re.M)
        sections = list(re.finditer(r"^## (.+)$", text[:match.start()], re.M))
        section = sections[-1][1] if sections else ""
        item = row(match[1], "manual_scenario", match[2], expected[1],
                   package_for(section), [anchor(MANUAL, text.count("\n", 0, match.start()) + 1)])
        item.update({"full_title": match[0].split("**")[1], "section": section,
                     "steps": steps[1] if steps else None, "user_result": "not_tested"})
        result.append(item)
    return result


def parse_pin(root: Path) -> list[dict]:
    inventory = read(root, PIN_INVENTORY)
    checklist = read(root, PIN_CHECKLIST)
    equivalents = {match[1]: match for match in re.finditer(
        r"^- \[ \] \*\*(PIN-[0-9]{3})\*\*\s+\W\s+(.+)$", checklist, re.M)}
    result = []
    section, source_anchor = "", ""
    for line_number, line in enumerate(inventory.splitlines(), 1):
        if line.startswith("### "):
            section = line[4:]
        elif line.startswith("Source:"):
            source_anchor = line
        match = re.match(r"- \*\*(PIN-[0-9]{3})\*\*: (.+)", line)
        if match:
            equivalent = equivalents.get(match[1])
            if equivalent is None:
                raise ValueError(f"missing Vertex checklist operation {match[1]}")
            item = row(match[1], "pinc_operation", match[2], equivalent[2], pin_owner(int(match[1][-3:])), [
                anchor(PIN_INVENTORY, line_number),
                anchor(PIN_CHECKLIST, checklist.count("\n", 0, equivalent.start()) + 1)])
            item.update({"section": section, "original_source_behavior": match[2],
                         "original_html_source_anchor": source_anchor,
                         "steps": equivalent[2], "user_result": "not_tested"})
            result.append(item)
    if set(equivalents) != {item["id"] for item in result}:
        raise ValueError("Pinc source inventory/checklist IDs differ")
    return result


def parse_adoption(root: Path) -> list[dict]:
    result = []
    for line_number, line in enumerate(read(root, PINC).splitlines(), 1):
        match = re.match(r"\| (PINC-[0-9]{3}) \| (.*?) \| (.*?) \|$", line)
        if match:
            item = row(match[1], "pinc_adoption", match[2], match[3], PINC_OWNERS[int(match[1][-3:])],
                       [anchor(PINC, line_number)], remaining=f"{match[2]} — {match[3]}")
            item.update({"required_behavior": match[2], "original_acceptance": match[3]})
            result.append(item)
    return result


def parse_feedback(root: Path) -> list[dict]:
    result = []
    for line_number, line in enumerate(read(root, FEEDBACK).splitlines(), 1):
        match = re.match(r"\| (FB-[0-9]{3}) \| (D[0-9]{2}) \| (.*?) \| (.*?) \|$", line)
        if match:
            result.append(row(match[1], "user_feedback", match[3], match[4], match[2],
                              [anchor(FEEDBACK, line_number)]))
    return result


def delivery_rows(document: dict):
    for group in ("requirements", "manual_scenarios", "user_feedback"):
        yield from document[group]


def build_registry(root: Path, previous: dict | None = None) -> dict:
    source = json.loads(read(root, APEX))
    gates = json.loads(read(root, GATES))["gates"]
    ownership = {}
    packages = [{"id": "D00", "name": "Workspace migration", "required_gate": None,
                 "dependencies": [], "pass_condition": "Root verifies the copied checkout before edits."}]
    for gate_id, gate in gates.items():
        package = "D" + gate_id.split("_")[0][1:].zfill(2)
        packages.append({"id": package, "name": gate["name"], "required_gate": gate_id,
                         "dependencies": DEPENDENCIES[package], "pass_condition": gate["pass_condition"]})
        for identifier in gate["requirement_ids"]:
            if identifier in ownership:
                raise ValueError(f"overlapping gate ownership: {identifier}")
            ownership[identifier] = (package, gate_id)
    requirements = []
    apex_text = read(root, APEX)
    for original in source["requirements"]:
        identifier = original["id"]
        if identifier not in ownership:
            raise ValueError(f"unowned Apex requirement: {identifier}")
        package, gate_id = ownership[identifier]
        offset = apex_text.index(f'"id": "{identifier}"')
        item = row(identifier, "apex", original["requirement"], original["acceptance"], package,
                   [anchor(APEX, apex_text.count("\n", 0, offset) + 1), f"{GATES}#/gates/{gate_id}"],
                   remaining=original["blocker"] or original["acceptance"])
        item.update({"source_snapshot": original, "required_gate": gate_id,
                     "original_package": original["package"]})
        paths = original["implementation_sources"]
        item["test_refs"] = [path for path in paths if path.startswith("tests/")] or None
        item["fixture_refs"] = [path for path in paths if "/fixtures/" in path] or None
        item["historical_evidence_refs"] = [path for path in paths if path.startswith("docs/verification/")] or None
        item["implementation"] = {
            "status": "source_reported_verified" if original["implementation_status"] == "verified"
                      else original["implementation_status"],
            "notes": original["implementation_note"],
        }
        requirements.append(item)
    if set(ownership) != {item["id"] for item in requirements}:
        raise ValueError("gate ownership and Apex source coverage differ")
    requirements.extend(parse_adoption(root))
    requirements.extend(parse_pin(root))
    plans = []
    for folder in PLAN_DIRS:
        for path in sorted((root / folder).glob("*.md")):
            relative = path.relative_to(root).as_posix()
            text = path.read_text(encoding="utf-8")
            title = next((line.lstrip("# ") for line in text.splitlines() if line.startswith("# ")), path.stem)
            package = package_for(path.stem)
            plans.append({"id": path.stem, "source_path": relative, "source_refs": [anchor(relative, 1)],
                          "title": title, "owner_package": package,
                          "package_dependencies": DEPENDENCIES[package],
                          "role": "historical_scope_and_evidence_reference",
                          "execution_cadence": "superseded_by_docs/delivery/plan.md",
                          "acceptance_status": "not_accepted"})
    document = {
        "schema_version": 1, "registry_id": "vertex-production-delivery",
        "authority": "docs/delivery/plan.md", "current_package": "D03",
        "scope": source["scope"], "source_catalog": source["source_catalog"],
        "qualification_boundary": "Mapping, source presence, historical claims and registry tests do not establish feature or production acceptance.",
        "status_vocabulary": {
            "implementation": ["not_started", "in_progress", "source_reported_verified", "implemented"],
            "verification": ["not_run", "passed", "failed", "blocked"],
            "final_acceptance": ["not_accepted", "accepted"],
            "user_result": ["not_tested", "pass", "fail", "blocked"],
        },
        "packages": packages, "requirements": requirements, "manual_scenarios": parse_manual(root),
        "user_feedback": parse_feedback(root), "dated_plans": plans,
        "product_source_fingerprint": product_source_fingerprint(root),
        "current_build_binding": previous.get("current_build_binding") if previous else None,
    }
    for item in delivery_rows(document):
        item["definition_fingerprint"] = definition_fingerprint(item, document)
    if previous:
        old = {item["id"]: item for item in delivery_rows(previous)}
        for item in delivery_rows(document):
            if item["id"] in old:
                fresh_states = {field: copy.deepcopy(item[field]) for field in
                                ("focused_verification", "installed_verification", "final_acceptance")}
                for field in PROGRESS_FIELDS + ("user_result",):
                    if field in old[item["id"]]:
                        item[field] = copy.deepcopy(old[item["id"]][field])
                prior = old[item["id"]]
                changed = prior.get("definition_fingerprint") != item["definition_fingerprint"]
                changed = changed or previous.get("product_source_fingerprint") != document["product_source_fingerprint"]
                changed = changed or any(item[field].get("binding") != applicability_binding(item, document)
                                         for field in fresh_states
                                         if item[field]["status"] in ("passed", "accepted"))
                if changed:
                    if any(item[field]["status"] not in ("not_run", "not_accepted") for field in fresh_states) or item.get("user_result", "not_tested") != "not_tested":
                        item["qualification_history"].append({
                            "reason": "definition_or_source_build_binding_changed",
                            "definition_fingerprint": prior.get("definition_fingerprint"),
                            "source_fingerprint": previous.get("product_source_fingerprint"),
                            **{field: copy.deepcopy(item[field]) for field in fresh_states},
                            "user_result": item.get("user_result"),
                        })
                    item.update(fresh_states)
                    if "user_result" in item:
                        item["user_result"] = "not_tested"
        document["current_package"] = previous.get("current_package", "D03")
    document["coverage_counts"] = {
        "apex_requirements": 130, "pinc_adoption_requirements": 13, "pinc_operations": 134,
        "manual_scenarios": 450, "dated_plans": 83, "user_feedback": len(document["user_feedback"]),
    }
    return document


def require(condition: bool, message: str):
    if not condition:
        raise ValueError(message)


def validate_registry(document: dict, root: Path):
    packages = {item["id"] for item in document["packages"]}
    require(packages == set(DEPENDENCIES), "package coverage differs")
    require(document["current_package"] in packages, "unknown current package")
    require(document["current_build_binding"] is None or
            bool(re.fullmatch(r"[0-9a-f]{64}", document["current_build_binding"])), "invalid build binding")
    rows = list(delivery_rows(document))
    identifiers = [item["id"] for item in rows]
    require(len(identifiers) == len(set(identifiers)), "duplicate registry IDs")
    originals = {item["id"]: item for item in json.loads(read(root, APEX))["requirements"]}
    gate_owners = {}
    for gate_id, gate in json.loads(read(root, GATES))["gates"].items():
        for identifier in gate["requirement_ids"]:
            require(identifier not in gate_owners, f"overlapping gate ownership: {identifier}")
            gate_owners[identifier] = ("D" + gate_id.split("_")[0][1:].zfill(2), gate_id)
    expected_sets = {
        "apex": set(originals),
        "pinc_adoption": {f"PINC-{n:03}" for n in range(1, 14)},
        "pinc_operation": {f"PIN-{n:03}" for n in range(1, 135)},
        "manual_scenario": {f"U{n:03}" for n in range(1, 450)} | {"U100a"},
    }
    for kind, identifiers_expected in expected_sets.items():
        actual = {item["id"] for item in rows if item["source_kind"] == kind}
        require(actual == identifiers_expected, f"{kind} coverage differs")
    expected_plans = {path.relative_to(root).as_posix() for folder in PLAN_DIRS
                      for path in (root / folder).glob("*.md")}
    plan_paths = [item["source_path"] for item in document["dated_plans"]]
    require(len(plan_paths) == len(set(plan_paths)) and set(plan_paths) == expected_plans,
            "dated plan coverage differs")
    require(len(plan_paths) == 83, "dated plan count differs from approved inventory")
    require(bool(document["user_feedback"]), "missing user feedback")
    for item in rows:
        identifier = item["id"]
        require(item["owner_package"] in packages - {"D00"}, f"{identifier}: invalid owner")
        require(item["package_dependencies"] == DEPENDENCIES[item["owner_package"]],
                f"{identifier}: package dependencies differ")
        if item["source_kind"] == "apex":
            require(item.get("source_snapshot") == originals[identifier],
                    f"{identifier}: original source snapshot differs")
            require(item["expected_result"] == originals[identifier]["acceptance"],
                    f"{identifier}: source acceptance differs")
            require((item["owner_package"], item.get("required_gate")) == gate_owners.get(identifier),
                    f"{identifier}: gate ownership differs")
        elif item["source_kind"] == "pinc_adoption":
            require(item["owner_package"] == PINC_OWNERS[int(identifier[-3:])],
                    f"{identifier}: Pinc adoption ownership differs")
        elif item["source_kind"] == "pinc_operation":
            require(item["owner_package"] == pin_owner(int(identifier[-3:])),
                    f"{identifier}: Pinc operation ownership differs")
        require(item.get("definition_fingerprint") == definition_fingerprint(item, document),
                f"{identifier}: definition fingerprint differs")
        for field in ("title", "source_refs", "remaining_behavior", "required_variants", "expected_result", "assertions"):
            require(bool(item.get(field)), f"{identifier}: missing {field}")
        require(item["implementation"]["status"] in document["status_vocabulary"]["implementation"],
                f"{identifier}: invalid implementation status")
        for field in ("focused_verification", "installed_verification"):
            state = item[field]
            require(state["status"] in document["status_vocabulary"]["verification"],
                    f"{identifier}: invalid {field} status")
            if state["status"] == "passed":
                require(bool(state["evidence_refs"]), f"{identifier}: passed check requires evidence")
                require(state.get("binding") == applicability_binding(item, document),
                        f"{identifier}: verification applicability binding differs")
                if field == "installed_verification":
                    require(document["current_build_binding"] is not None,
                            f"{identifier}: installed pass requires candidate build binding")
        final = item["final_acceptance"]
        require(final["status"] in document["status_vocabulary"]["final_acceptance"],
                f"{identifier}: invalid final acceptance status")
        if final["status"] == "accepted":
            require(bool(final["evidence_refs"]) and bool(final["observed_by"])
                    and item["focused_verification"]["status"] == "passed"
                    and item["installed_verification"]["status"] == "passed",
                    f"{identifier}: acceptance requires observed installed/focused evidence")
            require(final.get("binding") == applicability_binding(item, document),
                    f"{identifier}: acceptance applicability binding differs")
        if "user_result" in item:
            require(item["user_result"] in document["status_vocabulary"]["user_result"],
                    f"{identifier}: invalid user result status")
    # Do not leak personal checkout/fixture locations into the tracked crosswalk.
    require(not re.search(r"(?<![A-Za-z])[A-Za-z]:[\\/]", json.dumps(document, ensure_ascii=False)),
            "machine-specific paths are forbidden")


def publish_registry(target: Path, document: dict, root: Path):
    """Validate before publishing; interrupted replacement preserves prior progress."""
    validate_registry(document, root)
    payload = json.dumps(document, ensure_ascii=False, indent=2) + "\n"
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=target.parent,
                                         prefix=target.name + ".", suffix=".tmp", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, target)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Check coverage, integrity and source freshness; no writes.")
    parser.add_argument("--write", action="store_true", help="Refresh source mappings while preserving recorded progress.")
    parser.add_argument("--build-binding", help="SHA-256 digest of the identified candidate build/manifest; never inferred.")
    args = parser.parse_args()
    if args.check == args.write:
        parser.error("choose exactly one of --check or --write")
    if args.build_binding and (not args.write or not re.fullmatch(r"[0-9a-f]{64}", args.build_binding)):
        parser.error("--build-binding requires --write and a lowercase SHA-256 digest")
    root = Path(__file__).resolve().parents[1]
    target = root / REGISTRY
    try:
        previous = json.loads(target.read_text(encoding="utf-8")) if target.exists() else None
        if args.build_binding:
            if previous is None:
                previous = build_registry(root)
            previous["current_build_binding"] = args.build_binding
        if previous is not None:
            # Source snapshots may legitimately become stale after source edits;
            # reject malformed progress before merging without requiring freshness.
            refreshed_for_validation = build_registry(root, previous)
            old_ids = [item["id"] for item in delivery_rows(previous)]
            require(len(old_ids) == len(set(old_ids)), "duplicate IDs in previous registry")
            validate_registry(refreshed_for_validation, root)
        expected = build_registry(root, previous)
        validate_registry(expected, root)
        if args.check:
            require(previous is not None, "registry missing; run --write")
            validate_registry(previous, root)
            require(previous == expected, "registry source mappings are stale; run --write")
        else:
            publish_registry(target, expected, root)
        print(json.dumps({"coverage": expected["coverage_counts"], "check_scope": "registry_integrity_only"}))
        return 0
    except (ValueError, OSError, KeyError) as error:
        print(f"delivery registry: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
