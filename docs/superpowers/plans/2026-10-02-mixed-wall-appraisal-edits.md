# Mixed wall and object edits with linked appraisal measurements

Outcome: one authored operation may edit physical walls together with symbols,
labels, ordinary metadata and referenced assets while updating every eligible
current exterior measurement. Preview, Apply, Undo/Redo and saved history must
describe the same complete operation. Existing stale-source repair stays explicit.

Baseline: HEAD 13c58f7 completes ordinary wall-only source changes, but refuses
non-wall supplements and asset changes when automatic exterior updates are
required. This makes an ordinary mixed selection move unnecessarily fail.
The previous goal turn made verified progress: committed source, test evidence,
offline installation and desktop shortcut publication all changed authoritative
state. The full production objective remains active.

Ownership: core worker owns document command/admission and core regressions;
persistence worker owns digest, native/extraction floors and persistence checks;
desktop worker owns application authoring and native interaction fixtures. Root
owns integration, documentation, generators, builds, Git and installation. Writers
freeze before any build. Preserve unrelated temp.txt and older history semantics.

- [x] Add envelope v7 with explicit ordinary supplemental entity/asset intent;
  retain exact v1-v6 representation and replay. Admit the original ordinary
  command before adding source completion. Raw measured-owner/dimension payloads
  cannot replace typed geometry or inferred source lineage.
- [x] Apply all lanes atomically with duplicate/overlap, reference, asset,
  constraint, provenance and complete source-consumer checks. Reject invalid
  candidates without partial entity, asset or history mutation.
- [x] Verify actual mixed wall/object movement through the desktop authoring
  path, including preview, retained dimensions/facts, appraisal totals,
  one-step Undo/Redo, both units and save/reopen.
- [x] Require native 20/extraction 18 for retained v7, including undone/deleted
  work. Verify exact round trips, tampering/downgrades and old digest vectors.
- [x] Run affected checks and integrated review; resolve consequential findings,
  update practical user checklist and documented project format. Prepare matching Git and
  offline delivery.

This checkpoint is part of the original full scope. It does not certify Apex
compatibility, physical hardware, all architectural workflows or production
acceptance. Any remaining mixed-operation restriction remains a gap to fix.

Delivery evidence: the final commit/push and installed executable are recorded
under artifacts/mixed-wall-measurement-20261002/delivery.json. The source plan
checklist records the implemented and locally verified checkpoint; it is not
production acceptance or a user testing result.
