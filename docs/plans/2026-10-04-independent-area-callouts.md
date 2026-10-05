# Independent area callouts and text alignment

PINC-010 requires separate live area-name and calculated-value callouts, each
with its own placement, size, color, alignment, rotation and visibility. The
calculated value remains a projection of current authoritative geometry and
appraisal/room checks. It is never persisted as copied annotation text.

## Ownership and implementation

- Core schema worker: appended centered-default text alignment; conditional
  annotation state 8; independent `area_name`/`area_calculation` overrides;
  text-library version 2 and strict codecs with backwards defaults.
- Canvas worker: aligned paint/hit/export bounds, role-aware same-owner label
  caches and previews, focused rendered regression.
- Desktop worker: shared live scene/vertex-preview projection; explicit
  separation controls and role chooser; independent properties and placement;
  text alignment controls; owner-based selection and sheet scope.
- Root: native reader 45, extraction 43 and portable-package support;
  measured-owner role-offset completion; CMake, integrated review, native
  verification, source-kit generation and scoped Git delivery.

Older projects keep their combined callout until separation is explicitly
adopted. Both split callouts retain the real area owner ID; a role is presentation
metadata, not a new geometric or analytical entity. Hiding a role must not hide
the area or change totals. Centered legacy styles keep their prior wire meaning.
Retained history containing version-8 annotations requires the newer reader
even after Undo or deleting the presentation owner.

## Verification

Freeze all writers before root-owned native jobs. Verify independent role
styles/placement, invalid fields and versions, live values after edits,
stale-value suppression, aligned picking/paint/output, same-owner preview
retention, Undo/Redo, native reopen, retained reader floors and portable output.
Inspect rendered captures. Existing name/quantity, wall-dimension, text-library
and output workflows must remain working. Record actual results and limitations;
this feature does not establish the entire Pinc or production acceptance gate.
