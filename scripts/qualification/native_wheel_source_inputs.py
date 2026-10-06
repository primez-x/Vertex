"""Bounded selected-wheel evidence normalization through a caller-owned reader.

The input JSON has schema ``vertex.native-wheel-inputs.v1``, profile ``geos`` or
``openblas``, a ``files`` mapping of role to file receipt, and ``notices`` rows
containing ``archive_role``, ``member`` and ``file``. Receipts are resolved only
by reader.json/bytes/archive_members/record; this module performs no I/O.
These profiles preserve existing cad-shapely/cad-numpy inventory ownership.
Presence and byte identity do not confer source derivation or licensing rights.
"""
import base64
import hashlib
import json
import re

OPENBLAS_REVISION = "446c436e10450c348169808f1a6b3fae0925c9f7"
OPENBLAS_RECIPE_REVISION = "78fc0eaf6de71d92fd98f74f7e5bfa356a2f83e3"
GEOS_SHA512 = "38a6d8bb05b374160c6e5eb82e5f601915ee44e75bdba0414bb7b1096a62f3cdfbb877389998bd3ff4c77c98927ce95a8d8298dd599a0fcb8ea0e83f174f1744"
PINNED_SHA256 = {
    "shapely_wheel": "ca2591bff6645c216695bdf1614fca9c82ea1144d4a7591a466fef64f28f0715",
    "numpy_wheel": "71cad2b2a7451ab79d8f5e71b453485b6775963d5cf794179144a7463fe6e8ec",
    "openblas_wheel": "ea34bf76b8427ac2eca7a99334cbd8937a040303fa8364f9ed8e667d7c8e2c7d",
    "geos_source": "df2c50503295f325e7c8d7b783aca8ba4773919cde984193850cf9e361dfd28c",
    "shapely_recipe": "adbd4060635098d5109e04668edcc015117a133c39da2899cc3ce9b665b76439",
    "openblas_source": "ddecf3872f66f556aed35dabe9e04e663f014b05052b989288f28fc9a883afe0",
    "openblas_recipe": "ce4197b8d3c006370d3ee0f4e2d9d4940480b649a62fa209a405d419666cfb6b",
    "openblas_source_tree": "14d54cf621e35e35474ee1caf567fce71826423b0e13386c2b4f11870e716541",
    "openblas_recipe_tree": "06bc251325fa4d0c0fb821b636d57d7d0516706797302d9ec300213d41b2e238",
    "shapely_pypi": "e559348690028da6788cba77f912cdd533d3412a3331dea89f97d163f775e4b7",
    "numpy_pypi": "497ade587d7c2cad83a918ca79b9776dbde9eddf4c0abddbfb637b27160e87a9",
    "openblas_pypi": "4aef004221017f2f2ae1477a11c9266174092f332ba835b15e0673d288fa7a14",
    "openblas_provenance": "dce90e54776caffadc3a6b5a35fedc81958be41edd15c07d2c5a30497d6bf5ed",
}
GEOS_MEMBERS = (
    "shapely.libs/geos-ae6efa0782962b98e358f10ea539ae5f.dll",
    "shapely.libs/geos_c-072b7a9224d16d3e4ab2395bb855b2d3.dll",
)
NUMPY_DLL = "numpy.libs/libscipy_openblas64_-ed4f167a5330424524f45258e7ca2c8d.dll"
UPSTREAM_DLL = "scipy_openblas64/lib/libscipy_openblas64_.dll"
OPENBLAS_PREFIX = "OpenBLAS-" + OPENBLAS_REVISION + "/"
RECIPE_PREFIX = "openblas-libs-" + OPENBLAS_RECIPE_REVISION + "/"
MAX_NOTICES = 64
MAX_TREE_ROWS = 20000
MAX_BLOB_BYTES = 256 << 20
READER_ROLES = {"parent_wheel": "binary", "upstream_wheel": "binary", "parent_pypi": "metadata",
                "upstream_pypi": "metadata", "source_archive": "source", "recipe_archive": "recipe",
                "source_tree": "metadata", "recipe_tree": "metadata", "provenance": "metadata"}


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def _member_name(value):
    _require(isinstance(value, str) and len(value) <= 512 and value.isascii(), "invalid member name")
    _require(not value.startswith("/") and "\\" not in value and ":" not in value,
             "invalid member path")
    _require(all(part not in ("", ".", "..") for part in value.split("/")), "invalid member path")
    return value


