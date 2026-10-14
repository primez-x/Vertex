# Exact directional component resizing

This implements the existing requirement for horizontal/vertical canvas handles
and editable component dimensions. It does not change the production scope or
make qualification optional.

October 13 qualification update: the Claude handoff supersedes source-only
steering. The Release build and automated checks recorded in
[delivery progress](../delivery/progress.md) now cover the integrated sources.
Commit `9e61f314` adds closed coordinated directional/mapped slab authority and
its persistence guards. Complete mixed-family canvas resizing and its native
interaction qualification remain required. The dated source notes below describe
their original implementation state; they do not override the current plan.

## Authoritative geometry

Keep each assembly profile's admitted straight/circular source rings. Represent
their effective XY geometry with an optional anchored affine map:

`q = target_origin + M * (p - source_origin)`

The map contains two metre-coordinate origins and four row-major coefficients.
It must be finite, invertible, orientation-preserving and numerically
representable. Node placement continues to own reflection, yaw, uniform scale
and vertical scale. An absent map is identity. All rings and cutouts of one
profile share its map; extrusion elevation and height remain independent.

An affine image of a circular arc is an exact elliptical arc. It must never be
represented by transformed endpoints and the original circular sweep. Repeated
resizes compose matrices rather than baking distorted circles into source rings.
General matrices are needed because differently oriented resizes can shear.

The first source implementation supplies pure map validation/composition,
point/direction/inverse mapping, exact curve evaluation/tangents and analytical
bounds. Area scales by determinant for an admitted closed ring. Curved lengths
use the exact parametric derivative with bounded deterministic integration and
explicit tolerance/error/work policy; drawing tessellation never measures them.
Shallow arcs use displacement from the actual start and a sinc expression,
avoiding subtraction of distant recovered circle centres.

The source foundation uses bounded exact binary64 rational expressions for
range/cancellation cases, target-grid ties-to-even rounding including subnormals,
and original-coefficient inverse evaluation. It retains the exact determinant
until final area rounding. Ordinary directional products use a compensated fast
path; performance still needs execution evidence. No new third-party dependency
was added: the implementation reuses the existing pinned Boost Multiprecision.

## Required integration

1. Add the optional map to `AssemblyProfile`, equality, bounded preflight,
   geometry admission, volume and expansion. Effective features must remain
   above physical tolerances; determinant positivity alone is insufficient.
2. Introduce a closed mapped-profile codec and explicit parent catalog/instance
   versions. Preserve existing anonymous/identified wires exactly. Advance the
   project reader floor for mapped content in entities, retained commands,
   history and recovery; never let an older schema interpret a mapped profile.
3. Apply profile affine before node placement in native solids. Include it in
   geometry cache identity. Native solids remain derived from the document.
4. Make extent/edge/vertex dimensions consume effective geometry. Carry an exact
   mapped-edge descriptor rather than a manufactured circular Segment. Derive
   exterior text placement from actual curve tangents, winding and cutout role.
5. Carry that same geometry through canvas rendering, fill, picking, snapping,
   previews, rotated selection bounds, reopened profile editing and output.
6. Add exact supported exchange representations or explicit declared fidelity
   handling. Ellipses cannot be exported as circles, including dimension arcs.
7. Pull each whole-component resize through actual nested node frames and
   author every affected own-profile override. Preserve physical Z/base/height.
   Changed identified geometry requires fresh topology and explicit same-kind
   reference remapping/retirement through the reviewed Properties producer.
8. Expose side handles only through the complete captured preview/release path,
   with actual-source, viewport, selection and recovery fences. The complete
   path, not a mathematical helper alone, fulfils the canvas requirement.

The source continuation adds mapped-profile persistence, reader255, effective
model quantities, exact dimension descriptors, native geometry/cache, projected
curve support, canvas/editor presentation and explicit exchange fidelity. Saved
mapped carriers survive phase/clipboard/host operations. Numeric Properties
resizing composes maps for identified curved profiles through fresh topology
and reviewed dimension dispositions. Anonymous curved/mapped XY resizing now
uses an atomic initial-identity/deformation leaf v6, phase v8 and mixed Properties
v37 under reader256. It preserves the complete source rings, scalar and material
roster. Numeric Properties creates fresh identities and enters the same explicit
dimension review. Complete anonymous geometry authoring now uses leaf/request
v7, phase v9 and mixed Properties v38 under reader257. Set reviews the complete
actual own-source roster, permits additions/removals and combines redraw,
deformation, extrusion or material edits. Unchanged XY geometry retains actual
topology; changed/new XY geometry receives fresh identities. Clear requires an
actual own override and restores the inherited type geometry through the same
complete dimension review. Existing final-solid admission remains in force.
Numeric Properties uses this contract for combined edits while retaining v6 for
pure affine deformation. The separate explicit identity-initialization workflow
retains its existing guards. Compilation and runtime remain unverified.

The whole-component geometry helper pulls each profile through its actual
nested/reflected node frame, preserving Z, source geometry and override provenance.
It uses the exact inverse of the actual binary64 rotation basis, including its
norm, rather than assuming transpose is numerically the inverse. Unit factors
do not create maps or overrides. Selection frames now consume analytical mapped
arc bounds in that same root basis.

