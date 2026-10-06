"""Replay the locked CPython publisher evidence through an explicit reader.

This helper performs no filesystem or network access. The caller's reader owns
resolution, regular-file/hash checks, inert archive budgets and final drift
checks. The fixed publisher pins deliberately support the selected 3.13.15
release only; replacing them requires new publisher acquisition and review.
"""
from __future__ import annotations

import base64
import hashlib
import json
from pathlib import PurePosixPath

VERSION = "3.13.15"
PREFIX = "Python-" + VERSION + "/"
EMBED_NAME = "python-" + VERSION + "-embed-amd64.zip"
EMBED_URL = "https://www.python.org/ftp/python/" + VERSION + "/" + EMBED_NAME
EMBED_SHA256 = "d1f04d990aee1253d8569e8e5104e30fa9f5fa830899f14843448872d936a2cf"
SOURCE_SHA256 = "1e66a7945a48390ee4c2a4268a0e4185884059a13c4aab6d148aa208deea4a76"
SBOM_SHA256 = "ba428f93acb06764f0005246f8ca48bb1c04feef64ce7db0b177fb2fd371c423"
HACL_REVISION = "bb3d0dc8d9d15a5cd51094d5b69e70aa09005ff0"
PACKAGE_IDS = {"SPDXRef-PACKAGE-" + n for n in (
    "bzip2", "expat-2.8.2", "hacl-star-" + HACL_REVISION, "libb2-0.98.1", "libffi",
    "macholib-1.0", "mpdecimal", "mpdecimal-2.5.1", "openssl", "sqlite", "tcl-core", "tk", "xz", "zlib")}
UNRESOLVED = {"SPDXRef-PACKAGE-mpdecimal-2.5.1", "SPDXRef-PACKAGE-tcl-core", "SPDXRef-PACKAGE-tk"}
CONTROLS = {PREFIX + name for name in (
    "Modules/_blake2/blake2b_impl.c", "PCbuild/_bz2.vcxproj", "PCbuild/_ctypes.vcxproj",
    "PCbuild/_decimal.vcxproj", "PCbuild/_elementtree.vcxproj", "PCbuild/_hashlib.vcxproj",
    "PCbuild/_lzma.vcxproj", "PCbuild/_sqlite3.vcxproj", "PCbuild/_ssl.vcxproj",
    "PCbuild/get_externals.bat", "PCbuild/libffi.props", "PCbuild/liblzma.vcxproj",
    "PCbuild/openssl.props", "PCbuild/pyexpat.vcxproj", "PCbuild/python.props",
    "PCbuild/pythoncore.vcxproj", "PCbuild/sqlite3.vcxproj")}
VENDORED_COUNTS = {"expat": 23, "hacl-star": 20, "libb2": 14, "macholib": 4, "mpdecimal": 52}
NATIVE_ASSOCIATIONS = {
    "bzip2": ["_bz2.pyd"], "libffi": ["_ctypes.pyd", "libffi-8.dll"],
    "mpdecimal": ["_decimal.pyd"], "openssl": ["_hashlib.pyd", "_ssl.pyd", "libcrypto-3.dll", "libssl-3.dll"],
    "sqlite": ["_sqlite3.pyd", "sqlite3.dll"], "xz": ["_lzma.pyd"], "zlib": ["python313.dll"],
    "expat": ["_elementtree.pyd", "pyexpat.pyd"], "hacl-star": ["python313.dll"], "libb2": ["python313.dll"],
    "macholib": ["python313.zip"]}
LICENSE_MEMBERS = {
    "bzip2": ["cpython-source-deps-bzip2-1.0.8/LICENSE"], "expat": ["expat-2.8.2/COPYING"],
    "hacl-star": ["hacl-star-" + HACL_REVISION + "/" + n for n in ("LICENSE", "dist/LICENSE.txt", "vale/LICENSE")],
    "libb2": ["libb2-0.98.1/COPYING"],
    "libffi": ["cpython-source-deps-libffi-3.4.4/" + n for n in ("LICENSE", "LICENSE-BUILDTOOLS")],
    "mpdecimal": ["cpython-source-deps-mpdecimal-4.0.0/" + n for n in ("COPYRIGHT.txt", "doc/COPYRIGHT.txt")],
    "openssl": ["cpython-source-deps-openssl-3.0.21/LICENSE.txt"],
    "xz": ["cpython-source-deps-xz-5.2.5/" + n for n in ("COPYING", "COPYING.GPLv2", "COPYING.GPLv3", "COPYING.LGPLv2.1")],
    "zlib": ["cpython-source-deps-zlib-1.3.1/LICENSE"], "macholib": [], "sqlite": []}