class _Inputs:
    def __init__(self, input_receipt, reader, profile, roles):
        self.reader = reader
        self.provenance = reader.record(input_receipt, "native_input")
        value = reader.json(input_receipt, "native_input")
        _require(isinstance(value, dict) and set(value) == {"schema", "profile", "files", "notices"},
                 "invalid native input fields")
        _require(value["schema"] == "vertex.native-wheel-inputs.v1" and value["profile"] == profile,
                 "wrong native input profile")
        _require(isinstance(value["files"], dict) and set(value["files"]) == set(roles),
                 "missing or unexpected native input roles")
        self.files = value["files"]
        self.records = {role: reader.record(value["files"][role], READER_ROLES[role]) for role in sorted(roles)}
        _require(isinstance(value["notices"], list) and len(value["notices"]) <= MAX_NOTICES,
                 "invalid native notice table")
        self.notices = value["notices"]

    def data(self, role, limit=32 << 20):
        data = self.reader.bytes(self.files[role], READER_ROLES[role], limit=limit)
        _require(isinstance(data, bytes) and len(data) <= limit, "invalid reader bytes")
        record = self.records[role]
        _require(len(data) == record["bytes"] and _sha(data) == record["sha256"], "changed native input bytes")
        return data

    def json(self, role):
        value = self.reader.json(self.files[role], READER_ROLES[role])
        _require(isinstance(value, dict), "invalid native metadata object")
        return value

    def pinned(self, role, pin):
        data = self.data(role)
        _require(_sha(data) == PINNED_SHA256[pin], "wrong pinned native archive: " + role)
        return data

    def members(self, role, names, *, bulk=False):
        _require(len(names) <= (MAX_TREE_ROWS if bulk else 128)
                 and len(set(names)) == len(names), "invalid member selection")
        names = [_member_name(name) for name in names]
        result = self.reader.archive_members(self.files[role], names, READER_ROLES[role])
        _require(isinstance(result, dict) and set(result) == set(names), "missing native archive member")
        _require(all(isinstance(data, bytes) and len(data) <= 32 << 20 for data in result.values()),
                 "native archive member exceeds bound")
        _require(sum(map(len, result.values())) <= (MAX_BLOB_BYTES if bulk else 64 << 20),
                 "native member selection exceeds bound")
        return result

    def notice_records(self, required):
        seen = set()
        result = []
        for row in self.notices:
            _require(isinstance(row, dict) and set(row) == {"archive_role", "member", "file"},
                     "invalid native notice fields")
            role, name = row["archive_role"], _member_name(row["member"])
            _require(isinstance(role, str) and role in {"parent_wheel", "upstream_wheel", "source_archive", "recipe_archive"}
                     and role in self.files, "invalid notice archive role")
            _require(re.search(r"/(?:LICENSE[^/]*|COPYING[^/]*|AUTHORS)$", name), "invalid notice member")
            pair = (role, name)
            _require(pair not in seen, "duplicate native notice")
            seen.add(pair)
            original = self.members(role, [name])[name]
            retained = self.reader.bytes(row["file"], "notice", limit=1 << 20)
            record = self.reader.record(row["file"], "notice")
            _require(original == retained and record["bytes"] == len(retained)
                     and record["sha256"] == _sha(retained), "changed native notice member bytes")
            _require(bool(retained.strip()), "empty native notice")
            result.append({**record, "content_role": "notice_text_present_review_required"})
        _require(set(required) <= seen, "missing required native notices")
        return sorted(result, key=lambda item: item["path"])


