# Saved-view constraint previews Implementation Plan

> **For agentic workers:** Use the existing subagent-driven execution assignment. Root owns integration, verification, Git and packaging; source writers freeze before builds.

**Goal:** Make exact editing previews honor the same saved-view appearance and visibility as the committed canvas, including locally recovered physical sources and their measurements.

**Architecture:** Resolve presentation using canonical graph-owner/local-view identity over an immutable document snapshot. Carry that resolution into candidate geometry projection rather than independently applying the global appearance mask. Keep analytical geometry, constraint solving and appraisal qualification independent of presentation.

**Tech Stack:** C++20, Qt 6 Widgets, existing native constraint/document adapters.

**Spec:** docs/plans/2026-10-03-output-view-appearance.md; docs/production-plan.md; APX-ANNO-003 and GEO-CON requirements in docs/requirements/apex-parity.json.

## Global Constraints

- Windows 11 x64; imperial and metric; residential and light commercial.
- Graph entity plus local view ID identify a saved view.
- Global appearance is inherited, then whole-view style, then per-object style.
- Local visibility cannot bypass hidden layers, phases or restricted source lists.
- Hosted openings follow their resolved host visibility.
- Measurements and appraisal calculations use analytical source geometry.
- Stale or unavailable proposals never authorize a commit; accepted edits are atomic and undoable.
- Existing files, source identity, opaque metadata and serialized appearance remain intact; no new format is needed for a derived-scene repair.

## Review Focus

- A locally visible but globally hidden wall must remain visible during exact previews.
- Same-named views in different owners must not exchange presentation state.
- Hidden layers/phases/source-filtered objects must not leak into a preview.
- Dimensions, hosted openings and appraisal consequences must track proposed geometry.
- Cancel, stale completion and Undo must restore the exact source and normal retained scene.

### Task 1: Restore consistent preview presentation

**Files:** Modify src/desktop/main_window.cpp; test tests/output_view_appearance_desktop_tests.cpp. Worker owns these paths exclusively until returned.

**Interfaces:** Preserve existing captureConstraintGeometryPreview(PlanCanvas*, Revision) and candidate projection/constraint editing APIs. Consume the current canonical saved-view resolver and retained scene; produce the same exact-proposal contract for PlanCanvas without altering document commands.

- [ ] Add native regression using a globally hidden wall recovered in one saved view, an actual exact edit preview, and comparison with committed geometry/style/measurements. Add hidden-source and cancellation guards.
- [ ] Freeze source; root builds output_view_appearance_desktop_tests and runs it offscreen. Retain the observed baseline failure.
- [ ] Correct the shared capture/projection visibility policy. Retain legacy inheritance and organizational/source/host filters.
- [ ] Freeze source; root builds the application and affected native targets, runs focused checks, and inspects native captures.
- [ ] Root reviews source, interfaces, invariants and actual evidence; fix required gaps and rerun only affected checks.
- [ ] Update user workflow tasks, requirements note, verification and source-kit inventory; commit scoped paths, push and verify the remote ref.
- [ ] Package and install the verified Windows build without closing the user's app; verify hashes and conditionally update the controlled desktop shortcut.

This closes a specific original-scope editing gap. Full Apex compatibility, ANSI normative qualification and unified production acceptance remain required and unproven.
