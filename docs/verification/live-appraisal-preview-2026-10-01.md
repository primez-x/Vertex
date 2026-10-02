# Live appraisal area preview

Accepted boundary-corner previews now show candidate net areas for both the
changed boundary and unchanged deduction parents. They use the committed
calculation report's qualification, precision, units and semantic phase.
Candidate label layout includes deducted footprints and updated text metrics.

The candidate is obtained by replaying the verified typed command against an
immutable snapshot. Identity, source revision/digest, proposal integrity and
recomputed-result checks remain shared with Apply. Preview does not mutate
the live document, saved state, assets or history.

## Evidence

- The real pointer regression initially failed because numerical labels were
  suppressed. It now passes: moving a corner of a 25 ft² garage by 0.2 m on
  both axes yields 21.72 ft², leaving its 100 ft² parent at 78.28 ft².
- Cancel restores committed geometry and values. Release commits one revision
  matching the preview, and Undo restores the prior geometry and totals.
- The complete Appraisal desktop workflow and connected-wall canvas checks
  passed. Core checks passed for exact typed replay, unchanged assets/saved
  state, and stale, foreign, rejected and tampered proposal refusal.
- Root inspected the actual rendered canvas in
  `artifacts/live-appraisal-preview-20261001/green/live-net-area-preview.png`;
  both candidate values are visible and readable.
- An independent read-only advisor found no actionable issue in candidate
  authority, worker-thread ownership or candidate label calculations. This
  was source review, separate from executable verification.

Drawing remains on the conventional 2D surface. Requests to switch to 3D with
an unfinished drawing preserve the draft and now restore the correct checked
mode action. Input from the inactive canvas cannot add draft nodes. Starting
Draw First or Define First from 3D routes to the 2D authoring canvas.

The rendered evidence and controlled native checks are not user-observed
resolution or full production/Apex compatibility certification. Placement can
still omit a label when no readable position fits. The full release goal
remains open.

The subsequent [label-placement checkpoint](plan-label-placement-2026-10-01.md)
retains labels through exterior/manual placement and verifies qualification
transitions in live proposals. The omitted-label limit above is historical.
