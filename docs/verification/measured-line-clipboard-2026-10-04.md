# Loose measured-line clipboard and removal

Supported measured strokes now participate in native Copy, Cut, Paste and
Delete. Stroke, segment and vertex identities are remapped independently;
receipt bindings and supported move/resize intent follow those identities.
Opaque entity/model metadata and original typed quantities remain intact.
The existing independent graph collector carries only owned appearance.
Mixed area/stroke copies and pure-stroke copies use the same ownership guards.

The required flag is retained on copies because it declares reader support.
Explicit removal is permitted only after decoding a known supported stroke;
other required project owners remain protected. Removing a shared source
retains dependent area geometry, facts and lineage as stale observations,
withholding qualified appraisal totals. Existing command augmentation handles
phase and sheet references and exact Undo restoration.

In an empty destination, Paste retains measured coordinates and the existing
model dialect. It still performs the same graph preflight as a populated
destination. Copies beside existing geometry use the established right-side
placement. The canvas context menu reuses the existing Copy/Cut/Delete actions
and offers Paste even when no object is selected.

The Release RED build succeeded; the real native measured-line Copy failed
with `Every selected root must support clipboard operations.` Evidence is
retained under `artifacts/measured-line-clipboard-20261004/`. The final Release
build and seven affected registered checks pass: measurement_linework_clipboard_desktop,
measurement_linework_desktop, measurement_linework_source,
measurement_linework_storage, measurement_linework_edit,
measurement_linework_transform and nested_appraisal_copy_desktop.
Native coverage includes actual right-click Cut and Paste on selected and
empty canvas, original typed receipts, retained edited geometry and appearance,
independent identities, atomic history, save/reopen and unsafe payload refusal.
The two shared-source consumers initially contribute 16 m² of GLA. Delete
retains both areas and their facts exactly, makes both sources stale and
withholds totals; Undo after reopen restores their original qualification.
Root inspected native captures of selected pasted linework and the visible
missing-source/withheld-total state. Installed observation is recorded
separately in local delivery evidence.

The independent advisor identified missing stroke appearance admission and
an empty-destination preflight bypass. Both guards were corrected before the
verification run. Core constraints and user-bound dimensions still do not
support loose strokes; this change does not claim to add that separate
capability. The full Apex replacement, compatibility and ANSI normative
validation gates remain open. User acceptance tasks U409 and U410 cover these
clipboard and shared-source removal workflows.
