"""Generate and validate an Apex fixture evidence capture slice.

The script reads a selected manifest and fixture paths only, computes file
identity markers, and validates minimal safety and schema requirements. It does
not mutate inputs.
"""

import argparse
import hashlib
import json
import pathlib
import re
import sys
import urllib.parse
from collections.abc import Mapping

ROOT = pathlib.Path(__file__).resolve().parents[1]
SUPPORTED_SCHEMA_VERSION = "1.0"
REQUIRED_SCHEMA_KEYS = {
    "schema_version",
    "native_format",
    "provenance",
    "producing",
    "evidence_refs",
    "files",
}
HASH_RE = re.compile(r"^[0-9a-fA-F]{64}$")
HEADER_LIMIT = 4096


def load_json_no_duplicates(path):
    def ensure_unique(pairs):
        output = {}
        for key, value in pairs:
            if key in output:
                raise ValueError(f"Duplicate JSON key {key!r} in {path}")
            output[key] = value
        return output

    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=ensure_unique)


def require_keys(data, *, keys, context):
    missing = keys - data.keys()
    if missing:
        raise ValueError(f"Missing required {context} keys: {', '.join(sorted(missing))}")


def validate_schema_version(schema_version):
    if schema_version != SUPPORTED_SCHEMA_VERSION:
        raise ValueError(
            f"Unsupported schema_version {schema_version!r}; expected {SUPPORTED_SCHEMA_VERSION}"
        )


def validate_native_format(native_format):
    if native_format != "unknown":
        raise ValueError("native_format must be \"unknown\" for this capture slice")


def validate_provenance(provenance):
    if not isinstance(provenance, Mapping):
        raise ValueError("provenance must be an object")
    require_keys(provenance, keys={"observed_identity"}, context="provenance")
    if not isinstance(provenance["observed_identity"], str) or not provenance[
        "observed_identity"
    ]:
        raise ValueError("provenance.observed_identity must be a non-empty string")


def validate_list_of_strings(value, *, path):
    if not isinstance(value, list):
        raise ValueError(f"{path} must be a list")
    if not all(isinstance(item, str) for item in value):
        raise ValueError(f"{path} items must be strings")


def validate_producing(producing):
    if not isinstance(producing, Mapping):
        raise ValueError("producing must be an object")
    require_keys(
        producing,
        keys={"build", "modules", "settings", "permissions"},
        context="producing",
    )
    if not isinstance(producing["build"], str) or not producing["build"]:
        raise ValueError("producing.build must be a non-empty string")
    validate_list_of_strings(producing["modules"], path="producing.modules")
    validate_list_of_strings(producing["settings"], path="producing.settings")
    validate_list_of_strings(producing["permissions"], path="producing.permissions")


def validate_hash(raw_hash):
    if raw_hash is None:
        return
    if not isinstance(raw_hash, str) or not HASH_RE.fullmatch(raw_hash):
        raise ValueError(f"Malformed sha256 hash: {raw_hash!r}")


def safe_path(relative_path, *, root):
    if not isinstance(relative_path, str):
        raise ValueError("Path values must be strings")
    if not relative_path:
        raise ValueError("File path must be non-empty")
    if "\0" in relative_path:
        raise ValueError(f"Unsafe null byte in path: {relative_path}")
    if re.match(r"^[A-Za-z]:[\\/]", relative_path):
        raise ValueError(f"Unsafe windows drive reference: {relative_path}")
    candidate = pathlib.PurePath(relative_path)
    if candidate.is_absolute():
        raise ValueError(f"Unsafe absolute path reference: {relative_path}")
    if any(part == ".." for part in candidate.parts):
        raise ValueError(f"Unsafe traversal in path: {relative_path}")
    resolved = (root / candidate).resolve()
    try:
        resolved.relative_to(root)
    except ValueError:
        raise ValueError(f"Path escapes repository root: {relative_path}")
    return resolved


