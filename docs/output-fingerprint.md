# Output fingerprints

`OutputFingerprint` binds a derived output to an immutable document head and
to the dependency identities used to produce it. The implementation is in
`include/sketch/output_fingerprint.hpp` and
`src/core/output_fingerprint.cpp`. Draft PDF, draft SVG, and native 3D image
exports now write an adjacent `<output>.fingerprint.json` manifest generated
from the same document snapshot and view state used for rendering. The sidecar
also records `output_sha256`, the digest of the completed output bytes, so a
consumer can reject a sidecar paired with a modified or different output file.
Print
preview runs the same fingerprint gate before opening and when it paints, and
its local driver-evidence receipt carries the serialized output fingerprint
used for that page. The receipt remains preview-driver evidence only; it does
not claim that a physical print has been accepted.

Native 3D image exports also carry the same visible draft/checkpoint stamp as
the drawing outputs, including the active view-filter warning when applicable.
A Qt image post-process adds a white footer with red text beneath the native
framebuffer, preserving the model pixels and PNG transparency. It keeps the
detected image encoding; lossy formats still incur their normal re-encoding.
The image is staged and stamped before atomic destination replacement, and
read/stamp/write failures block successful export. The adjacent fingerprint
manifest remains part of the export. Desktop smoke tests cover visible footer
pixels, preservation of the original RGBA region, and read/write failures;
these helper checks do not qualify native framebuffer rendering or production
output.

Version 1 hashes a compact canonical JSON manifest. Its serialized document
summary contains only:

* `document_id` and `revision`;
* deterministic `entity_count` and `asset_count`; and
* `head_content_sha256`, a digest of snapshot-derived canonical head material.

The snapshot-derived material includes every head entity's ID, type, required flag,
properties, and extensions. It also includes every asset's ID, media type,
metadata, and a SHA-256 digest freshly computed from the asset bytes. The
asset's declared `Asset::sha256` must be a valid lower-case digest and must
match that fresh computation. Entity fields and raw asset bytes are used to
form the digest, then discarded; they are never copied into the serialized
fingerprint. Caller-supplied dependency IDs, metadata, and reasons are
serialized verbatim. Callers must provide only the intended public dependency
metadata; this utility does not sanitize or redact arbitrary payloads.

The manifest has explicit dependency groups for `profiles`, `fonts`, `views`,
`crs`, `processing_components`, and `application_build`. A resource has a
non-empty ID, a lower-case SHA-256 digest, and finite object metadata. Resource
arrays are sorted by ID before serialization, and duplicate IDs are rejected.
An empty ordinary group must use `no_resource` or `not_applicable` with a
non-empty reason; an unspecified state is rejected. `application_build` must
contain at least one actual build resource digest. `processing_components`
must cover `kernel`, `solver`, `renderer`, and `adapters`; each role supplies
resources or an explicit `not_applicable` reason, and a renderer resource is
mandatory. A `fonts` `no_resource` sentinel is allowed when the caller has
declared that the output contains no text.

The desktop workflow binds the shipped `Inter.ttf` bytes into the `fonts`
group, so text output cannot silently depend on an installed system font. It
also binds the effective view state used for the page, including hidden floor
and layer IDs and the project unit system, into `views`. The sheet-output
adapter may append its persisted scene resource alongside these caller-owned
view resources; changing an effective visibility mask therefore invalidates
the output fingerprint even when the document head is unchanged.

The desktop processing roles bind the complete local runtime dependency set.
For an installed package this set is read from the adjacent verified
`runtime-manifest.json`; every declared runtime binary is re-hashed from the
installed bytes before its resource is added. Source builds without an
installed manifest bind all application-local DLLs as developer evidence and
use an explicit fallback only when no such DLL is co-located. A changed Qt,
Open CASCADE, PlaneGCS, adapter, worker, or other packaged runtime binary
therefore invalidates the fingerprint instead of being mislabeled as static.
The application executable remains a separate `application_build` resource.

The public workflow is:

```cpp
const auto fingerprint =
    sketch::make_output_fingerprint(snapshot, dependency_inputs);
const auto encoded = sketch::serialize_output_fingerprint(fingerprint);
const auto restored = sketch::deserialize_output_fingerprint(encoded, &error);
const auto status = sketch::check_output_fingerprint_current(
    fingerprint, newer_snapshot, newer_dependency_inputs);
```

`check_output_fingerprint_current` rebuilds the candidate manifest. It reports
`valid=false` for an invalid stored manifest or invalid new inputs. For valid
inputs it reports `current=false` and names changed top-level groups such as
`document`, `fonts`, or `processing_components`; an unchanged fingerprint
reports `current=true` with no changed groups.

Object keys use nlohmann JSON's ordered object representation and are emitted
with `dump()` without whitespace. Entity and asset collections are traversed
from the document's sorted maps, and dependency resources are explicitly
sorted by ID. JSON arrays preserve their order because array order is treated
as semantic. Floating values are hashed according to their nlohmann JSON text
representation; independent producers must use the same representation and
should avoid claiming cross-language numeric normalization. Non-finite JSON
numbers are rejected recursively. Build and dependency identity is supplied
by the producing pipeline as content digests plus metadata; discovering,
hashing, and qualifying those artifacts remains its responsibility.

The envelope stores `schema_version`, the compact `manifest`, and
`digest_sha256`. The digest covers only the canonical manifest and provides
integrity identity, not authentication. Deserialization validates the complete
nested manifest shape, all dependency states, role coverage, resource hashes,
and the envelope digest. It cannot re-hash asset bytes because the bytes are
deliberately absent; that guarantee is established by
`make_output_fingerprint` and by currentness checks against a live snapshot.
The desktop exposes a draft SVG export through the same `PlanCanvas` vector
renderer used by preview, draft PDF, and print. SVG is stamped as a draft and
its sidecar records the document head, page/filter view descriptor, linked
processing roles, running Windows executable digest, and output-byte digest.
Consumers must compare the sidecar's `output_sha256` with a fresh SHA-256 of
the output before using the serialized fingerprint as currentness evidence;
`check_output_fingerprint_current` then checks the document and dependency
manifest. The output remains a draft until production output qualification and
printer evidence are complete.
