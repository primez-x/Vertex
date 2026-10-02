# Appraisal properties usability

The existing selected-area Properties panel exposed custom metadata only as JSON
and placed Edit appraisal facts below seventeen appraisal result rows. The native
regression opens the actual canvas context menu and Properties panel; on the
unchanged application it fails because the structured Name/Value entry point is
missing. Root observed exit 1 before the MainWindow changes.

Rendered checks exposed horizontal clipping caused by unconstrained diagnostic
labels, followed by primary totals below the initial viewport. Appraisal labels
now wrap inside the panel. A concise status and main totals stay in the summary;
full qualification reasons and the selected-area calculation remain under Details.
Geometry, curve and constraint actions share a compact row. The native regression
requires the facts button and qualified primary totals to fit without scrolling.

Verification and actual Qt captures for this delivery are retained under
`artifacts/appraisal-properties-20261002`. Native checks run offscreen and do not
establish user-observed resolution or Apex compatibility certification.

The Release desktop build completed successfully. The focused native Properties
workflow, full `appraisal_desktop_workflow_tests`, deductions-only `desktop_smoke`
scenario, and full `appraisal_report_desktop_tests` all exited 0. The focused
workflow covers the real context menu and modal controls, invalid and bounded
metadata, stale document/selection refusal, cancel and no-op behavior, undo/redo,
and native save/reopen while preserving geometry, facts and totals.

Root inspected the final actual Qt captures of qualified and unqualified summaries
and the metadata validation dialog. Declared classifications use readable labels
while retaining their semantic keys. Primary totals and the facts entry point fit
inside the initial Properties viewport. A read-only independent advisor found no
required data-integrity or modal-guard corrections in the implementation.

The full production goal, original requirements and all mandatory acceptance gates
remain unchanged. This UI work must preserve the current area metadata format,
geometric measurements, calculation rules, custom appearance and recoverable history.
