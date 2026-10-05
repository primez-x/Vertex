# Physical room repair: next implementation

Status: design discovery only. The installed creation/stale checkpoint does not
implement this plan. This work remains part of PINC-002 and the unified release
scope, not a separate product or reduced acceptance gate.

## Outcome

An explicit repair command lets the user select a retained physical room and
choose a freshly detected clear-space destination. Preview shows the old and new
space, hole deductions and affected references. Apply retains the room ID, name,
classification and unrelated metadata in one reversible revision. It records a
reviewed reassignment; it does not assert automatic geometric correspondence.

A split assigns the retained room only to the explicitly chosen component.
Other pieces remain unclassified. A merge does not acquire other owners' facts
or classifications. Other affected owners require visible, explicit dispositions.
Automatic one-to-one repair and atomic multi-room dispositions remain required
subsequent lifecycle work.

## Geometry and dependency prerequisite

Read-only discovery found that an entity-map overload alone leaves cycles through
project organization, document-wall projection and geometry operations. Extract
only the lower geometry algorithms needed by exact replay:

1. Move the bare closed-face walker and private analytical helpers into a
   precision-only topology module. Keep its existing public API compatible.
2. Move wall joining/contact into a Document-free network module accepting
   admitted physical walls and contact-domain keys. Retain context/elevation
   compatibility in the document admission wrapper.
3. Move bounded subtraction, analytical loop extraction and canonical ordering
   into a physical clear kernel depending on those modules and architecture.
4. Compile a source adapter into Document. It resolves entity-map context,
   effective plane and semantic phases, invokes the kernel and assembles exact
   lineage. Snapshot discovery calls that same adapter.

No lower module may link Document. Document source remains free of OCCT headers;
geometry-capable binaries require the native geometry runtime. Verify actual
supported configuration dependencies before changing CMake. A kernel-only link
check must prove no hidden Document dependency. Do not use caller-installed
validator callbacks or duplicate the geometry algorithm.

## Typed authority

The repair API takes the room ID, selected current source wall, strictly interior
witness, exact reviewed destination lineage, expected old descriptor digest and
explicit reference decisions. Fresh detection must reproduce outer, holes and
lineage. A component/face index, centroid, overlap score or caller-supplied area
cannot establish identity or authorize geometry.

Extend the existing boundary-redefinition dialect with a strict versioned repair
intent, fresh topology and no unrelated wall/linework replacement lanes or
classification overrides. Runtime apply and retained-history restore independently
rederive the destination from the preceding revision's entity map. Only that
verified typed result receives an exception to the marker/outline transition
fence. Reject stale revisions, forged geometry, changed reviewed lineage,
inactive sources, unresolved context and another current room's destination.

Mint fresh analytical edge/vertex IDs where topology changes; never map by edge
position. Use explicit existing child/reference mapping or removal contracts.
Unsupported dependencies block repair with an explanation. Keep source-room
Entity-only dimension resolution refused until its snapshot-aware implementation.
Regenerate inline holes analytically and preserve original evidence in history.

Reserve the next native/extraction floors for retained repair intent only after
final codec design. Update parsing, scanning, runtime replay, restore and package
validation together. Older unsupported readers must refuse before accepting the
history. Do not promise read-only opaque repair-history loading without an actual
opaque-history implementation.

## Ownership and verification

Root owns CMake, typed command/history integration, reader floors, desktop review,
generators, all native jobs, installation and Git. Geometry delegates receive
exclusive bounded extraction/kernel paths; no native job starts until all writers
return frozen. Existing interfaces and exact detector behavior must be preserved
before adding repair authority.

Focused cases must cover thickness/movement, reordered inputs, curves, holes,
stubs, source replacement, material-only component splits, baseline splits,
merges, target conflicts, forged witness/lineage/outer/holes, phase/context,
dependency decisions, identity lifetime, Undo/Redo/fork and native save/reopen
tamper refusal. Desktop cases must exercise preview, cancellation and actual
destination picking. Existing clear-room, wall-plan, face-graph, quantity and
exchange checks guard the extraction. Package only after the affected checks
pass; manual checklist outcomes remain user-owned.
