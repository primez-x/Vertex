# Exact directional component resizing

This implements the existing requirement for horizontal/vertical canvas handles
and editable component dimensions. It does not change the production scope or
make qualification optional. The current source-only instruction remains in
effect; no runtime evidence is claimed.

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
dimension review; combined anonymous affine deformation with profile redraw or
extrusion changes still needs a complete authoring contract.

The whole-component geometry helper pulls each profile through its actual
nested/reflected node frame, preserving Z, source geometry and override provenance.
It uses the exact inverse of the actual binary64 rotation basis, including its
norm, rather than assuming transpose is numerically the inverse. Unit factors
do not create maps or overrides. Selection frames now consume analytical mapped
arc bounds in that same root basis. These helpers do not yet supply the complete
captured side-handle preview/release producer. Compilation and runtime remain
unverified; canvas side handles and whole-component authoring are not claimed
complete by these source additions.

## Mixed selections

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