def _pypi_wheel(inputs, metadata_role, wheel_role, project, version, filename, pin):
    # Preserve the exact acquired publisher response independently of mutable
    # wrapper receipts; matching a few semantic fields is not original evidence.
    metadata_pin = {"shapely_wheel": "shapely_pypi", "numpy_wheel": "numpy_pypi",
                    "openblas_wheel": "openblas_pypi"}[pin]
    inputs.pinned(metadata_role, metadata_pin)
    metadata = inputs.json(metadata_role)
    info, rows = metadata.get("info"), metadata.get("urls")
    _require(isinstance(info, dict) and isinstance(info.get("name"), str)
             and info["name"].replace("_", "-").lower() == project
             and info.get("version") == version, "wrong parent project or version")
    _require(isinstance(rows, list) and len(rows) <= 512, "invalid PyPI file table")
    selected = [row for row in rows if isinstance(row, dict) and row.get("filename") == filename]
    _require(len(selected) == 1, "missing or ambiguous exact wheel platform")
    row = selected[0]
    _require(row.get("packagetype") == "bdist_wheel" and type(row.get("size")) is int,
             "invalid PyPI wheel metadata")
    _require(isinstance(row.get("digests"), dict) and row["digests"].get("sha256") == PINNED_SHA256[pin],
             "wrong pinned publisher wheel checksum")
    _require(isinstance(row.get("url"), str) and row["url"].startswith("https://files.pythonhosted.org/")
             and row["url"].rsplit("/", 1)[-1] == filename, "wrong original wheel URL")
    data = inputs.pinned(wheel_role, pin)
    _require(row["size"] == len(data), "publisher wheel size mismatch")
    return row["url"]


def _targets(targets, owner, members):
    _require(isinstance(targets, (list, tuple)) and 0 < len(targets) <= 4096, "invalid native targets")
    for row in targets:
        _require(isinstance(row, dict) and row.get("component_id") == owner, "wrong native inventory owner")
        _require(all(isinstance(row.get(key), str) for key in ("path", "destination", "sha256"))
                 and type(row.get("bytes")) is int and row["bytes"] >= 0, "invalid native target tuple")
    result = []
    for name, data in sorted(members.items()):
        matches = [row for row in targets if row["path"].endswith("/" + name)
                   and row["destination"].endswith("/" + name)]
        _require(len(matches) == 1, "missing or ambiguous exact native target")
        row = matches[0]
        _require(row["sha256"] == _sha(data) and row["bytes"] == len(data), "changed selected native member")
        result.append({key: row[key] for key in ("component_id", "path", "destination", "sha256", "bytes")})
    return result


def _source(inputs, role, name, url, boundary):
    return {"url": url, "status": "exact_local_source_present", "local_files": [inputs.records[role]],
            "name": name, "binding_role": boundary, "provenance": inputs.provenance}


def _contribution(inputs, sources, notices, options, remaining, disposition, bindings):
    return {"sources": sources, "notices": notices,
            "artifacts": [inputs.provenance] + [inputs.records[role] for role in sorted(inputs.records)
                           if role not in ("source_archive", "recipe_archive")],
            "recipe": {"status": "exact_local_recipe_present", "files": [inputs.records["recipe_archive"]],
                       "recipe_options": options,
                       "options_role": "Declared publisher recipe controls; actual expanded options, exact toolchain and source-to-binary derivation remain unverified."},
            "remaining": remaining, "dependency_dispositions": [disposition], "target_bindings": bindings}


def _bindings(inputs, bound, names, upstream=False):
    result = []
    for target, name in zip(bound, sorted(names)):
        row = {"target": target, "archive": inputs.records["parent_wheel"], "member": name,
               "byte_identity_verified": True, "publisher_authenticated": False, "signature_verified": False,
               "source_derivation_verified": False, "rights_qualified": False, "offline_rebuild_qualified": False}
        if upstream:
            row.update(upstream_archive=inputs.records["upstream_wheel"], upstream_member=UPSTREAM_DLL)
        result.append(row)
    return result


