# Appraisal area display and sheet typography

This is a scoped development checkpoint. It does not close Apex compatibility,
measurement-standard compliance, or the unified production acceptance gate.

## Implemented behavior

- Appraisal exposes **Tools > Area display** with precision from 0 to 6 decimal
  places. Missing configuration retains two places. Workspace units determine
  ft² or m². The compact editor does not expose appraisal eligibility rules.
- A real saved change updates only precision and the local display revision,
  preserving opaque legacy profile fields. Cancel and unchanged Save do not
  create history. Undo/Redo and ordinary save/reopen retain the setting.
- The inspector, 2D Appraisal schedules and sheet summary use shared display
  rounding. Quantities remain unrounded SI, and totals round the aggregate once.
  Hidden legacy Measurement metadata cannot alter or block Appraisal rules.
- Invalid precision visibly withholds numerical totals. A stale modal context,
  read-only document or invalid/exhausted display revision refuses an edit.
- Sheet typography uses physical point sizes without applying printer DPI
  twice. Inter's `calt` and `case` punctuation alternates are disabled for sheet
  text, avoiding private-use characters when copying/extracting PDF text.

## Verification

The core and native baseline checks failed before the precision implementation:
the aggregate used fixed precision and Appraisal refused the display dialog.
Focused checks now cover 0/1/6 places, unchanged entities and SI values, fixed
eligibility, metadata preservation, actual dialogs/schedules/PDF, history,
reopen, stale saves, malformed settings and read-only refusal.

- Core Appraisal and schedule checks: 2/2 passed in 0.13 seconds.
- Integrated pre-typography run: 8/8 passed in 266.69 seconds, including the
  full desktop workflow, calculations, Appraisal, schedules and schema contract.
- After the final font correction, the native `--area-display-only` check
  passed in 3.67 seconds. `drawing_set_output` and
  `appraisal_desktop_workflow` passed 2/2 in 18.56 seconds.
- The PDF regression requires standard `A-901` and `(GLA)` punctuation, no
  private-use characters, correct precision, and a numerical text bounding
  height of at most 12 PDF points.
- Independent pypdf inspection confirmed standard punctuation and 8-point
  physical body text in both captured PDFs. The previous sheet body measured
  45 physical points after applying the page transformation. Raw PDF font sizes
  alone are not physical sizes; the independent check includes that transform.
- Root rendered and inspected the corrected PDF: normal-sized body and title
  text replace the oversized, clipped text. The fixture's large schedule frame
  remains a test layout, not a claim of complete report-design qualification.
- Current rotation checks also passed 2/2 in 14.61 seconds, covering retained
  frame/pin orientation, 45-degree snaps, Shift fine rotation, live degrees,
  history and reopen. No additional rotation patch was required.
- Final core/native output, schedule, schema and source-kit checks passed 6/6
  in 19.10 seconds; calculations passed separately in 0.08 seconds. The
  source-kit allowlist includes all 1,115 required tracked files. Independent
  review found no remaining scoped font-size or punctuation blocker.

Local evidence is under `artifacts/area-display-20261001/native-physical-text`:
`appraisal-area-display.png`, `area-display-zero.pdf`, `area-display-six.pdf`,
`area-display-zero.png` and `independent-pdf-text.json`.

The Release executable SHA-256 is
`02328519dc1a8a44acef9821cc95130fb1787d4724c6ed5d1e43eca52f64121e`.
Run `scripts/run.ps1 -Configuration Release` to supply its local DLL/plugin
paths. The earlier installed rotation checkpoint remains separate and does not
contain these Area display changes.

These are controlled native checks on the development host. The user's exact
gesture, clean-machine installation, network-denial execution and complete
production output remain unverified. Canvas annotation text follows a separate
rendering path and is not covered by the sheet punctuation correction.
