# Boundary curvature and appraisal diagnostic checkpoint

The requested adaptive grid is present in the delivered October 1 build. Its
visible spacing and cursor snapping share the same zoom- and unit-aware ladder:
imperial feet/half-feet/inches/fractions and metric metres/centimetres/millimetres.
Panning keeps the world origin; exact object endpoints take precedence. Unit or
zoom changes do not quantize retained geometry or add a grid to exported output.

This checkpoint additionally closes two identified 2D editing gaps and improves
the appraisal audit's repair workflow. It does not certify Apex native-file,
caller/device compatibility, the complete architectural release, physical
printing, or production performance.

## Changes

- Tools and command search expose associated length, angle and area dimensions.
  The creator shows only the fields relevant to the chosen kind. A length label
  defaults to automatic exterior placement. Standalone dimensions can be removed
  through Delete without removing their source boundary.
- Boundary geometry editing supports fixed-chord curve angle, signed height and
  arc length. Signed angle/height choose the side; arc length uses clockwise.
  Preview retains the original outline, shows both fixed endpoints, and reports
  changed dimensions, area and perimeter before Apply.
- The strict `reconstruct_arc` intent retains exact construction expressions and
  stable children. Receipt-backed source geometry is archived unchanged, and
  derivation replay uses the existing analytical constructor. Manual dimension
  placements stay unchanged; automatic placements follow the new exterior arc.
  Normal document validation refuses invalid topology and conflicting locks.
- New semantic history requires native format 15 and JSON/assets exchange 13,
  including undone history and entity-only imported derivations. Existing
  formats are retained when the new intent is absent.
- Missing policy retains valid individual diagnostic calculations while qualified
  totals remain withheld. Malformed in-scope sources remain selectable report
  rows with specific issues and unavailable numbers. Show on canvas permits
  repair; other-property, site and semantically hidden sources stay excluded.

## Evidence

Root observed failing regression checkpoints before the bounded core fixes.
Affected checks:

- `boundary_canvas_tests --adaptive-grid-only` and
  `boundary_workflow_tests --adaptive-grid-only`: visible grid and snap increments,
  half-foot/inch/centimetre cases, signed world coordinates, panning, endpoint
  priority, exact retained geometry, and grid-free output.
- `geometry_operations_tests`: fixed endpoints/IDs, signed and major arcs,
  strict construction inputs, and crossing-topology refusal.
- `boundary_arc_edit_tests`: actual document preview/commit, exact archived
  receipts, dependent dimensions, atomic refusal, undo/redo, native save/load,
  retained v15 semantics, exchange v13, and physical arc-length lock refusal.
- `appraisal_document_tests`: undeclared/partial policy, malformed sources,
  deductions, category qualification, and scope exclusions.
- `appraisal_report_desktop_tests`: actual report controls, selectable invalid
  source, repair, withheld totals, diagnostic PDF, pagination and safe export.
- `boundary_editing_desktop_tests`: actual dimension creator and Delete action;
  existing length editor; inward/outward angle, height and arc-length controls;
  preview without live mutation; associated dimensions; undo/redo and native
  reopening.
- `project_store_tests` and `project_exchange_tests`: affected native storage and
  extraction regression checks.

Core and desktop suites passed. Desktop controls were exercised offscreen with
the bundled font. Captures and logs are retained under
`artifacts/boundary-curvature-20261001`; this is technical evidence, not a claim
of user-observed resolution. Root inspected the actual dimension creator,
curve previews and invalid-area report. Independent read-only engineering review
found no required correction in the scoped intent/replay/version/diagnostic code.

The requirement contract and source-kit checks validate their contracts only;
they are not production acceptance. User checklist tasks U306–U308 describe
dimension restoration, curve editing and diagnostic repair.
