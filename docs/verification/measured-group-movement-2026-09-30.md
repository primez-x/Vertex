# Measured group movement

Multiple receipt-backed measurement areas now move with architectural objects,
ordinary boundaries and presentation objects in one reversible command.
`TranslateBoundaries` reconstructs each measured owner before applying ordinary
changes, then validates the complete state. Exact input receipts, stable IDs,
metadata and dependent dimension anchors survive. A conflicting partial move
refuses without publishing any selected item.

New batch replay uses a construction transform frame. It avoids changing exact
imperial analytical joins through separately rounded additions to old starts.
Historical single-boundary translation replay remains unchanged. Storage v9,
exchange v6, strict command decoding, digests and recovery budgets retain the
batch proof, including undone and branched history.

Horizontal named plans convert model movements through their view basis while
presentation objects keep the existing overlay coordinate convention. The
view now retains measured outlines, curves, dimensions and symbols. Generated
model-label positions share one projection in canvas and sheet output;
explicit annotations retain overlay positions. Projected measured vertex
handles are suppressed until their inverse mapping is implemented.

## Observed verification

- Release desktop and affected core/storage targets built successfully.
- `document_commands`, `project_storage`, `project_exchange`, `document_digest`,
  `boundary_receipt_integrity` and `workspace_recovery_budget`: 6/6 passed,
  3.44 seconds. Coverage includes duplicate/overlapping changes, stale commands,
  invalid offsets, partial constraint refusal, proof tampering/removal, exact
  quantities, undo/redo, branch history, persistence and legacy digest vectors.
- Native `measurement_group_move`: passed after the projection correction.
  Actual canvas events move two joined classified 12-by-8-foot measured areas,
  a sofa, a wall and an ordinary porch once. Escape, exact undo/redo, dimensions,
  large fractional offsets, partial refusal and save/reopen are checked. Both
  upward and downward shifted/rotated horizontal named plans are exercised.
- Companion `boundary_workflow`, `symbol_transform_desktop` and
  `architectural_document_adapter` passed; the combined four-check run passed
  in 24.37 seconds before the shared output-label helper follow-up.
- Final `measurement_group_move`, `symbol_transform_desktop` and
  `coordinated_view_output`: 3/3 passed in 37.26 seconds. Output checks render
  actual PDFs, require area/dimension text, and compare origin-invariant
  rotated horizontal plans in both viewing directions.
- Root inspected the group-movement and live-rotation captures. Independent
  source review found the sheet label-position mismatch; it is corrected and
  covered by the final output check. `git diff --check` passed.

These are focused developer-machine checks. Physical input, oblique-plane
editing, Apex compatibility and complete production qualification remain open.
