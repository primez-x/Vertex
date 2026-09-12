# Survey and Pro georeferencing contracts

These standalone C++20 contracts provide local calculation and deterministic JSON output for APX-SPEC-001 and APX-SPEC-002. They do not establish Apex parity or complete either user workflow.

`SurveyTraverse` accepts ordered identified quadrant-bearing legs, positive metre distances, provenance, and an absolute closure tolerance. Angles range from 0 to 90 degrees measured from north or south toward east or west. Local geometry starts at (0,0). Output retains every measured vertex and leg. Diagnostics contain signed endpoint errors, linear closure, perimeter, and the dimensionless error/perimeter ratio. Acreage uses 4046.8564224 square metres per international acre. An open traverse has null area and acres. A closed traverse with at least three legs computes planar area including the endpoint-to-origin segment; it does not silently adjust bearings or distribute closure error. Degenerate or intersecting closed boundaries fail. Geodesic area, curve calls, arbitrary distance units, legal-survey certification, and Apex file import are outside this contract.

`GeoreferencingContract` maps local planar metre coordinates through an explicitly supplied invertible 2D affine transform to a declared projected CRS in easting/northing metre order. It does not fit a transform or infer CRS semantics. Control points are observations; even a single point can measure residuals for a supplied transform, but does not establish calibration quality. Residuals are transformed minus observed coordinates, with RMS and maximum magnitude. Ill-conditioned transforms and nonfinite inputs or intermediate results fail. Geographic/angular coordinates and unknown unit enum values are unsupported.

CRS identifiers and definitions are declarations, not database-validated CRS records. Offline resources require safe relative ASCII paths and lowercase SHA256 declarations, with networking disabled. Files are not opened, resolved, hashed, or loaded by the contract itself. `verify_georeferencing_runtime` is the explicit Windows runtime boundary: it requires an existing contained resource root, resolves every declared file without following it outside that root, enforces a per-file size limit, recomputes every SHA256 digest, binds PROJ to the declared `proj.db` and search directories, disables networking in the created context, and validates both CRS declarations plus an identity operation. No networking or filesystem operations occur in the contracts. JSON sorts unordered control points/resources while retaining survey leg order. `GeoreferencingContract::from_json` strictly validates the version-1 envelope, recomputes residuals, and rejects unknown fields or derived-value tampering. A version-1 `georeferencing` Document entity carries that model through save/reopen; it remains a declaration until the runtime preflight succeeds.

Focused synthetic tests establish contract behavior and typed-entity save/reopen. The Windows desktop exposes a persisted georeferencing editor with CRS, affine coefficients, control-point parsing, residual display, a sample transform check, and normal Document history; desktop smoke covers save/reopen. A runtime test now exercises the pinned local PROJ database, real resource hashing, CRS parsing, identity operation, and network-disabled context. Real Apex survey/module fixtures, export interoperability, Pro transform comparison, and production qualification remain open. No production requirement or release gate is certified by these tests.
# Desktop survey calculator

The More menu exposes **Survey traverse** for local bearing/distance entry.
Each nonblank line contains `quadrant, angle, distance`, for example
`NE, 45, 100 ft` or `NE, 45:30:15.5, 100 ft`. Angles accept decimal degrees
or `degrees:minutes:seconds`; DMS degrees/minutes must be integers, and seconds
may be fractional. Minutes and seconds must be below 60 and the full bearing
cannot exceed 90 degrees. Incomplete DMS calls are rejected, not guessed.
NE/SE/SW/NW bearings use the same north/south-relative
convention as the core contract. Explicit distance units override workspace
defaults. Source/reference and closure tolerance accompany the calculation.

Calculate reports closure error, perimeter, and area/acreage when available.
The adjacent canvas previews measured legs, including open traverses. A cyan
segment shows a proposed closure; choosing endpoint adjustment previews the
replacement final leg while retaining the measured geometry. Previewing,
panning, and zooming do not change project history. Changing input clears the
preview together with the report, so stale geometry cannot be mistaken for
the current calls.
Changing any input invalidates the displayed report and disables export until
recalculation. Export writes the versioned JSON contract atomically, retaining
source reference, legs, local vertices, tolerance, and diagnostics. Invalid
legs identify their input line. Calculation/export does not alter the project.

**Add boundary** inserts a closed, area-bearing traverse into the active
drawing layer as one undoable measurement boundary with classification
`survey`. Measured legs are preserved. A nonzero endpoint residual requires an
explicit extra segment back to the origin, described in the dialog; geometry
validation may reject a residual too small to form a valid segment. Alternatively,
the user may explicitly check **Close the final leg at the origin** for an
area-bearing traverse within tolerance. This adjusts that leg's endpoint
instead of creating an extra segment, while retaining the measured calls.
The choice clears when inputs change. No bearing or distance is silently
adjusted. Open traverses cannot be added as areas.
The dialog's document, revision, selection, layer, and units must still match
the captured drawing context before insertion.

The boundary's `extensions.survey_source` contains version 1, the original
report, `added_closing_segment`, `adjusted_final_endpoint`, and
`endpoint_adjustment_m` (east/north offsets, or null when no adjustment was
selected). This is historical source metadata:
subsequent boundary edits do not recalculate it or claim to update the original
survey. Geometry and metadata save/reopen and undo/redo together; the boundary
uses the common canvas and vector-output path.

Selecting a boundary carrying version-1 `survey_source` and opening **Survey
traverse** restores its original input directly from the project. The dialog
identifies this as the original source and recomputes it using the same input
validation as report reopening. It does not infer revised calls from subsequent
drawing edits or overwrite the selected boundary. Unsupported source versions
show an error; viewing the source leaves project history unchanged.

New survey boundaries persist `properties.calculation_scope = "site"`.
Earlier survey-classified boundaries without that field are interpreted as
site scope; other older boundaries default to building scope. Unknown scope
values or types block desktop calculation. The default profile recognizes
the survey classification. Site areas retain their own area results and
classification subtotal, but never contribute to building or living totals.
They may enclose building areas without triggering same-floor overlap errors;
overlapping site areas still trigger an error. The scope is independent of
classification changes once persisted.

Desktop exports add an optional `input_provenance` object, version 1, to the
version-1 core report. It records `legs_text` and `source_text` verbatim,
`default_unit` (`m` or `ft`) for suffixless input,
`closure_tolerance_expression`, and ordered `distances`. Each distance records
its `leg_id`, one-based `line_number`, `original_expression`, and normalized
`exact_metres` numerator/denominator. These fields preserve the entered source
without replacing the calculated metre values. Consumers must validate and
recompute input before using it as geometry.

**Open report** accepts desktop reports with version-1 input provenance, up to
4 MiB. It restores the source, original leg text, tolerance, and default units
and recalculates through the survey engine. Stored vertices, totals, and exact
receipts are ignored when recalculating. Unsupported versions and malformed
input metadata leave current entries intact; invalid entered measurements are
shown for correction with report export disabled. The default-unit selector
is explicit and independent of project display units. This is native report
reopening, not Apex interchange or a signed survey attestation.

Updating existing traverses from survey calls, Apex survey exchange,
and production survey qualification remain incomplete.
