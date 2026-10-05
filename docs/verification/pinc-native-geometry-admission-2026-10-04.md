# Pinc native geometry admission: 2026-10-04

This internal PINC-011 checkpoint converts validated detached Pinc geometry
into editable native measurement linework. It does not add an import command,
publish a project, or update the installed Desktop build.

## Implemented behavior

- Every source segment receives fresh native stroke, edge and vertex identities
  and a retained exact construction receipt. Straight lines and signed curves
  use the existing measurement model; source identifiers are provenance only.
- Each page has reviewed calculation and interior contexts on distinct empty
  layers. Pages can share a reviewed floor without merging their calculation
  graphs. A page name does not establish floor grade or physical wall depth.
- Modern assignments are matched through independently reconstructed source
  graph faces, complete analytical cycles and exact source intervals. Cached
  area and label anchors cannot choose a face. Shared long walls retain their
  partial intervals where a junction splits them.
- Contained outlines retain independent gross areas. Containment alone does
  not create a deduction, hole or ownership relationship. Conflicting owners
  and ambiguous source keys remain unresolved with diagnostics.
- Legacy assignments cover each ordered original source through contiguous
  exact intervals, including reversed aliases. An attached garage can split a
  house wall without losing the house assignment. A partition that changes the
  original footprint cannot inherit its assignment automatically.
- Preparation revalidates the private base and previews the proposed entity
  command without changing the active document. Aggregate graph, traversal,
  identity and correspondence budgets are bounded before expensive expansion.
  Native geometry is marked required. Fresh namespaces are capped at 101 ASCII
  characters to reserve the maximum suffix within the native identity limit.

## Actual verification

Release target `pinc_geometry_admission_tests` compiled successfully.
`artifacts/pinc-import-20261004/geometry-required-build.log` records exit 0;
`geometry-required-tests.log` records the focused `pinc_geometry_admission`
CTest check passing (1/1).

Fixtures cover detached preparation without base mutation, independent page
occurrences and reviewed layers, shared walls, partial long-wall intervals,
signed analytical arcs, exact legacy aliases in both directions, attached
house/garage boundaries, partition refusal, independent nested gross areas,
ambiguous arc-source keys, small open gaps, identity collisions and resource
boundaries. A typed imported-arc edit passes the Document command JSON codec,
Undo/Redo, retained-history validation and entity-command rehydration.

The initial native check stopped at a one-edge JSON fixture constructed as an
object rather than an array. Independent review then found a required-geometry
flag defect, legacy split-wall correspondence refusal and an inconsistent
namespace bound. All received corrections and relevant assertions before the
passing run. Earlier failed logs are preserved.

Independent review approved the corrected geometry checkpoint with no remaining
P1/P2 findings in that scope. Root reviewed the integrated API, CMake dependency
seam, final source and actual native result. The review does not approve the
unfinished import interface or claim full compatibility.

## Remaining import work

This API returns geometry and transient correspondence handles. Native area
authoring must rederive those handles against the private candidate. Source
retention, classifications, separate live name/calculation callouts, text
alignment, symbol mapping, visual wall associations, imagery, ordered views,
fidelity review, dirty-project guards, cancellation, atomic publication and
native Save As protection remain required. Installed sandbox, actual
save/reopen/output and representative historical-project qualification remain
open. Synthetic native command round trips are not file compatibility
certification.
