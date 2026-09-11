# Output fingerprints

`OutputFingerprint` binds a derived output to an immutable document head and
to the dependency identities used to produce it. The implementation is in
`include/sketch/output_fingerprint.hpp` and
`src/core/output_fingerprint.cpp`. The desktop preview, draft PDF, draft SVG,
print, and native 3D pipelines still need to call it at their output boundaries.

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
The desktop now exposes a draft SVG export through the same `PlanCanvas`
vector renderer used by preview, draft PDF, and print. SVG is stamped as a
draft and is intentionally not treated as authoritative until fingerprint and
currentness checks are wired at the output boundary.
