# Nested appraisal partition authoring

Fill the measured-area review gap that blocks a floor, an enclosed room and a
real exclusion inside that room from being defined in one transaction. The
existing ANSI appraisal report already supports these relationships. This
authoring change must expose those semantics without changing legacy
Auto-Subtract or implying new standards certification.

Collect explicit parent/child assignments and validate their complete overlay
against the captured project. Each parent deducts immediate-child gross
footprints. Each eligible measured child contributes its own net area. For
gross areas 100/36/4 m², qualified floor64 plus room32 gives GLA96 m², with the
4 m² void excluded once. Missing observations leave GLA unavailable; labels
and geometric containment do not establish eligibility.

Use a shared ANSI partition preparation helper for Detect closed areas and the
existing deduction editor. Resolve the actual appraisal policy and supported
version rather than using dimension styling as authority. Keep the legacy
TYPE restriction and historical subtraction reconstruction unchanged.
Preserve identifiers, source lineage, styles, facts and retained deductions.
Refuse cyclic, malformed, cross-context, unsupported, stale or phase-hidden
dependencies. Allow removal of an invalid link as a repair. Continue unioning
overlapping voids and defer eligibility/overlapping contributions to the report.

Core worker owns the pure helper, measured-area preparation and core regression.
Desktop worker owns the native review regression. Root owns UI integration,
CMake, documentation, all builds/native execution, Git and delivery. All native
writers freeze before execution. Independent advice challenges policy,
complete-overlay validation and retained history.

Root observed both pre-change failures in
`artifacts/nested-appraisal-partitions-20261004/red-check.log`: same-TYPE
preparation failed and the native review kept Apply disabled. Verify native
review net/deduction rows, one atomic commit, exact source preservation,
Undo/Redo, save/reopen, explicit-fact qualification and matching Details/PDF.
Check cycle/context/source refusals, removal repairs, void unions and frozen
legacy replay. No new geometry/report schema is introduced by authoring
already-supported links. Full Apex compatibility, ANSI normative validation,
independent copy of a parent with nested descendants and unified production
acceptance remain separate unresolved requirements.
