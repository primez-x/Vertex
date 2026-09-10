# Frozen boundary recovery reference

`boundary-recovery-v24a.json` was generated before the compact history refactor
using the frozen v24a session header, recovery fixture source and existing
Release libraries. The independent reference runner then reproduced it in
both Debug and Release against the pre-refactor engine.

It contains 13 cases and 66 history positions covering all eighteen normalized
actions, both drawing modes, manual/automatic/pending dimensions, multiple
accepted chains, counter gaps, existing redo, reset and cancellation. Every
position includes a branch, its undo and redo, and explicit redo discard.
Public geometry is recorded as IEEE-754 bit patterns; checkpoint JSON is also
stored as an exact byte string. This protects negative zero, adjacent doubles,
stable identities, classification, pointer state and complete local history.

Fixture SHA-256:
`51926c2d4dc130eb6db33900b17657bbf4719ddb7c7464f504e9f58379f2904e`.

Producer authoring library SHA-256:
`46220912587b7bc370c6adeeb45d03a7fba3d9ef2af19cb79f18795066e11b66`.

Run the `boundary_recovery_reference` CTest after a normal documented build.
Do not regenerate this fixture with the new implementation merely to make a
comparison pass. Diagnose any discrepancy against the retained producer and
the intended unchanged public/wire contract. This is internal engine parity
evidence; it is not an Apex compatibility fixture or production certification.