def validate_geos_inputs(input_receipt, reader, targets):
    """Bind selected Shapely GEOS to exact sources and original notices."""
    inputs = _Inputs(input_receipt, reader, "geos", ("parent_wheel", "parent_pypi", "source_archive", "recipe_archive"))
    wheel_url = _pypi_wheel(inputs, "parent_pypi", "parent_wheel", "shapely", "2.1.2",
                           "shapely-2.1.2-cp313-cp313-win_amd64.whl", "shapely_wheel")
    source = inputs.pinned("source_archive", "geos_source")
    _require(hashlib.sha512(source).hexdigest() == GEOS_SHA512, "assigned GEOS checksum mismatch")
    inputs.pinned("recipe_archive", "shapely_recipe")
    binaries = inputs.members("parent_wheel", GEOS_MEMBERS)
    bound = _targets(targets, "cad-shapely", binaries)
    _require(b"3.13.1\0" in binaries[GEOS_MEMBERS[0]] and b"3.13.1-CAPI-1.19.2\0" in binaries[GEOS_MEMBERS[1]],
             "missing raw GEOS version markers")
    recipe_names = ("shapely-2.1.2/.github/workflows/release.yml", "shapely-2.1.2/ci/install_geos.cmd",
                    "shapely-2.1.2/ci/wheelbuilder/LICENSE_GEOS")
    recipe = inputs.members("recipe_archive", recipe_names)
    release, install = recipe[recipe_names[0]], recipe[recipe_names[1]]
    _require(b'GEOS_VERSION: "3.13.1"' in release and b"windows-2022" in release
             and b"msvc_arch: x64" in release and b"delvewheel repair" in release,
             "wrong GEOS publisher build recipe")
    _require(b"cmake -GNinja" in install and b"CMAKE_BUILD_TYPE=Release" in install,
             "missing GEOS Windows build controls")
    wheel_notice_name = "shapely-2.1.2.dist-info/licenses/LICENSE_GEOS"
    wheel_notice = inputs.members("parent_wheel", [wheel_notice_name])[wheel_notice_name]
    _require(wheel_notice.replace(b"\r\n", b"\n") == recipe[recipe_names[2]].replace(b"\r\n", b"\n"),
             "GEOS tagged and wheel notices disagree")
    notices = inputs.notice_records({("parent_wheel", wheel_notice_name),
                                     ("parent_wheel", "shapely-2.1.2.dist-info/licenses/LICENSE_win32"),
                                     ("source_archive", "geos-3.13.1/COPYING"), ("recipe_archive", recipe_names[2])})
    disposition = {"component_id": "cad-shapely", "dependency": "GEOS", "version": "3.13.1",
                   "targets": bound, "parent_wheel_url": wheel_url, "checksum_origin": "assignment_pin",
                   "publisher_authenticated": False, "signature_verified": False,
                   "source_derivation_verified": False, "rights_qualified": False, "offline_rebuild_qualified": False,
                   "inference": "Raw DLL version markers and tagged recipe identify the source candidate; exact GEOS build derivation is unverified."}
    return _contribution(inputs, [_source(inputs, "source_archive", "GEOS 3.13.1",
            "https://download.osgeo.org/geos/geos-3.13.1.tar.bz2", "Assigned checksum pin; exact selected DLL build derivation remains unverified.")],
            notices, ["GEOS_VERSION=3.13.1", "windows-2022; MSVC x64", "CMake Ninja; CMAKE_BUILD_TYPE=Release", "delvewheel repair"],
            ["GEOS publisher checksum bytes and exact DLL build derivation remain unverified.",
             "Accompanying Microsoft runtime terms and distribution rights remain unqualified.",
             "Exact toolchain inputs and a clean offline rebuild remain unqualified."], disposition,
            _bindings(inputs, bound, binaries))


