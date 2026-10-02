# Drawing interactions and visible appraisal details

This checkpoint addresses project-level appraisal visibility, useful mouse-drawn
lengths, right-button panning/cancellation, and mixed object moves that include
walls with linked exterior measurements. It is part of the active full production
goal, not production or Apex compatibility certification.

## Implemented behavior

Details is the third left-panel tab beside Layers and Library. It obtains GLA,
separate categories, building/floor contributions and boundary traces from the
authoritative appraisal document report. It does not require a canvas selection.
Area rows expose analytical edge lengths, perimeter, gross area, deductions,
physical net area, factors and rounding. Setup, Edit facts, Show on canvas and
Full report connect to the application's ordinary authoring/report paths.

Mouse drawing quantizes length relative to the current start point, using common
increments selected by units and zoom. Exact object endpoints and closure take
priority. Exact typed input and Snap-off bypass this quantization. Transient
imperial drawing labels use inch fractions where exactly representable; arbitrary
retained endpoint precision is not silently changed.

Right-button dragging pans, including when a drawing or new symbol is pending.
A stationary right-click cancels that pending action. Already committed objects
remain; idle stationary right-click opens the relevant context menu.

Mixed wall, furniture, label and ordinary metadata/asset changes can update linked
exterior measurements atomically. Command envelope v7 retains exact supplemental
intent; native format 20 and extraction version 18 are required when retained
history contains it. Existing v1-v6 representations and semantics remain intact.

## Independent source review

The compound-command advisor identified numeric JSON exactness, protection of
fresh/removed typed dimensions, and valid same-wall metadata admission. All three
were corrected with focused coverage. The final source verdict approved that
change, explicitly subject to root's build and runtime verification. Drawing and
Details changes were outside that review and require root's integrated review.

## Verification

Release builds passed. Eight core checks and seven native desktop checks passed:
wall measurements, constraint authoring, document admission, digest, native store,
exchange, appraisal document, boundary arcs, Details, drawing measurements,
complete wall-measurement desktop, connected-wall canvas, boundary canvas, batch
cloning and wall-chain connections. Native checks ran offscreen with isolated
settings and noninteractive error handling; no user application was launched.

The build/check iterations corrected test fixture defects: a private restore API
call, Qt findChild on a class without Q_OBJECT, malformed annotation/phase
payloads, the wrong exception class for document admission, and a library fixture
that emitted double-click instead of the actual itemActivated placement signal.
The phase test now verifies rejection and unchanged authoritative state; it does
not bypass validation to manufacture invalid state.

Root inspected native light/dark captures. Clipped text and an unthemed scroll
background were fixed; GLA has prominent sizing and unnamed areas use readable
names. Show on canvas reveals the area's floor/layer, selects it and fits the
measurement workspace. The final presentation polish was rebuilt and checked
with the native Details fixture. Captures and actual exit records live under
artifacts/mixed-wall-measurement-20261002. The compound-edit native fixture covers
Imperial/Metric and straight/curved shell geometry, furniture, labels, a hosted
opening, an unselected exterior owner, retained facts/deduction, exact preview,
cancellation, atomic Undo/Redo and native reopening.

Root's integrated review found no remaining blocker for these changes. This
verification is technical evidence; user-observed acceptance remains pending.
Actual commit, push, installation and executable hashes are recorded separately
in artifacts/mixed-wall-measurement-20261002/delivery.json after installation.

## Remaining production gaps

The current appraisal rules are Vertex's versioned declared-facts policies.
ANSI status remains unverified. The source-backed standards work is recorded in
docs/requirements/appraisal-standards-gap-review.md and remains in scope.
Mixed operations carrying large changed assets can still exceed the existing
1 MiB encoded proof limit; this is an implementation gap, not a release exclusion.
Full Apex compatibility, device evidence and the unified production acceptance
gate also remain open. User checklist U326-U330 remains **Not tested** by the user.