The canvas source connection now captures complete independent components for
side-handle resizing in measurement, horizontal plan and Site plan views. One
proposal emits sorted changed-node intents with freshly reserved topology;
geometry-free ancestors do not prune descendants. More than 1000 changed nodes
refuses the entire proposal. Properties and canvas share the topology authoring
algorithm. Unit factors retain the source without overrides or dimension review.

Dragging uses a detached, source-derived geometric preview. Affected profile
dimensions may be suppressed in that preview, but those temporary dispositions
have no publication path. Release consumes provisional edit tickets, retains the
same geometry/topology and replacement-root identities, and requires explicit
dimension remap/removal review. The final worker manufactures the complete
reviewed command, native geometry and projected dimensions before publication.
New canvas input/cancellation retires the asynchronous release; source,
selection, camera, view publication and recovery changes refuse late output.
Compilation and runtime remain unverified. This source connection needs actual
interaction qualification; it does not establish complete canvas acceptance.

## Mixed selections

The component geometry foundation now accepts an optional explicit world-axis
angle, retaining actual root axes by default. This allows differently rotated
components to receive the same world operator, anchor and factors; each profile
is still pulled through its actual node pose. Matching analytical bounds in that
explicit unreflected basis permit a common selection frame. Both paths retain
actual binary64 basis inverses and transactional shared work admission. The
finite-angle requirement applies even to unit factors. This adds geometry
support, not mixed command, room, constraint or dimension authority.

The desktop intent producer also accepts a complete independent-component
cohort with one mandatory explicit world-axis angle. It sorts and authenticates
unique actual roots, admits each distinct catalog once through one shared raw
ledger, and derives every actual node through one transactional expansion
ledger. The entire proposal has one UUID namespace and a total limit of 1000
changed node targets. A later failure commits neither an earlier partial roster
nor its work ledger. Unit factors still authenticate every selected root. The
single-object canvas preview uses this same producer with its actual natural
angle and preserves its existing selection, dimension-review and release fences.
The original single-root API retains its default-axis behavior. This cohort
source facility does not yet enable complete mixed-family canvas resizing.

Independent-component cohorts now have a separate canvas side-handle path.
The host supplies complete analytical source bounds, rather than bounds of
display chords. Press captures every selected root and exact keyed profile
presentation, the fixed opposite-edge anchor, the analytical frame and viewport.
Every accepted preview must contain those actual presentations and a separately
computed analytical candidate frame. Missing or partial geometry, missing frames,
changed selection/source/navigation and late serials cannot authorize release.
Ordinary wall/mixed corner transforms keep their separate meaning.

All members receive one operation in canvas axes. A horizontal saved plan uses
its captured projection basis. Site Plan captures every root's actual placement,
then reexpresses the common anchor and axes through each placement, including
roots in different buildings. The target/operator producer sorts complete pairs
without detaching their operations, requires identical directional factors and
authenticates every finite explicit frame even for unit factors. Actual catalogs
share raw admission and a decode cache; every root/node shares the same
transactional expansion ledger, topology namespace and aggregate target limit.

The controller produces one phase-aware cohort command and exact manufactured
preview. Release retains that complete proposal and requires affected profile
dimension review before a final worker prepares the reviewed command and its
dimension presentation. Provisional display suppression never becomes a release
decision. Source, selection, view/presentation and recovery fences remain in
force through nested review and asynchronous completion. This completes the
source connection for independent-component side handles; it does not implement
directional resizing of mixed architectural families or qualify the interaction.

Cutout transfer now examines the exact relative map, rather than requiring both
individual profile maps to be similarities. Compatible anisotropic maps can
therefore transfer circular source primitives without tessellation. Anchored
endpoint conversion keeps the intermediate arithmetic exact and rounds only the
final destination coordinate. Arbitrary different ellipse shapes still require
a broader boundary representation; this correction does not close that gap.

Current ordinary transform dialects contain rigid/similarity operators, and the
combined invariant-child proof uses rigid encoding. Existing child-only skylight
resizing already has directional factors. Mixed resizing must carry the complete
axis operator through original-source preparation, both owner rosters, invariant
proofs, actual hosted consequences, phase replacements and dimensions.

Do not combine independently previewed single-owner commands or treat similarity
dimension carry as general anisotropic authority. Existing mixed Properties and
aggregate clipboard implementations remain in place. Connected walls/rooms,
slabs, roofs, stairs/railings, structural objects, assemblies and presentation
objects each need their genuine geometry/dependency authority; an initial family
implementation does not remove the other families from the release criteria.

## Qualification still required

Cover straight, semicircular, major and shallow arcs; cutouts; repeated rotated
resizes; nested/reflected frames; distant origins; actual dimension/perimeter/
area/volume agreement; bounded integration failure; singular/collapsed refusal;
raw historical wire retention; reader-floor detection; topology/reference
decisions; preview/release agreement; native cache/output; Undo and save/reopen.
These remain unperformed under source-only steering.