def validate_evidence_ref(ref, *, root):
    if not isinstance(ref, Mapping):
        raise ValueError("Each evidence_ref must be an object")
    require_keys(ref, keys={"kind", "value"}, context="evidence_ref")
    kind = ref["kind"]
    value = ref["value"]
    if kind == "file":
        path = safe_path(value, root=root)
        if not path.is_file():
            raise ValueError(f"Evidence reference file not found: {value}")
    elif kind == "uri":
        parsed = urllib.parse.urlparse(str(value))
        if parsed.scheme not in {"http", "https"}:
            raise ValueError(f"Unsupported evidence URI scheme: {value!r}")
    else:
        raise ValueError(f"Unsupported evidence_ref kind: {kind!r}")


def bounded_header_fingerprint(path):
    with path.open("rb") as source:
        return hashlib.sha256(source.read(HEADER_LIMIT)).hexdigest()


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while True:
            chunk = source.read(64 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def validate_and_build(manifest, *, root):
    if not isinstance(manifest, Mapping):
        raise ValueError("Manifest must be an object")
    require_keys(manifest, keys=REQUIRED_SCHEMA_KEYS, context="manifest")
    validate_schema_version(manifest["schema_version"])
    validate_native_format(manifest["native_format"])
    validate_provenance(manifest["provenance"])
    validate_producing(manifest["producing"])

    refs = manifest["evidence_refs"]
    if not isinstance(refs, list):
        raise ValueError("evidence_refs must be a list")
    for ref in refs:
        validate_evidence_ref(ref, root=root)

    fixtures = manifest["files"]
    if not isinstance(fixtures, list) or not fixtures:
        raise ValueError("files must be a non-empty list")

    captures = []
    for fixture in fixtures:
        if not isinstance(fixture, Mapping):
            raise ValueError("Each file entry must be an object")
        if "path" not in fixture:
            raise ValueError("Each file entry must contain a path")
        path = safe_path(str(fixture["path"]), root=root)
        if not path.is_file():
            raise ValueError(f"Selected fixture is missing: {fixture['path']}")

        expected_sha256 = fixture.get("expected_sha256")
        validate_hash(expected_sha256)
        observed_sha256 = file_sha256(path)
        if expected_sha256 is not None and expected_sha256.lower() != observed_sha256:
            raise ValueError(
                f"Mismatch for {fixture['path']}: expected {expected_sha256}, observed {observed_sha256}"
            )

        captures.append(
            {
                "path": str(path.relative_to(root)),
                "sha256": observed_sha256,
                "bytes": path.stat().st_size,
                "suffix_hint": path.suffix,
                "bounded_header_fingerprint": bounded_header_fingerprint(path),
                "expected_sha256": expected_sha256,
            }
        )

    return {
        "schema_version": SUPPORTED_SCHEMA_VERSION,
        "native_format": "unknown",
        "provenance": {
            "observed_identity": manifest["provenance"]["observed_identity"],
        },
        "producing": {
            "build": manifest["producing"]["build"],
            "modules": list(manifest["producing"]["modules"]),
            "settings": list(manifest["producing"]["settings"]),
            "permissions": list(manifest["producing"]["permissions"]),
        },
        "evidence_refs": list(refs),
        "capture_complete": True,
        "compatibility_passed": False,
        "compatibility_reason": "Not claiming Apex import compatibility in this capture slice",
        "captures": captures,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=pathlib.Path, help="Manifest JSON to validate")
    parser.add_argument("--output", type=pathlib.Path, help="Optional JSON output path")
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--compact", action="store_true")
    args = parser.parse_args(argv)

    root = args.root.resolve()
    try:
        manifest = load_json_no_duplicates(args.manifest)
        report = validate_and_build(manifest, root=root)
    except (ValueError, OSError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1

    payload = json.dumps(report, indent=None if args.compact else 2)
    if args.output:
        args.output.write_text(payload + "\n", encoding="utf-8")
    else:
        print(payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
