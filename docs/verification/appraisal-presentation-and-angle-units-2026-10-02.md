# Appraisal presentation and curve angle units

The selected classification and default area styling previously read saved manual
metadata, even when the appraisal report derived a different category from current
declarations. The native regression reproduced a declared Garage whose saved manual
category was Above-grade finished: the report showed Garage but the inspector
still exposed the stale editable category.

The curve editor suggested degree input but passed bare numbers to the generic
radian parser. The actual editor accepted bare `1` as a valid sweep. Root observed
that regression fail before changing the UI validation. With explicit-unit
validation, the focused editor check passes bare-number refusal and `90 deg`,
`pi/2`, and `-pi/2` construction, exact receipts, fixed endpoints, dependent length
dimensions, undo/redo and native reopening. Generic core bare radians remain valid.
Root inspected the retained actual invalid-angle dialog: the explanation is
visible and Apply is disabled.

Independent source review found a malformed-hierarchy fallback: a boundary with
an explicit property ID but no floor/building missed the declared-category seed
and recovered its old Garage style. A new native regression reproduced that
specific failure after the report correctly retained an invalid row with no
numeric trace. The ownership projection now retains the property hint so that
the invalid row stays unqualified rather than borrowing old styling.

An intermediate appearance check failed because its second MainWindow opened a
project still owned by the first. Runtime diagnostics confirmed the existing
read-only ownership safeguard. The fixture now releases each saved project before
opening the next editing window; production ownership checks are unchanged.

Root observed the focused derived-presentation check and full
`appraisal_desktop_workflow_tests` pass. Coverage includes retained stale metadata,
Garage/Dwelling defaults, the actual appearance dialog and Reset, manual edit
refusal, exact custom overrides, individually qualified categories during aggregate
refusal, missing-fact participants, valid exclusions, editable undeclared legacy
categories, inherited appearance with exact label-record preservation, malformed
hierarchy, undo/redo and native reopening. The fixture respects the established
manual-category migration and documented `plan_label_offset_m` field.

The full boundary editor, boundary event workflow and appraisal report desktop
checks also pass. Root inspected the actual selected Garage canvas and inspector,
including its consistent category, separate Garage bucket and zero living total.
No document format, geometry, core angle-parser contract or calculation rule was
changed. Display projection does not persist a derived category.

Targeted logs and actual Qt captures are retained under
`artifacts/appraisal-presentation-20261002`. These checks use real native controls
offscreen; they do not claim user-observed resolution. The requirement contract
passes; the release audit retains 130 requirements and ten mandatory gates and
reports `production_accepted=false`. Full production and Apex compatibility
certification remain open. Package, installation and exact delivery fingerprints
are recorded separately in the artifact directory's delivery evidence.
