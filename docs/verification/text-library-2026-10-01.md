# Reusable text library checkpoint

The component panel's + Text control opens a searchable, category-filtered
library. Users can create, save, edit and delete named multiline entries with
height, font, color, bold and italic styling. Built-ins remain immutable and
offer Save copy. Insert arms a canvas click; dragging pans and Escape cancels.
Placed labels are independent copies on the active layer.

The bounded local JSON format is documented in `docs/text-library-format.md`.
Atomic saves use cooperating writer locks and exact-byte changed-file checks.
Malformed, unsupported, read-only and stale files are preserved. A missing or
corrupt library does not prevent existing project text from loading or editing.

New Insert labels use annotation v5 model-plan anchors. World XY positions
project into horizontal plans, and rotated/reflected pointer movement converts
back to world XY. Text rotation stays relative to the view for readability.
Legacy view-overlay labels keep their coordinate behavior. Version 5 retains
v4 area offsets and older symbol representations without downgrading state.

## Verification

- Release build succeeds for Vertex and the affected check executables.
- `text_library_tests` passes codec limits, validation, reserved built-in IDs,
  actual dialog controls, atomic storage, writer locking, read-only refusal,
  corruption and external-change preservation.
- `text_library_desktop_tests` passes the actual dialog-to-canvas Insert path,
  independent instances, history, pan/cancel, stale/read-only refusal, raw
  annotation metadata and pinned sibling preservation, library edits/deletion,
  missing/corrupt-library reopen, named-plan projection and dragging, and v5
  coexistence with area-label presentation changes.
- Annotation catalog and entity codec checks pass v5 round-trip and native
  save/reopen, malformed field refusal and earlier/future version boundaries.
- Full boundary-canvas, boundary-workflow, appraisal-desktop-workflow,
  named-plan-vertex and symbol-transform-desktop checks pass.
- Root inspected the actual library dialog, selected text on the canvas and
  rendered PDF. Font changes affect both canvas and output. The offscreen font
  fixture explicitly registers a Windows font because that Qt platform does
  not enumerate system fonts as the native platform does.
- Independent source review identified the named-plan coordinate contract and
  a selection conflict. Both are resolved by the explicit v5 model-plan flag
  and a separate selectable canvas-label flag.

Artifacts are in `artifacts/text-library-20261001`, including final regression
results and the passing desktop capture set in `pass-8`. Historical failing
checks remain recorded. Invalid fixtures and a held project writer lock were
corrected; the named-plan placement and selection failures required source
fixes. Named-plan drag coverage disables the restored grid preference directly
before dragging so it checks exact unsnapped coordinate conversion.

Release executable SHA-256:
`d719a1924bd9404698f09f72d7172140f5599dc8ec00c26846fc743b70a6d25f`.

Manual user task U130 now lists the concrete save/edit/reuse operations.
Adaptive Imperial/Metric grid behavior remains covered by U081/U082 and the
boundary-canvas checks, including half-foot and centimetre intervals.

The default multipurpose sheet's sparse layout is not qualified by this text
retention fixture. Independent wall-dimension presentation, appraisal report
ergonomics, physical printing, user-observed resolution and Apex/device
compatibility remain open. This checkpoint does not certify the full production
release or complete the production goal.
