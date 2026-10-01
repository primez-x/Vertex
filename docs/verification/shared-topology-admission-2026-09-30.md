# Shared topology admission evidence

The prior connected-editing checkpoint documented that analytical topology
checks ran during interactive authoring but were absent from direct typed
command replay. This work brings those paths under one deterministic validator.
It preserves original project files and rejects unsafe history rather than
repairing or replacing the saved drawing.

## Baseline observations

The Release document and storage targets built with the new regressions before
changing admission behavior. Both failed as expected:

- `document_commands` accepted a direct typed preview that introduced crossing,
  overlap or an undeclared endpoint contact (0.11 seconds).
- `project_storage` accepted a saved curved-wall crossing despite the test's
  expected semantic refusal (1.57 seconds).

The saved fixture starts with a curved wall from (0,0) to (4,0), signed sweep
0.4 radians, and a straight obstacle at x=6 from y=-10 to y=10. Its valid
recorded edit extends the curve to (5,0). The forgery changes both the retained
wall and matching typed command to end at (7,0), then recomputes the logical
digest. The test independently establishes that the proof reproduces the exact
stored wall and that the resulting analytical arc properly crosses the
obstacle. Checksum or command/result mismatches cannot explain the refusal.

Logs are retained in `artifacts/shared-topology-20260930/`:
`baseline-document-red.log` and `baseline-storage-red.log`.

A further analytical regression failed against the initial shared validator:
`document_commands` accepted a nonbasis winding reversal between two curved
paths sharing endpoint tangents (0.12 seconds). Before testing refusal, the
fixture proves that all source/result loops are valid, both simpler cycle
signs remain unchanged, the loop between the paths reverses, and all ten wall
pair intersection kinds and contact-role signatures are unchanged. The log is
`tangential-theta-red.log`; aggregate crossing checks or a cycle basis alone
cannot detect this case.

Independent review then identified a narrow straight variant: 1,000,000-metre
extent with two distinct paths separated by a millimetre. Reusing a relationship
angle tolerance to group local branches hid that order. Its independent loop
and pair-contact assertions also passed before `document_commands` failed
with `narrow straight theta preview accepted a nonbasis winding reversal`
(0.12 seconds), recorded in `narrow-theta-red.log`.

The straight wall-only service regression failed against generic history:
`constraint_authoring` reported `straight wall-only authoring must retain typed
endpoint intent` (0.05 seconds), recorded in `straight-intent-red.log`.
Command envelope 4, project format 11 and exchange format 8 now encode that
intent. Authoring verifies service dispatch separately from the solver-free
storage and exchange suites, which exercise the resulting typed command.

## Implemented boundary and verification

Endpoint authoring and typed command replay now share one analytical validator.
It examines every intersection witness, original endpoint identities, before
and after coincidence graphs, floor/vertical-plane grouping, cyclic branch
order and analytical winding. Tangent ordering uses numerical roundoff rather
than a relationship residual tolerance; tangent ties use signed curvature.
Indistinguishable tangent/curvature ties require analytical overlap before
stable identities can break a tie. Bridges and separate floors do not create
or suppress protected cycles.

Release `vertex`, `vertex-cli` and the nine focused test targets built. The
integrated run passed authoring, workspace preview adapters, named-plan desktop
editing, constraint dialog, constraint integrity, organization, document
commands and storage. Exchange initially failed a fixture's hardcoded count
of directories after additional fixtures were added; its cleanup assertion now
compares the exact directory-name set before and after each injected failure.
The affected exchange rerun passed. Final storage/exchange checks, including
mixed v4/v3 history, passed 2/2 in 4.08 seconds:
`persistence-final-ctest.log`. Unchanged checks are recorded in `final-ctest.log`.

The passing tests establish actual straight-wall authoring emits envelope 4;
valid v1/v2/v3 encodings retain their rules; v4 geometry, receipts and navigation
reopen exactly; undone/deleted and later curved records cannot lower the v11
reader or exchange-8 floor; forged matching straight and curved states reject
even after digest recomputation; source/rejected file hashes remain unchanged.
Exchange exercises decoded commands and exported retained history. It has no
history importer, and these tests do not imply one exists.

Independent review accepted the final numerical topology guard after its RED
and GREEN observations. Root reviewed the integrated command/version/storage
boundary and fixed the standalone storage/exchange fixture dependency so those
suites remain solver-independent. `git diff --check` passed.

## Remaining qualification

Historical generic straight-wall commands do not contain endpoint-edit intent;
it is not reconstructed from their action text. Large cyclic-component latency,
physical arc-length locks, tangency, exact Apex compatibility, clean-machine
network-disabled execution and the other production acceptance gates remain
open. This is an implementation checkpoint, not the completed replacement.

Fresh bundled installation and installed-runtime evidence are pending.
