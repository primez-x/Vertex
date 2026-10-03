# Shared analytical curve contacts — 2026-10-03

## Scope

This checkpoint corrects analytical line/arc contacts in the common geometry
engine used by wall junctions, boundary validation, constraint integrity, and
appraisal deductions. Canvas directional alignment now uses the same contact
kernel. It does not establish complete Apex parity or ANSI compliance.

## Reproduced failures

The original radius-square subtraction returned one false midpoint contact for
an arc from (0, 0) to (2, 0) with sweep 1e-8 radians and a line at y = -1e-9.
The two analytical contacts are x = 1 ± sqrt(0.6). The new core regression failed
against the original production source before implementation.

Independent review found that using the caller's metre tolerance for arc-side
membership could admit the opposite part of the supporting circle. At the
default tolerance, the same positive arc against y = +1e-9 must have no arc
contact. That regression also failed before correction.

A diagonal line x + y = the represented `sqrt(2.0)` strictly misses a unit
semicircle. Normalizing its direction could displace the supporting line and
produce two false contacts. The corresponding regression failed against the
first implementation; the immediately adjacent inside value is a separate
two-contact control.

The same error occurred with a tilted diameter and a line at its rounded
`sqrt(2.0)` bound. The supporting circle is defined by the raw half-chord
squared, not by squaring a rounded radius. A distant shared-endpoint secant
also lost its second contact when a rounded world midpoint erased the endpoint
derivative. Both have independent geometric controls and reproduced failures.

A semicircular deduction with its upper extremum 5e-8 m below a rectangular
outer boundary passed strict root-only pairing at a 1e-7 m topology tolerance.
It must be rejected as insufficient clearance. A 2e-7 m gap is the valid control.

## Implementation

The shared solver uses a normalized local chord-frame circle equation and
stable quadratic roots, avoiding a distant center and cancellation between
large radius squares. Finite line endpoints are checked after solving the
near-arc supporting line. Signed sweeps, selected arc-side membership, root
residuals and numerical uncertainty determine admission. Exact represented
half-turns use canonical midpoint centers and extrema. The public mixed
line/arc wrapper translates about the arc rather than a distant line endpoint.

Near-zero discriminants require an independent sign proof or return an empty
indeterminate result. Half-turn refinement uses power-of-two scaling and
compensated products on the original line and half-chord. Exact cardinal
tangencies require representable canonical geometry; equality to a rounded
bound is insufficient. Exact shared stations anchor a factored quadratic that
retains the possible second contact. Symbolic signed half-chords keep its
derivative independent of world midpoint rounding. Whole finite-line chord-side
separation excludes tangencies on the unselected circle portion.

Internal validator calls normalize at the arc start as well. Contact restoration
back to world coordinates must remain finite and round-trip within the caller's
local tolerance; otherwise the entire result is empty and indeterminate. This
prevents an unrepresentable world contact from silently hiding an intersection.

The canvas retains exact structural endpoint admission, ray clipping,
directional ordering and transverse cursor preservation, and delegates the
curve intersection to the shared engine.

Hole validation separately checks analytical line/arc clearance using endpoint
and circle-normal stationary candidates. It emits no contact coordinates and
rejects numerically unresolved proximity. The safeguard is confined to hole
pairing; ordinary boundary adjacency exemptions are preserved.

## Verification

The final Release build passed all 16 affected consumer executables: geometry,
geometry operations, calculations, appraisal documents, wall measurement,
wall junctions, constraint integrity, boundary arc editing, directional canvas
alignment, witness alignment, drawing measurement, desktop wall measurement,
boundary authoring sessions, boundary workflows, appraisal Details, and ANSI
appraisal desktop workflows. Each final process exited zero after the last
coordinate-restoration safeguard. Local ignored logs and actual exit receipts
are under `artifacts/shared-curve-contacts-20261003/`.

The source-kit tracked-file check passed with 1,259 explicit entries. Runtime
inspection covered 113 component binaries with no unresolved imports. Root
inspected the actual native Details capture, including GLA, gross/deduction/net
values, selected source geometry and canonical dimension text. Independent
bounded review accepted the internal contact-normalization safeguard; root
reviewed the integrated change and its remaining qualification limits.

## Qualification boundary

GEO-BASE-001 remains in progress. Separate center-based shallow-arc containment,
arc/arc numerical qualification, independent high-precision fixture parity,
broader topology workflows, native Apex compatibility and ANSI normative
validation remain open. Historical acceptance evidence is preserved, not
rewritten to imply current full production acceptance. Offscreen native
fixtures do not establish user-observed resolution, physical-device behavior
or clean-machine installation.

At a shallow arc's rounded extremum, the true contact sign/locations can remain
unresolved. The previous directional fixture wrongly called the represented
bound a true tangent: for sweep 1e-8, its stored sagitta magnitude is sweep/4,
while the true magnitude is tan(sweep/4), strictly larger. This checkpoint
refuses to invent a midpoint contact there; it still resolves the separately
tested two contacts at y = -1e-9. Refining transcendental near-tangencies further
remains a qualification gap. Shallow line/arc clearance can likewise reject
uncertain proximity conservatively.

The restoration safeguard checks output translation, not arbitrary original
input-subtraction accuracy. Broader IEEE and input-translation qualification
remains open. The public contact API retains its separately tested approximate
world-coordinate restoration behavior.
