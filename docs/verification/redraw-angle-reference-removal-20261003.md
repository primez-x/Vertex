# Redraw reference decisions

This closes an area-editing gap within the existing production scope. It does
not certify Apex native-file compatibility, ANSI measurement standards, or the
complete production release. User-observed acceptance remains pending.

## Behavior

Redrawing an area now offers Keep and map or Remove for an affected automatic
angle label. Manual dimensions and supported endpoint constraints retain their
existing choices. Automatic edge measurements regenerate from the replacement
outline. Area labels, unrelated objects and their references remain intact.

Cancel keeps the editable redraw without applying a reference decision. Apply
commits the reviewed replacement and its reference decisions together. Undo
restores the prior geometry and annotations; Redo restores the accepted result.
An undone completed input remains a retired recovery record, without restarting
drawing. Saved unfinished redraws resume through the canvas workflow.

The new permission is explicit and versioned. Strict redraw intent v5 must
actually remove an affected automatic angle. Workspace decision seal v3 binds
that choice to the target, mappings, removed references and replacement hash.
Unused permission, unrelated IDs, automatic edge/area removal, invalid mappings
and conflicting retained constraints remain refused. Legacy intent v1-v4
encodings and admission remain unchanged.

Native format23 and extraction21 preserve retained v5 decisions in current,
undone and deleted history. A geometry derivation also preserves the decision
when boundary entities are imported without their originating command history.
Both native format markers and the logical digest must satisfy the reader floor.

## Verification

The Release application and affected fixtures built. Ten core/persistence/
workspace executables passed: boundary integrity, document, document digest,
project store, project exchange, project workspace, lifecycle validation,
workspace Finish, archive restoration and history records.

The new native fixture passed real review Keep/map and Remove, actual redraw
action and canvas clicks, saved draft recovery, Finish Cancel/retry, archived
retired decision seal v3, strict v5 intent, source preservation, current area and
edge calculations, and exact Undo/Redo after editable reopening. The test
releases its original file owner before editable reopening; concurrent opens
remain read-only by the existing ownership policy.

Boundary editing, desktop workspace recovery and the appraisal Details panel
also passed. Windows were offscreen with hidden test surfaces and noninteractive
error handling. The user's application was neither launched nor closed.

Independent source review found a missing derivation on plain changed-count
redraws. It was corrected; imported-proof regressions passed. Root inspected
the real native reference-review capture and integrated interfaces. Native
runtime inspection found 113 binaries and zero unresolved imports.

The full boundary workflow also passed after its obsolete expectations were
corrected. Chained drawing uses practical relative-length magnets while
preserving heading; dimension placement still uses the world grid. The fixture
checks explicit metric/Imperial intervals, live preview/commit agreement,
unchanged existing geometry across zoom and unit changes, and exact persisted
coordinates. Area moves and rotations retain the existing dependency-aware
grouped transform proof, while unrelated objects and atomic Undo/Redo checks
remain intact. No production drawing or transform behavior was changed to
satisfy these older expectations.

All fifteen affected executables passed. Requirement schema and source-kit
tracked coverage checks passed. The release audit still reports production
acceptance false and 208 unresolved release-evidence gaps, including broader
compatibility and standards qualification. User checklist U053 documents the
actual Keep/map and Remove workflow without marking user acceptance complete.

Ignored evidence is under `artifacts/redraw-angle-removal-20261003`. The final
production gate remains open; this focused delivery does not reduce its scope.
