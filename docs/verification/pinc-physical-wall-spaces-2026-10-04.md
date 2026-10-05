# Physical wall clear-space geometry checkpoint

This checkpoint supplies the geometry needed for the physical-wall portion of
PINC-002. It does not establish a new user-facing classification workflow.
The existing palette still detects measurement-linework spaces. Typed physical
room consumers, classification persistence, source-update handling and their
desktop/output qualification remain required.

## Implemented geometry

`detect_physical_wall_spaces` works from an immutable document snapshot and a
selected active physical wall. It resolves the complete property/building/floor/
layer context and effective elevation, analytically nodes original baselines,
then subtracts uncut joined physical wall footprints using the existing OCCT
engine. Presentation eye masks do not change the input. Semantic phase decisions
do; inherited and explicit hierarchy references resolve to the same context.

Clear room area excludes wall material, including open interior stubs and
isolated obstacles. Nested closed islands are excluded from their parent's clear
space while retaining the separate inner room. Physical material can split one
baseline face into multiple components. Hosted openings retain virtual continuous
room limits. This convention excludes doorway threshold material from clear room
area and is distinct from appraisal exterior floor measurement.

Returned outer loops and holes retain analytical lines/circles and exact shared
endpoints. Kernel surface area and independent analytical boundary area must
agree. Original baseline parameter intervals and physical owners, thickness,
resolved placement and relevant phase decisions remain in deterministic
provenance. The detector does not mint entities, classifications or ANSI facts.
The returned provenance is an internal API value, not a new saved-file codec.

Source, Boolean, edge, topology and provenance work are bounded. Emitted lineage
is capped at 16 MiB across returned components; budget, invalid, collapsed or
ambiguous cases fail explicitly. Large-project performance and complete geometry
coverage are not certified by these fixtures. Discovery belongs in a captured
snapshot cache, not a pointer-movement paint path.

## Actual native evidence

Root ran hidden, noninteractive Release native jobs after all native writers
returned frozen. Evidence is retained in
`artifacts/physical-wall-spaces-20261004/`:

- `initial-build.log/json`: failed on a missing complete OCCT wire type and a
  Windows `near` macro collision in the test helper. Both were corrected.
- `corrected-build.log/json`: Release build passed.
- `focused.log/json`: three existing checks passed; the new test exposed a
  shared graph rejection of separated rotated wall supports.
- `graph-corrected-build.log/json`: final affected Release build passed.
- `graph-corrected-focused.log/json`: all seven focused checks passed:
  `physical_wall_spaces`, `measurement_area_graph`, `measurement_linework_source`,
  `appraisal_area_partition`, `measurement_area_definition`, `document_wall_plan`
  and `wall_measurement`.

The twelve physical-space cases cover ordinary and unequal wall thickness,
overlapping material, unsplit T/X partitions, analytical curved rooms, nested
islands, visibility/context/phase/elevation, source-order reversal, immutable
input, hosted openings/open chains, intrusive stubs and isolated obstacles,
disconnected clear components, large coordinate translation, rotation,
collinear thickness steps, exact cyclic closure, invalid/collapsed input and
provenance limits. Independent area answers include 10.64 m² ordinary interior,
8.84 m² unequal thickness, 10.46 m² with an interior stub, 10.24 m² with an
isolated wall, and a nested parent/child pair of 78.4/14.44 m².

The graph correction proves finite segments lie strictly on the same side of
another support using an arithmetic error band before rejecting a nearly
parallel determinant. It never merges supports using the metre tolerance.
Uncertain crossings and a large-coordinate exact-collinear overlap regression
remain refused; exact shared endpoints do not bypass that uncertainty check.

One independent read-only advisor reviewed the detector and narrow graph change.
The review led to exact kernel intersection endpoints, compact relevant phase
provenance, aggregate memory limits, a valid admitted budget fixture and retention
of the original uncertain-overlap refusal. No further required source correction
remained; native verification belongs to the root evidence above.

## Delivery limits

This is a source/core checkpoint. It does not change the installed floor-reference
build or its shortcut, and no new desktop installer or UI capture is claimed.
PINC-002 still requires the typed consumer and real classification actions;
all 22 preset mappings, the other Pinc additions and original unified production
gate remain in scope. No practical user checklist item is marked passed by a
core test. Appraisal exterior GLA remains a separate calculation with recorded
eligibility evidence.
