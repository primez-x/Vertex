# Persisted sheet output scene adapter

`sheet_output_scene.hpp` supplies distinct version-1
`sketch.sheet-output-scene` and `sketch.sheet-set-output-scene` envelopes
independently of Qt and the desktop renderer. Selected-sheet creation resolves an
explicit `sheet_view_model` entity ID and sheet ID in one immutable Document
snapshot. It decodes through the existing entity codec, validating positive
finite page dimensions and viewport scales, page bounds, orthonormal view
frames, and viewport, schedule-registry and cross-sheet callout references.

The detached JSON envelope contains `schema`, `version`, `entity_id`, `sheet_id`,
`definition` and `fingerprint`. Definition is the complete canonical sheet/view
graph, including other sheets required to resolve callouts. Its arrays use the
model's ID ordering; viewport scales remain independent. Creation is deterministic
for the same snapshot and dependencies, including after ProjectStore save/reopen.
The selected sheet's dimensions are in millimetres; model coordinates are metres;
scale denominator 100 means 1:100. No navigation zoom or inferred page defaults
are introduced.

The adapter owns the fingerprint `views` dependency group. Callers leave that
input group unspecified and empty; supplying anything else fails rather than
silently dropping caller metadata. The generated resource binds the canonical
descriptor, including selection and complete definition, by SHA-256. All other
groups use the existing output-fingerprint requirements. Callers still identify
the real renderer, adapter build, fonts, profiles, CRS and application artifacts.
The descriptor and fingerprint record the adapter schema version, not a fabricated
implementation binary digest.

`check_sheet_output_scene_current` validates envelope shape/version, graph,
selection, fingerprint digest and descriptor binding before recomputing from the
current snapshot. It returns valid/current and changed fingerprint groups. A
changed valid sheet scale reports stale `document` and `views`; changed external
dependencies name their group. Invalid envelopes, references, dependency inputs,
or a selection no longer resolvable in the current snapshot return `valid=false`
with an error. Equivalent reordered definition arrays normalize before comparison.
Fingerprint integrity is not authentication. Callers must require both `valid`
and `current` before treating this as evidence for current output.

The set envelope carries `sheet_ids` instead of `sheet_id`. Those identities
must exactly match the model's complete persisted page order. Its fingerprint
therefore changes when a page is added, removed, reordered, or edited and is
independent of the locally selected single-sheet page. The desktop uses this
contract for ordered multipage PDF export and drawing-set print preview; the
selected-sheet schema and behavior remain unchanged.

This is a semantic output plan and validator, not a geometry scene renderer or
printer calibration check.
It does not resolve schedule contents or prove output fidelity. It does not
persist a derived scene back into Document or change ProjectStore's format.
The standalone multipage-project model is not persisted in Document, so it is
deliberately not conflated with coordinated drawing sheets. The definition
includes project/title metadata; unlike the compact fingerprint, the envelope
is not a metadata-minimized export. Untrusted serialized bytes need caller-side
size limits before parsing; no separate streaming parser budget is supplied.
