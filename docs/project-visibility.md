# Floor and layer visibility

This implements the view-filter step of the project-organization contract.
Visibility does not change geometric coordinates, area membership, classifications,
quantities, document revisions or history. Both workspaces must render the same
explicit filter while calculating from the complete semantic document.

## Derived visibility

`ProjectViewFilter` contains hidden floor IDs and hidden layer IDs.
`visible_project_entities` resolves the organization once and returns stable
visible entity IDs. Hiding a floor masks its resolved layers and contents;
hiding one layer preserves the other layers on that floor. Hosted openings use
their wall's resolved placement. Stale filter IDs do not hide unrelated objects.
Unassigned, malformed or contradictory placement stays visible for inspection.
The filter never repairs or rewrites a document to make it fit the view.

The native viewport accepts the visible-ID set alongside the complete snapshot.
It hides presentations and removes them from picking while retaining their
validated geometry cache. Hidden invalid geometry still blocks image output.
Plan rendering and hit-testing must consume the same visible-ID set, while
geometry validation and calculation aggregation retain the complete snapshot.

## Workspace controls

The navigator exposes independent floor/layer checkboxes and a Show all action.
Clicking a checkbox retains the selected object and drawing destination. Arrow
keys select rows; Space toggles the current row's visibility. A checked layer
under a hidden floor explains its effective hidden state without changing its
own check state. The context label explains when the drawing destination is
hidden. Switching workspaces preserves the filter.
Creating or opening another project resets transient filters; failed opens
preserve the existing state. Saved workspace profiles must explicitly version and
restore filter state without treating visibility as calculation membership.

Draft printing/export identifies the filtered view when filters apply.
Production sheets will use their own explicit view definitions and qualified
output fingerprints. Transient filtering is not a substitute for those required
saved-workspace and output capabilities.

Every PDF/native-image request and each asynchronous print paint reads the
current document snapshot and refreshes derived geometry and validation before
output. Mutable/shared document callers can replace a head even with the same
document ID and revision, so those identifiers alone are not a freshness test.
Expensive pure geometry results remain cached by complete source properties.
Regressions cover direct and shared mutations, same-revision alternate heads,
and invalidation after a print preview has already opened.

Verification includes nested floor/layer masks, hosted openings, unresolved
placement, unchanged snapshots, actual native framebuffers and picking,
calculation totals, project-switch behavior, keyboard controls and scaled UI
captures. Core and native tests alone do not certify the entire workflow.

`scripts/test-visibility-workspace.ps1 -Configuration Release` runs the desktop
workflow and captures both workspaces plus a filtered PDF at normal and 150
percent scaling. The fixture hides a floor containing a nonzero calculated area
and proves full-document totals are unchanged in the same inspector context.
Native framebuffer/picking tests separately run at DPR 1, 1.5 and 2.