SIGNATURE_PINS = {
    EMBED_NAME + ".asc": "16683bc54930b2a5ef625d77d152e4248e0d0fbf4a8a55bb4ebca459154e685f",
    EMBED_NAME + ".crt": "1d0302575baa48cce5353a38826ea7846d6e3acaad9b461cd110440bbed4552b",
    EMBED_NAME + ".sig": "f43cb69858ddbfbe39e12dcfadf997c1efdfc27b94278055923d85d26a0b0cc9",
    EMBED_NAME + ".sigstore": "3e487c064a40d94a59476eb05e2d6225c325665590797e0a03cd33592b617137"}
REMAINING = [
    "cpython_publisher_sbom_has_no_per_binary_dependency_graph",
    "cpython_native_source_associations_are_source_build_inferences",
    "cpython_libffi_openssl_prebuilt_input_provenance_and_build_options_pending",
    "cpython_signature_trust_identity_and_transparency_verification_pending",
    "cpython_tcl_tk_and_vendored_mpdecimal_2_5_1_candidate_exclusions_not_qualified",
    "cpython_complete_transitive_license_and_notice_obligations_not_qualified",
    "cpython_offline_source_rebuild_and_binary_derivation_not_qualified"]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def table(rows, key, limit=256):
    require(isinstance(rows, list) and len(rows) <= limit, "CPython record table exceeds bound")
    result = {}
    for row in rows:
        require(isinstance(row, dict) and isinstance(row.get(key), str), "Invalid CPython record identity")
        require(row[key] not in result, "Duplicate CPython record identity")
        result[row[key]] = row
    return result


def checksum(package):
    rows = package.get("checksums")
    require(isinstance(rows, list) and len(rows) <= 4, "Invalid SPDX checksums")
    values = [r.get("checksumValue") for r in rows if r.get("algorithm") == "SHA256"]
    require(len(values) == 1 and isinstance(values[0], str) and len(values[0]) == 64
            and all(c in "0123456789abcdef" for c in values[0]), "Expected one SPDX SHA256")
    return values[0]


def false_flags(value):
    """No supplied wrapper can promote a qualification or trust claim."""
    if isinstance(value, dict):
        for key, child in value.items():
            if key.endswith("_qualified") or key in {
                "licensing_clearance", "cryptographic_signature_verified",
                "certificate_chain_and_identity_verified", "transparency_log_verified"}:
                require(child is False, "CPython qualification/trust flag must remain false")
            else:
                false_flags(child)
    elif isinstance(value, list):
        for child in value:
            false_flags(child)


def same_member(reader, archive, member, receipt, role, original=None):
    require(isinstance(member, str) and len(member) <= 1024 and "\\" not in member
            and not member.startswith("/") and not ({".", "..", ""} & set(member.split("/"))),
            "Invalid canonical archive member")
    data = original if original is not None else reader.archive_members(archive, [member], role)[member]
    require(len(data) <= 2 * 1024 * 1024, "CPython source/notice member exceeds bound")
    original = reader.bytes(receipt, role, limit=2 * 1024 * 1024)
    require(original == data, "CPython preserved bytes differ from original archive member")
    return reader.record(receipt, role)


def unique_records(rows):
    result = {}
    for row in rows:
        prior = result.get(row["path"])
        require(prior is None or prior == row, "Conflicting normalized CPython record")
        result[row["path"]] = row
    return [result[p] for p in sorted(result)]


