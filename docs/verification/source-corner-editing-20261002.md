# Coordinated exterior-corner editing — 2026-10-02

## Scope

A current measured exterior can be reshaped through its stable corner coordinates
or canvas handle. The operation reconstructs physical source walls, retains
thickness and source lineage, moves attached partitions and hosted openings,
solves saved constraints, and regenerates measured consumers in one revision.
Appraisal calculations and dimensions read that same document.

Native format 22 and extraction version 20 retain the qualified version-8
command proof. Historical command validation remains unchanged. Stale sources,
ambiguous offsets, contradictory constraints, invalid openings and unproved
physical changes are refused without changing the document.

## Evidence

The final Release build passed. Nine focused core suites passed, including the
solver, wall measurement, authoring, document history, storage, extraction,
digest, appraisal and integrity checks. All six focused native suites passed:
boundary editing, saved-plan vertex editing, connected-wall canvas movement,
wall measurement, ANSI-oriented appraisal and the Details panel. The independent
review approved the integrated source; distribution verification is recorded
separately in the delivery evidence. Evidence is retained under
`artifacts/source-corner-editing-20261002`.

Core fixtures cover unequal thickness, curved outlines, reversed lineage,
cross-layer attachments, reciprocal partition joints, persistent length locks,
inactive alternatives, source freshness, command tampering and retained replay.
Desktop fixtures exercise the actual coordinate dialog in both measurement
systems, proposal/cancel/apply, stable identifiers, one Undo/Redo, reopening,
GLA changes, and the asynchronous canvas corner gesture.

## Limits

This change closes a specific appraisal editing gap. It does not certify full
Apex parity, native Apex-file compatibility or production readiness. The
ANSI-oriented profile still requires final validation against the normative
standard; it must not be described as ANSI approved or certified.

The 366-wall preview fixture took 2337.91 ms on the current machine, with 360
unrelated walls bound to a floor level. This is a timing observation, not a full
responsiveness certification; large-project proposal latency remains a gap.
User-observed resolution has not been established.
