# Measurement group movement implementation plan

**Goal:** Move multiple measured areas and mixed area/architectural/presentation selections as one reversible, receipt-preserving edit.

**Architecture:** Add a typed `TranslateBoundaries` command carrying boundary translations and ordinary entity changes. Replay all translations in a detached map, validate ordinary changes against that intermediate state, then admit the complete final state once. Persist the exact typed proof through document history, project storage, exchange, digest, and recovery accounting.

**Technology:** Existing Windows Qt/C++ desktop, deterministic document model, JSON command codec, SQLite project storage.

**Scope:** This implements the existing production plan's group selection/editing, reversible operations, authoritative geometry, and recoverable project history. It does not certify Apex file compatibility or production release qualification.

## Ownership and interface

- Core worker: `include/sketch/document.hpp`, `src/core/document.cpp`, `include/sketch/project_store.hpp`, `src/core/project_store.cpp`, `src/core/project_exchange.cpp`, `src/core/document_digest.cpp`, `src/core/workspace_recovery_budget.cpp`, `src/core/project_workspace_preview_adapters.cpp`, and relevant core/storage tests.
- Root: `src/desktop/main_window.cpp`, `tests/desktop_smoke.cpp` or an existing focused native desktop test, CMake if necessary, documentation, generators, builds, Git, and integration.
- Read-only advisor: challenge retained proof admission and persistence boundaries.

`TranslateBoundaries` contains `Revision expected_revision`, `std::vector<BoundaryTranslation> translations`, `std::vector<EntityChange> entity_changes`, and `std::string message`. `Command` gains this type; `RevisionRecord` gains optional `boundary_translations` of this type. The strict version-1 JSON envelope uses kind `translate_boundaries`, fields `translations`, `entity_changes`, `message`, and `expected_revision`. Project format 9 is required only when the new proof occurs; older histories retain their existing required format.

## Requirements and verification

- [x] Replay nonempty translations with unique valid owner IDs and finite offsets. Preserve receipt entries, exact measurement expressions, geometry derivations, segment/vertex IDs, dependent dimension anchors, and metadata.
- [x] Reject supplemental changes overlapping translated owners or their dependent dimensions, duplicate changes, invalid ordinary transitions, stale revisions, conflicting fixed constraints, and forged/missing history proof without changing live state.
- [x] Validate final constraints once so connected owners moving together are admitted, while partial/conflicting moves refuse atomically.
- [x] Persist and replay the typed batch through command JSON, save/reopen, exchange, digest, undo/redo, recovery accounting, and navigation history. Keep old formats/commands byte-compatible.
- [x] Partition canvas selections into qualified measured owners, ordinary boundaries, architectural roots, and presentation entities. Merge all changes into one command without independently committing or admitting an owner preview.
- [x] Add core regression coverage for two measured areas plus a symbol, derivation-backed geometry, constraint refusal, proof tampering, and persistence.
- [x] Add a native canvas workflow for dragging two measured areas plus a symbol. Verify exact displacement, unchanged IDs and measurements, one history entry, undo/redo, and save/reopen.
- [x] Build affected targets, run focused checks and `git diff --check`, and inspect the integrated diff. Delivery is retained in the scoped Git commit and verified remote ref.

Observed results and limitations are recorded in
[the verification report](../../verification/measured-group-movement-2026-09-30.md).

## Review focus

- Coincident owners temporarily disagree during detached replay: final-state admission must not reject a valid group solely because an intermediate owner has not moved yet.
- Supplemental changes must not overwrite a translation's boundary or dimension proof.
- Named plans must convert the gesture delta to model coordinates before publication.
- Constraint failures must leave both presentation and geometry at the original revision.
- Missing/forged proofs after save or exchange must fail closed; adding a command must not silently weaken old history validation.
