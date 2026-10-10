# Survey and Pro georeferencing contracts

These standalone C++20 contracts provide local calculation and deterministic JSON output for APX-SPEC-001 and APX-SPEC-002. They do not establish Apex parity or complete either user workflow.

`SurveyTraverse` accepts ordered identified quadrant-bearing legs, positive metre distances, provenance, and an absolute closure tolerance. Angles range from 0 to 90 degrees measured from north or south toward east or west. Local geometry starts at (0,0). A curved leg's distance is its chord; its signed sweep describes an analytical circular arc. Output retains every measured vertex and leg. Diagnostics contain signed endpoint errors, linear closure, measured perimeter, and the dimensionless error/perimeter ratio. Acreage uses 4046.8564224 square metres per international acre. An open traverse has null area and acres. Closed boundaries use analytical area and intersection checks, including valid two-arc regions. Perimeter excludes any separately proposed closing line. Straight-only reports preserve their v1 schema and numerical path. No bearing or distance is silently adjusted. Geodesic area, legal-survey certification and Apex file import remain outside this contract.

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

Curve calls specify chord bearing and chord length, then one construction:

- `CURVE, NE, 90, 20 m, -180` uses signed decimal central angle in degrees.
- `ARC_HEIGHT, NE, 90, 20 m, -10 m` uses signed arc height.
- `ARC_LENGTH, NE, 90, 20 m, 31.415926536 m, CW` uses measured arc length and direction.

Positive sweep/height is counter-clockwise; negative is clockwise. Length calls
require `CW` or `CCW` and an arc length greater than the chord. Every arc is
validated even in an open traverse. Zero/full-turn, unsupported constructions
and intersecting closed outlines are rejected. Mixed curve calls produce v2
reports with v2 entered-input provenance and versioned construction receipts.
The shared `build_survey_report` / `rebuild_survey_report` API reconstructs
normalized calls and verifies their receipts; derived display values never
establish measurement truth.

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
The endpoint-adjustment choice is disabled for a curved final call and refused
by the core. Retaining measured calls adds a separate straight closing segment;
a nonzero residual at or below geometry tolerance produces an explicit insertion
error instead of silently moving an arc endpoint. The shared
`make_survey_boundary` helper validates the exact proposed boundary.
The dialog's document, revision, selection, layer, and units must still match
the captured drawing context before insertion.

The boundary's `extensions.survey_source` contains version 1 for straight-only
calls or version 2 for curved calls, the retained
report, `added_closing_segment`, `adjusted_final_endpoint`, and
`endpoint_adjustment_m` (east/north offsets, or null when no adjustment was
selected). This is historical source metadata:
subsequent boundary edits do not recalculate it or claim to update the original
survey. Geometry and metadata save/reopen and undo/redo together; the boundary
uses the common canvas and vector-output path.

Selecting a boundary carrying version-1 or version-2 `survey_source` and opening **Survey
traverse** restores its original input directly from the project. The dialog
identifies this as the original source and recomputes it using the same input
validation as report reopening. It does not infer revised calls from subsequent
drawing edits. Unsupported source versions
show an error; viewing the source leaves project history unchanged.

After correcting the calls and choosing **Calculate**, **Update boundary**
replaces the selected survey outline in one undoable operation. Its identity,
current starting point, layer, classification and factor remain intact. The
entered bearings use north as their reference; this replaces later manual
rotation or vertex edits rather than inferring new calls from them. The closure
choice is restored when reopening and saved with the correction. The first
correction preserves `original_report` and `original_closure`; subsequent
reports remain recoverable through project history. `placement` records the
current anchor and called-north orientation.
Source version 2 remains sticky after subsequent straight-call corrections. The
reader floor scans all retained history and preserves future source/report/input
versions with read-only protection. Archival source metadata is never compared
with live edited geometry during admission.

Surviving call rows keep their vertex and edge identities when their original
ownership can be established. Changes to leg count or a reordered boundary
cycle allocate new child identities. Dependent dimensions and constraints must
still resolve; conflicts block the update without changing the document.
Ownership is established from the current uninterrupted report/closure
authoring ancestry.
Returning to an earlier set of calls after a leg-count change retains the newer
identities on subsequent same-count corrections; an earlier occurrence of those
calls does not replace the current ownership. Undo/Redo inherits the ownership
of its restored source revision; navigation does not erase an earlier reorder.
Reordering within the current ancestry still prevents row reuse.
Receipt-backed and derived boundaries require a receipt-preserving workflow
and are rejected here. Changing the project while the dialog is open also
blocks the update until the dialog is reopened.

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

**Open report** accepts desktop reports with version-1 or version-2 input
provenance, up to 4 MiB. It restores the source, original leg text, tolerance, and default units
and reconstructs geometry through the survey engine. Stored vertices and totals
never establish geometry; known receipt fields must match the reconstructed
calls. Validated opaque input and receipt extensions survive reopening,
unchanged recalculation, export and boundary insertion/correction. Editing an
input invalidates the report and requires fresh reconstruction. Unchanged
recalculation also retains an explicitly selected, still-valid endpoint
adjustment. Unsupported versions, malformed receipts and invalid entered measurements leave current
entries intact and display an error. The default-unit selector
is explicit and independent of project display units. This is native report
reopening, not Apex interchange or a signed survey attestation.

The current row-ownership and desktop extension-preservation changes have only
source review. Compilation, interaction and save/reopen qualification for these
changes remain outstanding; historical runtime evidence does not cover them.

Apex survey exchange and production survey qualification remain incomplete.
