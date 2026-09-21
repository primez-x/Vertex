"""Validate the complete offline Vertex symbol-catalog manifest.

The C++ catalog validator protects the application at runtime.  This small
dependency-free checker validates the exported JSON manifest as a release
artifact as well, so a package cannot claim catalog coverage from an arbitrary
filtered or malformed listing.  It checks structure and geometry; artwork
recognition and print legibility still require the human visual qualification
described in ``docs/production-qualification.md``.
"""

import argparse
import json
import math
import pathlib
import sys


REQUIRED_CATEGORIES = frozenset({
    "plumbing", "furniture", "fixtures", "appliances", "accessibility",
    "lighting", "doors_windows", "structural", "site", "commercial",
})
REQUIRED_REPRESENTATIVES = frozenset({"toilet", "sofa", "checkout-counter"})
BED_REPRESENTATIVES = frozenset({"single-bed", "double-bed"})
MINIMUM_DISTINCT_FAMILIES = 300
MINIMUM_CATALOG_ENTRIES = 600


def _number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def _point(value, label, errors):
    if not isinstance(value, dict) or not _number(value.get("x")) or not _number(value.get("y")):
        errors.append(label + ": finite x/y point required")
        return None
    return float(value["x"]), float(value["y"])


def validate_manifest(manifest):
    """Return a deterministic report for an unfiltered catalog manifest."""

    errors = []
    if not isinstance(manifest, dict):
        return {"valid": False, "errors": ["manifest: object required"], "entry_count": 0}
    if manifest.get("schema_version") != 1:
        errors.append("schema_version: expected integer 1")
    if manifest.get("catalog_id") != "vertex.symbol-catalog":
        errors.append("catalog_id: expected vertex.symbol-catalog")
    if type(manifest.get("catalog_revision")) is not int or manifest["catalog_revision"] != 1:
        errors.append("catalog_revision: expected integer 1")

    query = manifest.get("query", "")
    category = manifest.get("category", "")
    if not isinstance(query, str) or not isinstance(category, str):
        errors.append("query and category must be strings")
    elif query or category:
        errors.append("manifest must be the complete unfiltered catalog")

    entries = manifest.get("entries")
    if not isinstance(entries, list):
        errors.append("entries: array required")
        entries = []
    entry_count = manifest.get("entry_count")
    if type(entry_count) is not int or entry_count != len(entries):
        errors.append("entry_count must equal the entries array length")
    catalog_entry_count = manifest.get("catalog_entry_count", entry_count)
    if type(catalog_entry_count) is not int or catalog_entry_count != len(entries):
        errors.append("catalog_entry_count must equal the complete entries array length")
    filtered_entry_count = manifest.get("filtered_entry_count", entry_count)
    if type(filtered_entry_count) is not int or filtered_entry_count != len(entries):
        errors.append("filtered_entry_count must equal the entries array length")
    if len(entries) < MINIMUM_CATALOG_ENTRIES:
        errors.append(f"catalog must contain at least {MINIMUM_CATALOG_ENTRIES} entries")

    computed_categories = {}
    computed_families = {}
    previous_id = None
    seen_ids = set()
    family_representatives = set()
    for index, entry in enumerate(entries):
        label = f"entries[{index}]"
        if not isinstance(entry, dict):
            errors.append(label + ": object required")
            continue
        identifier = entry.get("id")
        family = entry.get("family")
        category_name = entry.get("category")
        if not isinstance(identifier, str) or not identifier:
            errors.append(label + ".id: nonempty string required")
            identifier = ""
        else:
            if identifier in seen_ids:
                errors.append(label + ": duplicate id " + identifier)
            seen_ids.add(identifier)
            if previous_id is not None and identifier <= previous_id:
                errors.append(label + ": entries must be sorted by id")
            previous_id = identifier
        if not isinstance(family, str) or not family:
            errors.append(label + ".family: nonempty string required")
            family = ""
        if not isinstance(category_name, str) or not category_name:
            errors.append(label + ".category: nonempty string required")
            category_name = ""
        if (family and identifier and identifier != family and
                not identifier.startswith(family + "-")):
            errors.append(label + ": id must match its family or begin with the family name")
        if family and category_name:
            computed_categories[category_name] = computed_categories.get(category_name, 0) + 1
            family_record = computed_families.setdefault(
                family, {"id": family, "category": category_name, "variant_count": 0})
            if family_record["category"] != category_name:
                errors.append(label + ": family spans multiple categories")
            family_record["variant_count"] += 1
            family_representatives.add(family)

        width = entry.get("width_metres")
        depth = entry.get("depth_metres")
        minimum = entry.get("minimum_scale")
        maximum = entry.get("maximum_scale")
        if not _number(width) or width <= 0:
            errors.append(label + ".width_metres: finite positive number required")
            width = 0.0
        if not _number(depth) or depth <= 0:
            errors.append(label + ".depth_metres: finite positive number required")
            depth = 0.0
        if not _number(minimum) or not _number(maximum) or minimum <= 0 or maximum < minimum:
            errors.append(label + ": invalid scale limits")
        elif not (minimum < 1.0 < maximum):
            errors.append(label + ": scale limits must permit practical resizing")
        anchor = _point(entry.get("anchor"), label + ".anchor", errors)
        preview = entry.get("preview")
        if not isinstance(preview, list) or not preview:
            errors.append(label + ".preview: nonempty array required")
            preview = []
        for stroke_index, stroke in enumerate(preview):
            stroke_label = f"{label}.preview[{stroke_index}]"
            if not isinstance(stroke, dict):
                errors.append(stroke_label + ": object required")
                continue
            start = _point(stroke.get("start"), stroke_label + ".start", errors)
            end = _point(stroke.get("end"), stroke_label + ".end", errors)
            if start is None or end is None:
                continue
            if start == end:
                errors.append(stroke_label + ": degenerate stroke")
            if anchor is not None and width > 0 and depth > 0:
                for point_name, point in (("start", start), ("end", end)):
                    if (abs(point[0] - anchor[0]) > width / 2.0 + 1e-9 or
                            abs(point[1] - anchor[1]) > depth / 2.0 + 1e-9):
                        errors.append(stroke_label + f".{point_name}: outside declared footprint")

    declared_categories = manifest.get("category_counts")
    if not isinstance(declared_categories, dict):
        errors.append("category_counts: object required")
        declared_categories = {}
    if not REQUIRED_CATEGORIES.issubset(declared_categories):
        missing = sorted(REQUIRED_CATEGORIES - set(declared_categories))
        errors.append("category_counts missing required categories: " + ", ".join(missing))
    if declared_categories != computed_categories:
        errors.append("category_counts must match entry categories")

    declared_families = manifest.get("families")
    if not isinstance(declared_families, list):
        errors.append("families: array required")
        declared_families = []
    family_ids = [item.get("id") for item in declared_families if isinstance(item, dict)]
    if family_ids != sorted(family_ids) or len(family_ids) != len(set(family_ids)):
        errors.append("families must contain unique ids sorted lexicographically")
    if type(manifest.get("family_count")) is not int or manifest["family_count"] != len(computed_families):
        errors.append("family_count must match the entries")
    if len(computed_families) < MINIMUM_DISTINCT_FAMILIES:
        errors.append(
            f"catalog must contain at least {MINIMUM_DISTINCT_FAMILIES} distinct families")
    expected_families = [computed_families[key] for key in sorted(computed_families)]
    if declared_families != expected_families:
        errors.append("families must match entry family/category/variant counts")

    missing_representatives = sorted(REQUIRED_REPRESENTATIVES - family_representatives)
    if missing_representatives:
        errors.append("missing representative family: " + ", ".join(missing_representatives))
    if not BED_REPRESENTATIVES.intersection(family_representatives):
        errors.append("missing representative family: single-bed or double-bed")

    return {"valid": not errors, "errors": sorted(set(errors)), "entry_count": len(entries),
            "family_count": len(computed_families), "category_counts": computed_categories}


def _load(path):
    return json.loads(pathlib.Path(path).read_text(encoding="utf-8"))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=pathlib.Path)
    args = parser.parse_args(argv)
    try:
        report = validate_manifest(_load(args.manifest))
    except (OSError, ValueError, TypeError) as error:
        report = {"valid": False, "errors": [str(error)], "entry_count": 0}
    print(json.dumps(report, indent=2, sort_keys=True, allow_nan=False))
    return 0 if report["valid"] else 1


if __name__ == "__main__":
    sys.exit(main())
