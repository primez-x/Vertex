"""Copy original notice bytes from pinned Qt release archives, without running code.

This is a conservative notice superset, not binary/source derivation or licensing
clearance. No downloads, extraction of the whole tree, or replacement of output.
All inputs are validated before creating output. A failed write retains partial
output for diagnosis; a subsequent run requires a different fresh output path.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import stat
import tarfile
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qt_source_inputs as qt_inputs

VERSION = "6.8.3"
INDEX = "notice-index.json"
PINNED_ARCHIVES = {
    "qtbase": {"name": "qtbase-everywhere-src-6.8.3.tar.xz", "bytes": 48426536,
        "sha256": "56001b905601bb9023d399f3ba780d7fa940f3e4861e496a7c490331f49e0b80"},
    "qtsvg": {"name": "qtsvg-everywhere-src-6.8.3.tar.xz", "bytes": 2009072,
        "sha256": "35eb516460f00f264eb504baa253432384351cf23fb9980a5857190e8deef438"},
    "qtwebengine": {"name": "qtwebengine-everywhere-src-6.8.3.tar.xz", "bytes": 566553436,
        "sha256": "df4e19ba2b3a540551b6f998d62597377ffa688c1cff564589b7da2e2bf87337"},
}
for _pin in PINNED_ARCHIVES.values():
    _pin["url"] = "https://download.qt.io/archive/qt/6.8/6.8.3/submodules/" + _pin["name"]

MAX_ENTRIES = 400_000
MAX_MEMBER_BYTES = 512 << 20
MAX_ARCHIVE_EXPANDED_BYTES = 16 << 30
MAX_NOTICE_BYTES = 16 << 20
MAX_TOTAL_NOTICE_BYTES = 128 << 20
MAX_NOTICE_FILES = 10_000
CHROMIUM = "src/3rdparty/chromium"
PDFIUM_LICENSE = CHROMIUM + "/third_party/pdfium/LICENSE"
NOTICE_NAME = re.compile(r"(^|[-_.])(licen[cs]es?|copying|copyright|notice|authors)([-_.]|$)", re.I)
DEVICE_NAME = re.compile(r"^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\.|$)", re.I)
README_NAMES = {"README.chromium", "README.pdfium"}
FLAGS = ("licensing_clearance", "shipped_notice_closure_qualified",
         "binary_source_derivation_qualified", "corresponding_source_qualified",
         "source_rebuild_qualified", "distribution_qualified")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def no_links(path):
    """Reject links/reparse points on existing ancestors before resolving paths."""
    path = Path(os.path.abspath(path))
    for item in (path, *path.parents):
        try:
            info = item.lstat()
        except FileNotFoundError:
            continue
        require(not stat.S_ISLNK(info.st_mode) and
                not (getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT),
                "Input/output path contains a link or reparse point")
    return path


def stamp(info):
    # Python 3.12 Windows stat/fstat disagree on deprecated ctime semantics
    # (creation versus metadata-change time). Identity, size and mtime agree.
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns)


def portable_member(name):
    require(isinstance(name, str) and 0 < len(name) <= 2048 and
            not any(ord(c) < 32 or ord(c) == 127 for c in name) and
            not any(c in name for c in '\\:<>"|?*') and not name.startswith("/"),
            "Unsafe archive member path")
    name = name.rstrip("/")
    parts = name.split("/")
    require(all(p and p not in (".", "..") and not p.endswith((".", " ")) and
                not DEVICE_NAME.match(p) for p in parts), "Unsafe archive member segment")
    return name


def reference_path(metadata, reference):
    require(isinstance(reference, str) and reference and not reference.startswith("/") and
            not any(c in reference for c in '\\:<>"|?*') and
            not any(ord(c) < 32 for c in reference), "Unsafe attribution reference")
    result = posixpath.normpath(posixpath.join(posixpath.dirname(metadata), reference))
    require(result != ".." and not result.startswith("../"), "Attribution reference escapes archive")
    portable_member(result)
    return result


def string_list(value, split=False):
    if value is None:
        return []
    if isinstance(value, str):
        return value.split() if split else [value]
    require(isinstance(value, list) and all(isinstance(v, str) for v in value),
            "Malformed attribution reference list")
    return value


def initial_reasons(relative):
    base = PurePosixPath(relative).name
    reasons = []
    if relative.startswith("LICENSES/") or NOTICE_NAME.search(base):
        reasons.append("notice-name-superset")
    if base == "qt_attribution.json":
        reasons.append("qt-attribution-original")
    if base in README_NAMES:
        reasons.append("chromium-pdfium-attribution-original")
    if relative in (".tag", "CHROMIUM_VERSION"):
        reasons.append("source-version-original")
    return reasons


def scan_archive(stream, module, selected_only=None, version=VERSION):
    """Validate every entry, retaining only notice candidates or selected refs."""
    prefix = f"{module}-everywhere-src-{version}"
    members, case_names, payloads, count, expanded, retained = {}, set(), {}, 0, 0, 0
    stream.seek(0)
    with tarfile.open(fileobj=stream, mode="r|xz") as archive:
        for member in archive:
            count += 1
            require(count <= MAX_ENTRIES, "Archive exceeds entry bound")
            name = portable_member(member.name)
            require(name == prefix or name.startswith(prefix + "/"), "Unexpected archive root")
            key = name.casefold()
            require(key not in case_names, "Duplicate or case-colliding archive member")
            case_names.add(key)
            require(member.isdir() or (member.isfile() and not member.issparse()),
                    "Archive contains link, sparse file or special entry")
            require(not member.linkname and 0 <= member.size <= MAX_MEMBER_BYTES,
                    "Archive contains a link target or oversized entry")
            expanded += member.size
            require(expanded <= MAX_ARCHIVE_EXPANDED_BYTES, "Archive exceeds expanded-byte bound")
            relative = name[len(prefix) + 1:] if name != prefix else ""
            if member.isfile():
                members[relative] = member.size
                wanted = relative in selected_only if selected_only is not None else bool(initial_reasons(relative))
                if wanted:
                    require(member.size <= MAX_NOTICE_BYTES, "Notice exceeds per-file byte bound")
                    retained += member.size
                    require(retained <= MAX_TOTAL_NOTICE_BYTES and len(payloads) < MAX_NOTICE_FILES,
                            "Notice selection exceeds aggregate bound")
                    data = archive.extractfile(member).read(member.size + 1)
                    require(len(data) == member.size, "Truncated notice entry")
                    payloads[relative] = data
            # Streaming mode needs no retained TarInfo objects or random seek.
            archive.members.clear()
    # A file cannot also be an ancestor directory, including case variations.
    regular = {name.casefold() for name in members}
    for name in members:
        require(not any(str(parent).casefold() in regular for parent in PurePosixPath(name).parents
                        if str(parent) != "."), "Archive file/directory conflict")
    return members, payloads, {"entries": count, "expanded_bytes": expanded}


def select_references(module, members, payloads):
    reasons = {name: initial_reasons(name) for name in payloads}
    unresolved = []

    def select(metadata, reference, reason, required=True):
        target = reference_path(metadata, reference)
        if target not in members:
            require(not required, f"Missing Qt attribution reference: {module}/{target}")
            unresolved.append({"module": module, "metadata_member": metadata,
                               "reference": reference, "resolved_member": target,
                               "reason": "not-present-in-pinned-archive"})
        else:
            reasons.setdefault(target, []).append(reason)

    for metadata, data in sorted(payloads.items()):
        base = PurePosixPath(metadata).name
        if base == "qt_attribution.json":
            # Qt attribution format permits literal newline characters in strings.
            rows = json.loads(data, strict=False)
            rows = rows if isinstance(rows, list) else [rows]
            require(rows and all(isinstance(row, dict) for row in rows), "Malformed Qt attribution")
            for row in rows:
                license_refs = string_list(row.get("LicenseFile")) + string_list(row.get("LicenseFiles"))
                for ref in license_refs + string_list(row.get("CopyrightFile")) + string_list(row.get("CopyrightFiles")):
                    select(metadata, ref, "qt-attribution-notice-reference")
                if not license_refs:
                    for ref in string_list(row.get("Files"), split=True):
                        select(metadata, posixpath.join(row.get("Path", ""), ref), "inline-notice-source-original")
        elif base in README_NAMES:
            text = data.decode("utf-8", errors="replace")
            for match in re.finditer(r"^License File:[ \t]*([^\r\n]+)", text, re.M):
                value = match.group(1).strip()
                # Preserve ambiguous/pruned/external references; never fetch them.
                if value in ("NOT_SHIPPED", "N/A"):
                    continue
                # A literal comma list names separate files, including // paths.
                # URLs and prose remain original unresolved metadata, not guesses.
                references = [part.strip() for part in value.split(",")]
                if any(not ref or re.match(r"https?://", ref) or
                       any(c.isspace() or c == ";" for c in ref) for ref in references):
                    unresolved.append({"module": module, "metadata_member": metadata,
                                       "reference": value, "reason": "external-or-ambiguous-reference"})
                else:
                    for reference in references:
                        if reference.startswith("//"):
                            reference = posixpath.relpath(CHROMIUM + "/" + reference[2:], posixpath.dirname(metadata))
                        select(metadata, reference, "chromium-license-reference", required=False)
    return reasons, unresolved


def read_archive(cache, module, pin, version=VERSION):
    path = no_links(cache / pin["name"])
    before = path.stat()
    require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1 and before.st_size == pin["bytes"],
            "Archive is not an ordinary file of the pinned size")
    with path.open("rb") as stream:
        require(stamp(os.fstat(stream.fileno())) == stamp(before), "Archive changed before read")
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
        require(digest == pin["sha256"], "Pinned archive SHA-256 mismatch")
        members, payloads, summary = scan_archive(stream, module, version=version)
        mandatory = {".tag", "LICENSES/LGPL-3.0-only.txt", "LICENSES/GPL-3.0-only.txt"}
        if module == "qtsvg":
            # The selected 6.11.2 source renamed the original permission file.
            # Historical archives retain their exact old member binding.
            mandatory.add("src/svg/XSVG_LICENSE.txt" if version == VERSION else "src/svg/LICENSE.XSVG.txt")
        elif module == "qtwebengine":
            mandatory.update({"CHROMIUM_VERSION", "LICENSE.Chromium", CHROMIUM + "/LICENSE", PDFIUM_LICENSE})
        missing = sorted(name for name in mandatory if not payloads.get(name))
        require(not missing,
                f"Missing or empty mandatory Qt GNU/PDFium/XSVG notice or version record: {module} {version}: {missing}")
        reasons, unresolved = select_references(module, members, payloads)
        missing_bytes = set(reasons) - set(payloads)
        if missing_bytes:
            _, additional, second_summary = scan_archive(stream, module, missing_bytes, version=version)
            require(second_summary == summary and set(additional) == missing_bytes,
                    "Archive reference extraction changed")
            payloads.update(additional)
        require(len(payloads) <= MAX_NOTICE_FILES and sum(map(len, payloads.values())) <= MAX_TOTAL_NOTICE_BYTES,
                "Notice selection exceeds aggregate bound")
        require(stamp(os.fstat(stream.fileno())) == stamp(before), "Archive changed during read")
    no_links(path)
    require(stamp(path.stat()) == stamp(before), "Archive changed after read")
    return payloads, reasons, unresolved, summary, before


def materialize(cache_dir, output_root, version=VERSION):
    # Existing callers retain their historical 6.8.3 defaults and test fixtures.
    pins = PINNED_ARCHIVES if version == VERSION else qt_inputs.selected_archives(Path(__file__).resolve().parents[2], version)
    cache, output = no_links(cache_dir), no_links(output_root)
    require(cache.is_dir() and output.parent.is_dir() and not output.exists(),
            "Expected existing cache/output parent and fresh nonexistent output")
    require(cache != output and not output.is_relative_to(cache) and not cache.is_relative_to(output),
            "Input/output roots must be disjoint")
    selected, archive_rows, unresolved, input_stamps = [], [], [], []
    for module, pin in sorted(pins.items()):
        payloads, reasons, gaps, summary, before = (read_archive(cache, module, pin) if version == VERSION else read_archive(cache, module, pin, version=version))
        input_stamps.append((cache / pin["name"], before))
        archive_rows.append({"module": module, **pin, **summary, "selected_files": len(payloads),
                             "selected_bytes": sum(map(len, payloads.values()))})
        unresolved.extend(gaps)
        for position, (relative, data) in enumerate(sorted(payloads.items()), 1):
            # Short output names avoid Windows path limits; index retains full originals.
            path = f"texts/{module}/{position:04d}.txt"
            row = {"path": path, "archive": pin["name"],
                   "archive_member": f"{module}-everywhere-src-{version}/{relative}",
                   "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                   "selection_reasons": sorted(set(reasons[relative]))}
            selected.append((row, data))
    require(len(selected) <= MAX_NOTICE_FILES and sum(len(data) for _, data in selected) <= MAX_TOTAL_NOTICE_BYTES,
            "Combined notice selection exceeds aggregate bound")
    for path, before in input_stamps:
        no_links(path)
        require(stamp(path.stat()) == stamp(before), "Input changed before publication")
    report = {"schema_version": 1, "qt_version": version,
        "selection": "conservative-source-notice-superset",
        "selected_qt_license_route": "LGPL-3.0-only",
        "application_license": "GPL-3.0-or-later",
        "route_scope": "Qt library code offering LGPLv3; original third-party licenses and alternatives remain intact",
        "scope_limitations": ["Includes unshipped platforms, examples and build/test notices.",
            "Qt PDF binary SBOM omits consumed third-party dependencies; this superset is not a shipped closure proof.",
            "Release archives are not proof of exact prebuilt revision, options, patches or source derivation.",
            "Notice naming and attribution references cannot prove discovery of every inline source notice."],
        **{flag: False for flag in FLAGS}, "archives": archive_rows,
        "files": [row for row, _ in selected], "unresolved_references": unresolved}
    overview = (f"Qt {version} original notices\n\n"
        "Selected Qt library route: LGPL-3.0-only (LGPL version 3).\n"
        "Vertex application license: GPL-3.0-or-later.\n"
        "Third-party copyright, license alternatives and disclaimers are preserved.\n"
        "This is a conservative QtBase/QtSVG/QtWebEngine (PDFium) source notice superset.\n"
        "It includes unshipped code notices and does not establish licensing clearance,\n"
        "complete shipped notices, corresponding source, or binary/source derivation.\n"
        "Original bytes are in texts/; notice-index.json maps each file to its pinned\n"
        "official source archive/member and records SHA-256, size and selection reason.\n"
        "Unresolved references are listed there and require build-specific review.\n").encode("utf-8")
    report["generated_files"] = [{"path": "NOTICE.txt", "bytes": len(overview),
                                  "sha256": hashlib.sha256(overview).hexdigest()}]
    no_links(output.parent)
    # mkdir exclusively reserves the fresh output: concurrent/unknown output wins.
    # Files use exclusive creation too. No recursive deletion or replacement.
    try:
        output.mkdir()
    except FileExistsError as exc:
        raise ValueError("Output appeared before publication; preserved existing output") from exc
    for row, data in selected:
        path = output / row["path"]
        path.parent.mkdir(parents=True, exist_ok=True)
        no_links(path)
        with path.open("xb") as stream:
            stream.write(data)
    with (output / "NOTICE.txt").open("xb") as stream:
        stream.write(overview)
    # Written last: a missing index means materialization never completed.
    with (output / INDEX).open("xb") as stream:
        stream.write((json.dumps(report, indent=2, ensure_ascii=True) + "\n").encode("utf-8"))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--qt-version", default=VERSION, help="Release to materialize; historical default is 6.8.3")
    args = parser.parse_args()
    try:
        report = materialize(args.cache_dir, args.output_root, args.qt_version)
    except (ValueError, OSError, tarfile.TarError, EOFError) as exc:
        parser.exit(1, f"Qt notice materialization failed: {exc}\n")
    print(f"Materialized {len(report['files'])} original notice/version files; "
          f"{len(report['unresolved_references'])} unresolved references. Qualification remains false.")


if __name__ == "__main__":
    main()
