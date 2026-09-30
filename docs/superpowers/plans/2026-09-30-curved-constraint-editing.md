# Curved connected editing

Outcome: enable existing endpoint constraints and connected geometry edits for analytical curved walls and measurement/room boundaries, retaining signed sweeps, identifiers, original entries, and atomic undo/replay. This checkpoint does not reinterpret endpoint distance as arc length or add tangent constraints.

## Ownership and contract

- Core worker: authoring, integrity, analytical topology, wall edit replay/receipts, document command codec, and corresponding core tests. Existing seven relations retain their point/chord meanings. Fixed sweep is immutable during endpoint solving. Historical straight wall proofs retain their original encoding and validation; curved proofs use explicit version 2 and outer command version 3.
- Persistence worker: project/exchange minimum-reader gates, including retained undone history, and storage/exchange regression coverage. Curved wall proof history requires project format 10 and exchange format 7; existing older payloads remain compatible.
- Root: desktop entry points, true-arc preview rendering, connected canvas editing, native user workflow tests, documentation, integration, build, packaging, Git, and delivery.
- Advisor: read-only contract challenge and consequential replay/provenance review.

## Verification

- [x] Existing straight constraints remain compatible.
- [x] Curved wall/boundary endpoints solve all existing relationships without flattening sweeps.
- [x] Authoring checks analytical winding, intersection, collapse, reversal, hosted openings and conflicts before committing. Shared topology admission for direct typed replay remains open.
- [x] Original curve construction expressions and metadata survive derivation and replay, including transforms and explicit reconstruction.
- [x] Command codecs and saved/undone history enforce the new reader floor.
- [x] Native dialog and canvas workflows display actual arcs and commit connected edits; undo/redo and save/reopen agree.
- [x] Focused runtime checks and independent source review pass.
- [ ] Diff check, runnable offline checkpoint, scoped commit, push and remote-ref verification.

Remaining production gaps, including a separately defined physical arc-length constraint, tangent relationships, exact Apex compatibility, and clean-machine qualification, stay visible in the production ledger.