def _tree(inputs, role, revision, archive_role, prefix, pin, expected_gitlinks):
    # The immutable original tree pin establishes complete membership without
    # trusting a rehashed wrapper that could omit or substitute blob rows.
    inputs.pinned(role, pin)
    tree = inputs.json(role)
    _require(tree.get("sha") == revision and tree.get("truncated") is False,
             "wrong or truncated exact native source tree")
    rows = tree.get("tree")
    _require(isinstance(rows, list) and len(rows) <= MAX_TREE_ROWS, "invalid native tree table")
    paths = set()
    gitlinks = []
    blobs = []
    for row in rows:
        _require(isinstance(row, dict), "invalid native tree entry")
        path = _member_name(row.get("path"))
        _require(path not in paths and isinstance(row.get("sha"), str)
                 and re.fullmatch(r"[0-9a-f]{40}", row["sha"]),
                 "invalid or duplicate native tree identity")
        paths.add(path)
        mode, kind = row.get("mode"), row.get("type")
        _require(isinstance(mode, str) and isinstance(kind, str), "invalid native tree mode/type")
        if mode in ("100644", "100755"):
            _require(kind == "blob" and type(row.get("size")) is int
                     and 0 <= row["size"] <= 32 << 20, "invalid native file blob mode/type/size")
            blobs.append(row)
        elif mode == "040000":
            _require(kind == "tree", "invalid native directory mode/type")
        elif mode == "160000":
            _require(kind == "commit", "invalid native gitlink")
            gitlinks.append({"path": path, "sha": row["sha"]})
        elif mode == "120000":
            # Neither selected original tree has symlinks. The caller's archive
            # reader returns ordinary files only; do not silently follow links.
            raise ValueError("symlink blobs are not present in the selected native profile")
        else:
            raise ValueError("unsupported native tree mode/type")
    _require(gitlinks == expected_gitlinks, "missing, changed or unclosed native source gitlink")
    _require(blobs and sum(row["size"] for row in blobs) <= MAX_BLOB_BYTES,
             "native Git blob selection exceeds bound")
    members = inputs.members(archive_role, [prefix + row["path"] for row in blobs], bulk=True)
    for row in blobs:
        data = members[prefix + row["path"]]
        framed = b"blob " + str(len(data)).encode("ascii") + b"\0" + data
        _require(len(data) == row["size"] and hashlib.sha1(framed).hexdigest() == row["sha"],
                 "native archive Git blob bytes/hash differ")
    return len(blobs)


def _attestation(inputs):
    inputs.pinned("provenance", "openblas_provenance")
    value = inputs.json("provenance")
    bundles = value.get("attestation_bundles")
    _require(value.get("version") == 1 and isinstance(bundles, list) and len(bundles) == 1,
             "invalid native publisher attestation")
    bundle = bundles[0]
    _require(isinstance(bundle, dict) and bundle.get("publisher") == {
        "environment": "pypi", "kind": "GitHub", "repository": "MacPython/openblas-libs", "workflow": "publish.yml"},
        "wrong native publisher attestation identity")
    attestations = bundle.get("attestations")
    _require(isinstance(attestations, list) and len(attestations) == 1, "ambiguous native attestation")
    try:
        encoded = attestations[0]["envelope"]["statement"]
        _require(isinstance(encoded, str) and len(encoded) <= 65536, "oversized native attestation subject")
        statement = json.loads(base64.b64decode(encoded, validate=True))
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError("invalid native attestation statement") from error
    _require(isinstance(statement, dict) and statement.get("subject") == [{"name": "scipy_openblas64-0.3.34.106.0-py3-none-win_amd64.whl",
             "digest": {"sha256": PINNED_SHA256["openblas_wheel"]}}], "native attestation subject mismatch")


