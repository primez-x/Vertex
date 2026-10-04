# Remove a boundary point

The selected-area context menu, Tools and command palette now expose Remove
point. The dialog shows the original and proposed boundary, removed point and
analytical before/after area and perimeter. Compatible same-circle arcs merge
with their combined signed sweep; other incident edges become a visibly
previewed straight chord. Invalid topology, unknown children, collapsed regions,
multiple selection, read-only ownership and stale context refuse changes.

The pure helper retains stable analytical children. Desktop commits follow the
existing fresh-child topology contract, with proven mappings for surviving
children. Reference review defaults those children to Keep and requires an
explicit decision for retired references. Automatic edge dimensions regenerate;
manual measurements retain their identity, presentation and semantic targets
through mapping. The boundary identity, appraisal facts and historical source
receipts remain on the ordinary typed redraw/history path. No format or command
schema relaxation was introduced.

Core checks cover rectangle/collinear/wrap removal, signed minor/major arc split
inverses, valid two-arc regions, incompatible arcs and invalid intersections.
Actual desktop fixtures cover Preview/Cancel/Apply, manual-reference decisions,
declared GLA recalculation from 48 to 24 m² with retained facts, multiple/stale
selection, second-open read-only ownership, exact Undo/Redo, editable save/reopen
history and restoration of a split semicircle. The ownership fixture releases
the first writer before reopening for editing; the first read-only open is
tested separately.

Final native verification and visual inspection are recorded under
`artifacts/boundary-point-removal-20261004`. The initial RED run reproduced the
missing geometry command. Failed compilation and fixture runs are retained,
including the Qt child lookup, appraisal workflow setup and second-open ownership
corrections. Root visual review found a provisional-size clipping problem in
the existing reference dialog; its read-only canvases now refit on show/resize
and use short edge identifiers, with a full-corner containment regression.
The final Release build and all seven affected checks pass: geometry operations,
boundary editing desktop, redraw reference desktop, boundary integrity, boundary
dimensions, project storage and project exchange. Root inspected both final
preview captures and reviewed the integrated source and interface behavior.

Physical-wall-derived outlines require a faithful physical source merge adapter
and refuse this standalone removal command. That remains a full-release gap.
This checkpoint does not certify ANSI normative validation, Apex native-file
compatibility, the complete architectural/assisted scope or production acceptance.
User checklist U423 remains unchecked. Stale documentation about qualified live
GLA previews and curved related-object editing was corrected from actual source
and existing regression coverage; historical qualification evidence was not
regenerated.