def _validate_inputs(input_receipt, reader, targets):
    """Return existing source-report contributions; reader resolves every input."""
    value = reader.json(input_receipt, "native_input")
    require(value.get("schema") == "vertex-cpython-publisher-source-input-receipt-v1", "Unsupported CPython input schema")
    false_flags(value)
    require(isinstance(value.get("qualification"), dict) and value["qualification"]
            and all(v is False for v in value["qualification"].values()), "Expected false CPython qualification boundary")
    metadata = value["publisher_metadata"]["original_sbom"]
    provenance = reader.record(metadata, "metadata")
    require(provenance["sha256"] == SBOM_SHA256, "Original CPython publisher SPDX pin differs")
    original = reader.json(metadata, "metadata")
    require(original.get("documentNamespace") == EMBED_URL + ".spdx.json" and original.get("files") == [], "Wrong CPython publisher SPDX identity")
    packages = table(original["packages"], "SPDXID", 15)
    require(set(packages) == PACKAGE_IDS | {"SPDXRef-PACKAGE-cpython"}, "Original CPython dependency graph differs")
    parent = packages["SPDXRef-PACKAGE-cpython"]
    require(parent.get("name") == "CPython" and parent.get("versionInfo") == VERSION
            and parent.get("packageFileName") == EMBED_NAME and parent.get("downloadLocation") == EMBED_URL
            and checksum(parent) == EMBED_SHA256, "Wrong CPython parent package/version/archive")
    relations = original["relationships"]
    require(isinstance(relations, list) and len(relations) == 15, "CPython relationship graph differs")
    expected_edges = [{"spdxElementId": "SPDXRef-PACKAGE-cpython", "relationshipType": "DEPENDS_ON", "relatedSpdxElement": pid} for pid in sorted(PACKAGE_IDS)]
    expected_edges.append({"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES", "relatedSpdxElement": "SPDXRef-PACKAGE-cpython"})
    require(sorted(relations, key=lambda r:json.dumps(r, sort_keys=True)) == sorted(expected_edges, key=lambda r:json.dumps(r, sort_keys=True)), "CPython original relationships differ")
    binding = value["parent_archive_binding"]
    archive = binding["archive"]
    require(reader.record(archive, "binary")["sha256"] == EMBED_SHA256 and binding["publisher_package"] == parent, "Wrong CPython containing archive")
    lock = reader.json(binding["lock_file"], "recipe")
    require(lock.get("python_version") == VERSION, "Wrong CPython lock version")
    interpreters = [a for a in lock["assets"] if a.get("kind") == "interpreter"]
    require(len(interpreters) == 1 and interpreters[0] == binding["lock_asset"], "Wrong original interpreter lock binding")
    asset = interpreters[0]
    require(asset.get("filename") == EMBED_NAME and asset.get("version") == VERSION
            and asset.get("url") == EMBED_URL and asset.get("sha256") == EMBED_SHA256, "Wrong original locked embed archive")
    source_archive = value["cpython_source_archive"]
    source_record = reader.record(source_archive, "source")
    require(source_record["sha256"] == SOURCE_SHA256, "Wrong CPython source archive")
    source_assets = [a for a in lock["assets"] if a.get("name") == "CPython source" and a.get("version") == VERSION]
    require(len(source_assets) == 1 and source_assets[0].get("sha256") == SOURCE_SHA256
            and source_assets[0].get("url") == "https://www.python.org/ftp/python/" + VERSION + "/Python-" + VERSION + ".tar.xz", "Wrong original CPython source lock")

    require(isinstance(targets, list) and 0 < len(targets) <= 64, "Expected bounded CPython inventory targets")
    names = set()
    target_bindings = []
    for target in sorted(targets, key=lambda r:(r["destination"], r["path"])):
        require(set(target) == {"component_id", "path", "destination", "sha256", "bytes"}
                and target["component_id"] == "cad-cpython", "CPython target owner/tuple differs")
        member = PurePosixPath(target["destination"]).name
        require(member not in names and member not in {"vcruntime140.dll", "vcruntime140_1.dll"}
                and PurePosixPath(member).suffix in {".exe", ".dll", ".pyd"}, "Duplicate or non-CPython native target")
        names.add(member)
        data = reader.archive_members(archive, [member], "binary")[member]
        require(len(data) == target["bytes"] and hashlib.sha256(data).hexdigest() == target["sha256"], "CPython native member differs from exact inventory target")
        # Targets are authenticated by the caller's candidate inventory binding.
        # Dependency-kit replay has their tuples, not copied runtime binaries.
        target_bindings.append({**target, "archive_member": member, "checksum_origin": "locked_embed_archive_member"})

    controls = table(value["source_build_controls"], "source_archive_member", 17)
    require(set(controls) == CONTROLS, "CPython source-build control set differs")
    recipes = [reader.record(binding["lock_file"], "recipe")]
    original_controls = reader.archive_members(source_archive, sorted(controls), "source")
    for name, row in sorted(controls.items()):
        recipes.append(same_member(reader, source_archive, name, row["local_file"], "recipe", original_controls[name]))
    vendor_receipt = value["vendored_source_metadata"]["receipt"]
    vendor_record = same_member(reader, source_archive, PREFIX + "Misc/sbom.spdx.json", vendor_receipt, "metadata")
    vendor = reader.json(vendor_receipt, "metadata")
    vendor_packages = table(vendor["packages"], "SPDXID", 5)
    vendor_files = table(vendor["files"], "SPDXID", 113)
    require(len(vendor_files) == 113 and {p["name"] for p in vendor_packages.values()} == set(VENDORED_COUNTS), "Vendored CPython source graph differs")
    wrappers = value["vendored_source_metadata"]["packages"]
    require(isinstance(wrappers, list) and len(wrappers) == 5, "Vendored CPython wrapper set differs")
    wrappers = table([{**row, "id": row["package"]["SPDXID"]} for row in wrappers], "id", 5)
    require(set(wrappers) == set(vendor_packages), "Vendored package wrapper identities differ")
    used = set()
    require(isinstance(vendor["relationships"], list) and len(vendor["relationships"]) <= 256, "Vendored relationship budget exceeded")
    vendor_members = reader.archive_members(source_archive, sorted(PREFIX + row["fileName"] for row in vendor_files.values()), "source")
    for pid, package in sorted(vendor_packages.items()):
        require(wrappers[pid]["package"] == package, "Vendored package wrapper differs from original SPDX")
        edges = [e for e in vendor["relationships"] if e["spdxElementId"] == pid and e["relationshipType"] == "CONTAINS"]
        require(len(edges) == VENDORED_COUNTS[package["name"]], "Vendored package source membership differs")
        rows = []
        for edge in edges:
            fid = edge["relatedSpdxElement"]
            require(fid not in used and fid in vendor_files, "Duplicate/unknown vendored source member")
            used.add(fid)
            declared_file = vendor_files[fid]
            member = PREFIX + declared_file["fileName"]
            data = vendor_members[member]
            require(len(data) <= 2 * 1024 * 1024 and hashlib.sha256(data).hexdigest() == checksum(declared_file), "Vendored original source hash differs")
            rows.append({"member": member, "bytes": len(data), "sha256": checksum(declared_file)})
        require(sorted(rows, key=lambda r:r["member"]) == wrappers[pid]["verified_source_members"], "Vendored wrapper source table differs from original graph")
    require(used == set(vendor_files), "Unbound vendored source file")

    notices = [same_member(reader, archive, "LICENSE.txt", value["notice_evidence"]["embed_license"], "notice"),
               same_member(reader, source_archive, PREFIX + "LICENSE", value["notice_evidence"]["cpython_source_license"], "notice")]
    declared = table([{**row, "id": row["publisher_package"]["SPDXID"]} for row in value["declared_source_records"]], "id", 14)
    require(set(declared) == PACKAGE_IDS, "CPython dependency dispositions must preserve all original IDs")
    sources = [{"url": source_assets[0]["url"], "status": "exact_local_source_present", "name": "CPython",
                "binding_role": "locked_interpreter_preferred_source_archive", "local_files": [source_record], "provenance": reader.record(binding["lock_file"], "metadata")}]
    dispositions = []
    for pid in sorted(PACKAGE_IDS):
        package, wrapper = packages[pid], declared[pid]
        require(wrapper["publisher_package"] == package and wrapper["publisher_relationship"] in expected_edges, "CPython wrapper substituted original SPDX record")
        require(package.get("licenseConcluded") == "NOASSERTION", "CPython dependency license assertion differs")
        sha = checksum(package)
        availability = wrapper["source_availability"]
        unresolved = pid in UNRESOLVED
        require((availability is None) == unresolved, "CPython exact source availability set differs")
        if not unresolved:
            require(availability["url"] == package["downloadLocation"], "CPython source URL differs from original SPDX")
            source = availability["local_file"]
            canonical = reader.record(source, "source")
            require(canonical["sha256"] == sha, "CPython source archive differs from original SPDX checksum")
            evidence = table(availability["license_evidence"], "source_archive_member", 4)
            require(set(evidence) == set(LICENSE_MEMBERS[package["name"]]), "CPython original source notice set differs")
            for member, row in sorted(evidence.items()):
                notices.append(same_member(reader, source, member, row["local_file"], "notice"))
            sources.append({"url": package["downloadLocation"], "status": "exact_local_source_present", "name": package["name"],
                            "binding_role": "publisher_declared_embedded_dependency_source", "local_files": [canonical], "provenance": provenance})
        dispositions.append({"package_id": pid, "name": package["name"], "version": package["versionInfo"], "url": package["downloadLocation"],
                             "sha256": sha, "checksum_origin": "original_publisher_spdx_package", "license_concluded": "NOASSERTION",
                             "relevance": "unresolved_candidate_exclusion" if unresolved else "shipped_stdlib" if package["name"] == "macholib" else "native_source_build_control",
                             "source_build_inference": not unresolved and package["name"] != "macholib",
                             "candidate_exclusion_qualified": False, "native_members": [] if unresolved else NATIVE_ASSOCIATIONS[package["name"]]})

    runtime_rows = table(value["selected_runtime_binding"]["archive_members"], "member", 64)
    require("python313.zip" in runtime_rows, "Expected original standard-library member")
    stdlib = runtime_rows["python313.zip"]["selected_runtime_file"]
    stdlib_data = reader.archive_members(archive, ["python313.zip"], "binary")["python313.zip"]
    stdlib_record = reader.record(stdlib, "metadata")
    require(len(stdlib_data) == stdlib_record["bytes"] and hashlib.sha256(stdlib_data).hexdigest() == stdlib_record["sha256"], "Selected stdlib differs from original archive")
    macholib = table(value["stdlib_evidence"]["macholib_members"], "member", 7)
    expected_macholib = {"ctypes/macholib/" + n for n in ("dyld.pyc", "dylib.pyc", "fetch_macholib", "fetch_macholib.bat", "framework.pyc", "README.ctypes", "__init__.pyc")}
    require(set(macholib) == expected_macholib and value["stdlib_evidence"]["tkinter_members"] == [], "Selected macholib/Tk stdlib declaration differs")
    nested = reader.archive_members(stdlib, sorted(macholib), "metadata")
    require(all(len(nested[n]) == r["bytes"] and hashlib.sha256(nested[n]).hexdigest() == r["sha256"] for n,r in macholib.items()), "Shipped macholib members differ")

    # Explicit metadata delivery is required for nested stdlib replay. This is
    # the sole selected-runtime record admitted as a dependency-kit input.
    artifacts = [provenance, vendor_record, stdlib_record]
    raw = table(value["publisher_metadata"]["raw_metadata_files"], "path", 16)
    signature_records = {}
    for row in raw.values():
        name = PurePosixPath(row["path"]).name
        if name in SIGNATURE_PINS:
            require(name not in signature_records, "Duplicate original CPython signature metadata")
            signature_records[name] = row
            canonical = reader.record(row, "metadata")
            require(canonical["sha256"] == SIGNATURE_PINS[name], "Original CPython signature metadata pin differs")
            artifacts.append(canonical)
    require(not signature_records or set(signature_records) == set(SIGNATURE_PINS), "Incomplete supplied CPython signature metadata")
    if signature_records:
        bundle = reader.json(signature_records[EMBED_NAME + ".sigstore"], "metadata")
        msg = bundle["messageSignature"]
        require(base64.b64decode(msg["messageDigest"]["digest"], validate=True).hex() == EMBED_SHA256, "Sigstore declared digest differs from embed")
        logs = bundle["verificationMaterial"]["tlogEntries"]
        require(isinstance(logs, list) and len(logs) == 1, "Sigstore declared log entry count differs")
        body = json.loads(base64.b64decode(logs[0]["canonicalizedBody"], validate=True))
        require(body["spec"]["data"]["hash"]["value"] == EMBED_SHA256, "Sigstore log body's declared digest differs")
        detached = reader.bytes(signature_records[EMBED_NAME + ".sig"], "metadata", limit=4096)
        require(base64.b64decode(detached.strip(), validate=True) == base64.b64decode(msg["signature"], validate=True), "Detached Sigstore signature bytes differ")
    return {
        "sources": sources, "notices": [{**r, "content_role": "notice_text_present_review_required"} for r in unique_records(notices)],
        "artifacts": unique_records(artifacts), "recipe": {"status": "exact_local_recipe_present", "files": unique_records(recipes),
            "recipe_options": ["CPython 3.13.15 Windows AMD64 source-build controls preserved", "OpenSSL/libffi default prebuilt inputs require exact original binary-deps provenance", "Compiler, linker, toolchain and relinking evidence remain unqualified"],
            "options_role": "incomplete_original_derivation_evidence_source_controls_only"},
        "remaining": list(REMAINING), "dependency_dispositions": dispositions, "target_bindings": target_bindings}


def validate_inputs(input_receipt, reader, targets):
    """Normalize one authenticated wrapper; malformed input always refuses."""
    try:
        return _validate_inputs(input_receipt, reader, targets)
    except (KeyError, TypeError, IndexError, AttributeError) as error:
        raise ValueError("Malformed CPython publisher source input") from error