def validate_openblas_inputs(input_receipt, reader, targets):
    """Bind NumPy's selected OpenBLAS bytes, exact gitlink and original notices."""
    roles = ("parent_wheel", "parent_pypi", "upstream_wheel", "upstream_pypi", "source_archive",
             "source_tree", "recipe_archive", "recipe_tree", "provenance")
    inputs = _Inputs(input_receipt, reader, "openblas", roles)
    parent_url = _pypi_wheel(inputs, "parent_pypi", "parent_wheel", "numpy", "2.5.3",
                            "numpy-2.5.3-cp313-cp313-win_amd64.whl", "numpy_wheel")
    upstream_url = _pypi_wheel(inputs, "upstream_pypi", "upstream_wheel", "scipy-openblas64", "0.3.34.106.0",
                              "scipy_openblas64-0.3.34.106.0-py3-none-win_amd64.whl", "openblas_wheel")
    inputs.pinned("source_archive", "openblas_source")
    inputs.pinned("recipe_archive", "openblas_recipe")
    binaries = inputs.members("parent_wheel", [NUMPY_DLL])
    upstream_binary = inputs.members("upstream_wheel", [UPSTREAM_DLL])[UPSTREAM_DLL]
    _require(binaries[NUMPY_DLL] == upstream_binary, "upstream and selected OpenBLAS DLL bytes differ")
    bound = _targets(targets, "cad-numpy", binaries)
    source_blob_count = _tree(inputs, "source_tree", OPENBLAS_REVISION, "source_archive", OPENBLAS_PREFIX,
                             "openblas_source_tree", [])
    recipe_blob_count = _tree(inputs, "recipe_tree", OPENBLAS_RECIPE_REVISION, "recipe_archive", RECIPE_PREFIX,
                             "openblas_recipe_tree", [{"path": "OpenBLAS", "sha": OPENBLAS_REVISION}])
    recipe_names = [RECIPE_PREFIX + name for name in ("openblas_commit.txt", ".gitmodules", "pyproject.toml",
                    "tools/build_steps_windows.sh", ".github/workflows/windows.yml", "patches-windows/openblas-make-libs.patch")]
    recipe = inputs.members("recipe_archive", recipe_names)
    _require(recipe[recipe_names[0]].strip() == b"v0.3.34-106-g446c436e", "wrong recipe OpenBLAS revision")
    _require(b"path = OpenBLAS" in recipe[recipe_names[1]] and b"https://github.com/xianyi/OpenBLAS.git" in recipe[recipe_names[1]],
             "wrong publisher submodule origin")
    _require(b'name = "scipy-openblas64"' in recipe[recipe_names[2]]
             and b'version = "0.3.34.106.0"' in recipe[recipe_names[2]], "wrong publisher OpenBLAS version")
    build, workflow = recipe[recipe_names[3]], recipe[recipe_names[4]]
    _require(all(token in build for token in (b"INTERFACE64=1", b"SYMBOLSUFFIX=64_", b"SYMBOLPREFIX=scipy_",
             b"USE_THREAD=1", b"USE_OPENMP=0", b"openblas-make-libs.patch"))
             and b"-static" in workflow and b"-static-libgcc" in workflow and bool(recipe[recipe_names[5]].strip()),
             "missing OpenBLAS Windows build controls")
    _attestation(inputs)
    notices = inputs.notice_records({("parent_wheel", "numpy-2.5.3.dist-info/licenses/LICENSE.txt"),
          ("upstream_wheel", "scipy_openblas64-0.3.34.106.0.dist-info/licenses/LICENSE.txt"),
          ("source_archive", OPENBLAS_PREFIX + "LICENSE"), ("source_archive", OPENBLAS_PREFIX + "lapack-netlib/LICENSE"),
          ("recipe_archive", RECIPE_PREFIX + "LICENSE.txt"), ("recipe_archive", RECIPE_PREFIX + "tools/LICENSE_win32.txt")})
    disposition = {"component_id": "cad-numpy", "dependency": "OpenBLAS", "revision": OPENBLAS_REVISION,
          "recipe_revision": OPENBLAS_RECIPE_REVISION, "targets": bound, "parent_wheel_url": parent_url,
          "upstream_wheel_url": upstream_url, "upstream_dll_byte_identity": True,
          "source_gitlinks": [], "publisher_gitlink": OPENBLAS_REVISION, "checksum_origin": "pypi_original_metadata_and_pinned_source_archives",
          "source_git_blobs_verified": source_blob_count, "recipe_git_blobs_verified": recipe_blob_count,
          "publisher_authenticated": False, "signature_verified": False, "source_derivation_verified": False,
          "rights_qualified": False, "offline_rebuild_qualified": False,
          "inference": "Publisher recipe declares this full source revision; source-to-binary reproduction remains unverified."}
    return _contribution(inputs, [_source(inputs, "source_archive", "OpenBLAS " + OPENBLAS_REVISION,
          "https://codeload.github.com/OpenMathLib/OpenBLAS/tar.gz/" + OPENBLAS_REVISION,
          "Exact selected upstream DLL byte identity and publisher gitlink; source-to-binary derivation unverified.")],
          notices, ["OpenBLAS v0.3.34-106-g446c436e", "INTERFACE64=1; SYMBOLSUFFIX=64_; SYMBOLPREFIX=scipy_",
                    "USE_THREAD=1; USE_OPENMP=0", "Windows openblas-make-libs.patch", "LDFLAGS=-lucrt -static -static-libgcc"],
          ["Publisher attestation signature, chain and transparency verification remain unperformed.",
           "Static GCC and Fortran runtime inputs, accompanying Microsoft terms and distribution rights remain unqualified.",
           "Exact toolchain inputs, source-to-binary derivation and a clean offline rebuild remain unqualified."], disposition,
          _bindings(inputs, bound, binaries, upstream=True))
