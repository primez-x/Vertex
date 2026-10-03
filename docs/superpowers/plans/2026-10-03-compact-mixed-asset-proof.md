# Compact mixed asset proof Implementation Plan

> **For agentic workers:** Use the existing subagent-driven assignment. Root owns native storage/extraction, integration, verification, Git and packaging; all source writers freeze before builds.

**Goal:** Preserve full accepted asset edits in an atomic measured-wall transaction without duplicating asset bytes inside bounded proof JSON.

**Architecture:** Mixed exterior-source proof 9 extends proof 7 with compact asset references. Full assets remain independently stored in each immutable revision. Decoder hydration validates exact content and metadata identity before existing deterministic replay; old inline proofs retain their original semantics.

**Tech Stack:** C++20, nlohmann JSON, existing SHA-256, SQLite native archive.

**Spec:** docs/verification/drawing-and-appraisal-details-2026-10-02.md; mixed source-completion contract in docs/project-format.md; CORE-DOC atomic editing/history/recovery requirements.

## Global Constraints

- Proof 8 already means exterior corner intent; preserve it unchanged.
- New proof 9 retains the proof-7 outer fields; it cannot borrow corner authority.
- Asset limit remains 256 MiB, aggregate persisted assets across retained history 512 MiB, JSON column 1 MiB and aggregate JSON 64 MiB.
- New upsert references carry exactly id, media_type, sha256, byte_size and metadata_sha256; metadata digest hashes canonical metadata.dump(). No bytes_hex or inline metadata appears in a reference.
- Erase records retain kind and asset_id. Empty proof-9 markers remain explicit intent.
- Native format 26/extraction 24 qualify proof 9 across retained Undo/deleted history; lower formats cannot silently drop it.
- Original source files, unknown metadata, exact history and atomic Undo/Redo remain intact.
- Standalone reference-image import is separate; evidence must not claim an unobserved combined UI gesture.

## Review Focus

- Contextless/missing-reference decode must refuse rather than fabricate bytes.
- Changed metadata with identical bytes must retain its own exact identity.
- Tampered sizes/hashes/media types/IDs, duplicate references and missing result assets must reject atomically.
- Old proof 7 must keep byte-for-byte wire roundtrips, while proof 8 remains corner-specific.
- Recomputed logical digests must not authorize native downgrade or mismatched same-revision hydration.

### Task 1: Versioned proof and workspace transport

**Files:** Worker owns include/sketch/document.hpp, src/core/document.cpp, src/core/project_workspace.cpp and focused wall/document/workspace fixtures.

**Interfaces:** Append bool supplemental_asset_reference_completion to ApplyBoundaryConstraintChanges. Extend command_from_json(value, const std::function<const Asset*(std::string_view)>& asset_resolver = {}). command_to_json emits proof 9 only for explicit reference intent; new mixed asset completion sets it. Workspace resolves original typed upserts or current assets without copying an asset pool.

- [ ] Replace the existing 530 KiB rejection fixture with exact accepted completion and compact-proof assertions; freeze and run the observed baseline failure.
- [ ] Implement strict reference codec/hydration and replay-preserving command transport.
- [ ] Cover large payloads, metadata-only updates, erases, no-op and strict tamper/legacy refusal.

### Task 2: Native history and extraction

**Files:** Root owns include/sketch/project_store.hpp, src/core/project_store.cpp, src/core/project_exchange.cpp and native/extraction fixtures.

**Interfaces:** Defer only proof-9 decoding until independently validated same-revision assets are loaded; supply that revision's asset resolver. required_format_version returns at least 26 for the explicit proof marker. Extraction advertises 24 and keeps full asset payloads in its existing separate asset representation.

- [ ] Extend native reader bounds/markers without raising any byte budget.
- [ ] Verify active, Undo and deleted history; exact save/reopen/recovery, metadata identity, recomputed-digest downgrade and tamper refusal.
- [ ] Verify extraction version and referenced asset availability; do not claim a JSON importer.

### Task 3: Integrate and deliver

- [ ] Freeze all writers, build application and affected targets, then run focused core/native/desktop checks.
- [ ] Obtain one read-only consequential review for data integrity and backward compatibility; resolve required findings.
- [ ] Update format/workflow/requirements/verification/source-kit documentation without changing historical evidence.
- [ ] Commit only scoped paths, push and verify remote ref; package/install and verify the Windows runtime/source kit and controlled shortcut.

This removes a documented original-scope transaction limit. It does not substitute for full Apex compatibility, ANSI normative qualification or unified production acceptance.
