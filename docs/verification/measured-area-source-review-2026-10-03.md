# Measured-area source review verification

This checkpoint implements explicit reassignment of an existing appraisal area
to a current measured-line graph face after topology changes. The full accepted
production scope remains active; this is not Apex compatibility or ANSI signoff.

## Implemented outcome

Details, the selected area's context menu and Commands expose Review measured
sources. The dialog shows retained/proposed outlines, the chosen actual face
area and the proposed property GLA or why it remains withheld. Changed topology
requires existing dimension/reference decisions. That reference dialog displays
the final calculation status before its Apply. Cancel and stale context cannot
commit. Owner names, facts, classifications, factors and metadata survive.

The typed v6 redefinition proves complete same-context, active-phase lineage,
rejects duplicate current-face assignment and validates retained deductions.
Construction/topology and source review are retained as immutable derivation
evidence. Native format 30/extraction 28 protect these operations and measured
stroke phase membership, including Undo history and imported derivation owners.

## Actual evidence

- A pre-implementation executable failed the explicit source-face assertion.
  The final core fixture verifies selected-face replacement, paired-owner review,
  strict v1-v6 codecs, authored construction archives, preserved metadata,
  wrong/missing/coincident/context/phase sources, duplicate assignment, retained
  references, immutable preview, replay and Undo/Redo.
- Actual native dialogs verify face choice, Cancel, source reassignment,
  duplicate prevention, context invalidation, explicit reference removal and
  cancellation, and calculation status before Apply. The sample dwelling
  changes from 8 to 4 m²; garage stays 8 m². The unassigned remaining face is
  not automatically classified or included.
- Native save/reopen preserves authoring source digest and history. Extraction
  advertises 28. Lowering a reviewed file's native markers to 29 rejects it as
  unsupported before digest acceptance, preserving original bytes. Imported
  derivation-only owners and Undo history require 30. Phase-only documents also
  save/reopen with 30; generic vendor collisions keep their previous floor.
- Eight affected executable checks passed: measurement_area_source_replacement,
  measurement_area_source_desktop, appraisal_details_panel,
  measurement_linework_area_desktop, measurement_linework_storage, project_store,
  project_exchange and boundary_editing_desktop (each has a `_tests` executable).
  Both new targets also passed directly through CTest with their declared DLL
  paths and offscreen Qt environment. Qt paths are resolved before those test
  properties are configured; the manual runner does not mask missing setup.
- A read-only independent advisor reviewed data admission, retained proofs,
  compatibility and UI. Required phase-only format and CTest environment fixes
  were applied. Its missing reference-bearing calculation preview finding was
  corrected and exercised through the native dialog.
- Root inspected the final rendered source preview and Details captures.
  The requirement contract audit reports no errors; 130 requirements and ten
  release gates remain represented.

Local logs and captures are under
`artifacts/measured-area-source-review-20261003/`. User-observed resolution,
clean-machine/network-denied qualification, complete Apex parity/compatibility
and normative ANSI validation remain unverified. The manual checklist adds U367
for this user workflow, without treating automated checks as user signoff.
