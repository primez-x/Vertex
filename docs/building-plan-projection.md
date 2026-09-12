# Building object plan projection

`sketch::project_building_plan(const BuildingObject&)` derives the visible
top-down drawing edges for one of the eight canonical `BuildingObject` forms:
rectangular column, circular column, straight beam, stair flight, straight
railing, sloped roof panel (including its zero-rise flat-roof case), gable roof,
and hip roof.  Coordinates
remain in the document's world XY frame,
so translation and horizontal rotation are retained.  The result is a
`Boundary` whose zero-sweep segments are lines and whose nonzero-sweep
segments are exact circular arcs.

The function first calls the existing `make_building_shape` builder.  It then
runs OCCT's exact BRep hidden-line pipeline:

1. `HLRBRep_Algo::Add`, `Projector`, `Update`, and `Hide` evaluate the solid
   with an identity-oriented `HLRAlgo_Projector` (OCCT's top projection).
2. `HLRBRep_HLRToShape` extracts the visible sharp, outline, smooth, and sewn
   edge groups.  Hidden edges and isoparameters are intentionally excluded.
3. The resulting 2D edges are read from OCCT's canonical `BRepLib::Plane`
   p-curves.  Lines and circles are converted directly; no spline, conic, or
   pixel approximation is accepted.  A full circle is emitted as four exact
   quarter arcs because `Boundary` cannot represent a zero-chord full-turn
   segment.

The returned boundary is presentation geometry for PlanCanvas and PDF
rendering.  It is never an authoring boundary or a calculation input.  The
first view style is a complete top-down projection without a cut plane,
floor/elevation filtering, or hidden-line display.  All eight forms are
projected as their complete solids, including roof thickness and stair
landings.  Invalid semantic dimensions, a null/empty HLR result, missing
projected p-curves, and unsupported projected curve types throw
`std::invalid_argument`; callers should keep the geometry error visible and
avoid presenting a partial drawing as authoritative.

## Link requirements

The projection implementation requires OCCT `TKHLR` for
`HLRBRep_Algo`/`HLRBRep_HLRToShape`, `TKBRep` for edge and p-curve access,
`TKTopAlgo` for `BRepLib::Plane`, and `TKG2d` for the exact 2D line/circle
types.  The target also links the existing `sketch_building_entities` (and its
normal OCCT modeling dependencies) because that target owns
`make_building_shape`.

This package does not add a new geometry primitive or a raster fallback.  If a
future building form yields a curve outside the line/circle contract, the
projection must fail explicitly until that curve can be represented exactly
by the plan model.

Regression evidence includes exact segment-multiset comparisons against
independently constructed rotated columns, horizontal and sloped beams,
stairs with and without landings, straight railings, sloped panels, gable roofs,
and hip roofs. These include
tread, landing, roof-thickness and ridge edges. The current solid compound
projection retains coincident edges from distinct solids, including the two
gable panel ridge edges; the oracle checks their explicit multiplicity rather
than silently dropping them. Canonical joined sheet linework remains part of
the coordinated-output qualification.

The circle test requires exactly four connected quarter arcs with the expected
center/radius, one complete winding and no extra edges. Bounds checks still
cover all eight forms; they are not used alone to establish complete edge
correctness. The desktop workflow also checks that all eight forms reach the
shared plan/PDF scene after reopening.
