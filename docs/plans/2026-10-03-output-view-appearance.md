# Saved-view appearance

This implements independent drawing appearance for saved plan, elevation and
section views within APX-ANNO-003. It does not remove the remaining SVG palette,
Apex compatibility or production acceptance requirements.

## Behavior

The existing saved-view settings expose Drawing appearance. A whole view or
one physical source object within it can inherit appearance or use its own
outline, fill, pattern, hatch scale and paper line weight. View-wide visibility
controls the entire view, including its annotations and reference content.
Hidden views remain available in the saved-view selector and settings.

Global object appearance is the fallback, followed by view-wide style and then
the view's explicit object style. Local visibility can recover an object hidden
by global appearance, but cannot override hidden layers, phases or a restricted
view source list. Filters retain dormant style entries; deleting a source object
removes its entries in the same transaction. Geometry and appraisal quantities
are independent of this presentation.

Appearance belongs to the pair of graph entity and view identity. A view ID
alone is insufficient because separate graphs may reuse it. Resolve appearance
on the retained derived scene before canvas and sheet/PDF consumers use it.

## Storage and editing

Optional ViewPresentation.appearance retains explicit intent. Absence means
inheritance and emits the existing model version 6. Presence requires model
version 7, native project format 24 and extraction version 22, including when
retained only in undone or deleted history. Earlier readers must refuse this
meaning rather than discard it. Object references are canonical and unique;
Document admission checks their existence. Opaque unrelated entity extensions
and protected owner flags survive targeted property edits.

Cancel, unchanged Apply and Reset on an absent override add no history. Stale
modal contexts and read-only documents refuse edits. One accepted edit has one
Undo step; save/reopen retains all scopes.

## Ownership and evidence

The implementation worker owns the saved-view model, desktop editor/resolver
and native fixture. Root owns document admission, native reader qualification,
extraction, core fixtures, build registration, documentation, integration, Git
and packaging. A read-only advisor challenges data integrity and scene scope.
The independent canvas worker owns configured-null exact-move preview refusal
and its focused interaction fixture.

All source writers freeze before builds. Tests run offscreen with isolated
settings and noninteractive error handling. Verify two saved views of the same
wall/opening, complete scene hiding and recovery, global/local precedence,
history, native reopen, source deletion, legacy no-op and owner identity.
Inspect rendered canvas/sheet/PDF captures and compare source model and
measurement facts. Verify format downgrade refusal with a recomputed digest,
then perform the scoped commit, push and remote-ref check. A scoped pass is an
internal checkpoint, not unified production acceptance.
