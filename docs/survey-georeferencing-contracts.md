# Survey and Pro georeferencing contracts

These standalone C++20 contracts provide local calculation and deterministic JSON output for APX-SPEC-001 and APX-SPEC-002. They do not establish Apex parity or complete either user workflow.

`SurveyTraverse` accepts ordered identified quadrant-bearing legs, positive metre distances, provenance, and an absolute closure tolerance. Angles range from 0 to 90 degrees measured from north or south toward east or west. Local geometry starts at (0,0). Output retains every measured vertex and leg. Diagnostics contain signed endpoint errors, linear closure, perimeter, and the dimensionless error/perimeter ratio. Acreage uses 4046.8564224 square metres per international acre. An open traverse has null area and acres. A closed traverse with at least three legs computes planar area including the endpoint-to-origin segment; it does not silently adjust bearings or distribute closure error. Degenerate or intersecting closed boundaries fail. Geodesic area, curve calls, arbitrary distance units, legal-survey certification, and Apex file import are outside this contract.

`GeoreferencingContract` maps local planar metre coordinates through an explicitly supplied invertible 2D affine transform to a declared projected CRS in easting/northing metre order. It does not fit a transform or infer CRS semantics. Control points are observations; even a single point can measure residuals for a supplied transform, but does not establish calibration quality. Residuals are transformed minus observed coordinates, with RMS and maximum magnitude. Ill-conditioned transforms and nonfinite inputs or intermediate results fail. Geographic/angular coordinates and unknown unit enum values are unsupported.

CRS identifiers and definitions are declarations, not database-validated CRS records. Offline resources require safe relative ASCII paths and lowercase SHA256 declarations, with networking disabled. Files are not opened, resolved, hashed, or loaded; the eventual runtime must verify containment, physical files, digests, the CRS definition, and all required PROJ resources before execution. No networking or filesystem operations occur in these contracts. JSON sorts unordered control points/resources while retaining survey leg order; it is output only, with no deserialize/reopen implementation here.

Focused synthetic tests establish contract behavior only. Real Apex survey/module fixtures, export interoperability, UI workflows, persisted reopen fixtures, Pro transform comparison, bundled PROJ execution, actual resource integrity, and network-denied runtime evidence remain open. No production requirement or release gate is certified by these tests.
# Desktop survey calculator

The More menu exposes **Survey traverse** for local bearing/distance entry.
Each nonblank line contains `quadrant, decimal degrees, distance`, for example
`NE, 45, 100 ft`. NE/SE/SW/NW bearings use the same north/south-relative
convention as the core contract. Explicit distance units override workspace
defaults. Source/reference and closure tolerance accompany the calculation.

Calculate reports closure error, perimeter, and area/acreage when available.
Changing any input invalidates the displayed report and disables export until
recalculation. Export writes the versioned JSON contract atomically, retaining
source reference, legs, local vertices, tolerance, and diagnostics. Invalid
legs identify their input line. Calculation/export does not alter the project.

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

This is a calculator and report workflow; persisted survey geometry in the
project, Apex survey exchange, DMS entry, and production survey qualification
remain incomplete.
