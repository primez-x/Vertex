# Pinc importer foundation: 2026-10-04

This is an internal parser/worker checkpoint for PINC-011. There is no native
Pinc import command yet. The installed `vertex-20261004-room-repair` checkpoint
does not include this foundation. The production and compatibility requirements
remain open.

## Implemented

- Known `PincSketch` string version `4.2` and the inspected legacy `2.*`
  dialect parse into detached typed records. Geometry converts feet/downward Y
  to metres/upward Y. Curves preserve signed sagitta, including major arcs and
  the original small-curve thresholds. Symbol/text angles change sign, with
  valid orientations outside one turn retained.
- Source identities include page occurrence and collection. Exact legacy
  duplicate edges retain their original sources and visual wall associations;
  near-edge conflicts stay distinct. Assignment keys are evidence, not proof
  of a native face or appraisal eligibility.
- JSON duplicate keys, depth, nodes, strings, records, aggregate edges and graph
  pair work have hard limits. Assignment-reference expansion is charged before
  copying. Unknown content remains a diagnostic/source pointer; it is not
  promoted into native properties.
- A separate `VertexPincCandidate` JSON codec and `VPIW0001` binary framing
  leave the existing DXF/IFC protocol unchanged. Page frames contain original
  image bytes and existing validated `PSIR0002` raw RGBA pixels. Framing and
  aggregate source/pixel budgets are checked before allocating images.
- PNG/JPEG/BMP/TIFF decode only inside the worker. The TIFF lane checks the
  actual container GUID. The desktop copies validated pixels and owns retained
  source bytes; it does not decode compressed Pinc imagery. Unsupported or
  unreadable underlays produce an explicit diagnostic.
- Broker policy fixes arguments, time, memory, process count, output and offline
  controls. Only a controls-attested report can become a broker-accepted result;
  serialized content cannot claim attestation.

## Actual verification

Release build targets `pinc_project_import_tests`,
`pinc_import_candidate_codec_tests`, `pinc_import_worker_tests` and
`reference_import_tests` compiled successfully. Logs are retained under
`artifacts/pinc-import-20261004/`.

`parser-review-tests.log`: all four focused CTest checks passed. A later TIFF
container correction rebuilt successfully and `raster-format-tests.log` records
both affected checks passing. The unchanged parser and candidate checks retain
their earlier passing evidence.

Meaningful fixtures cover modern/legacy geometry and presentation, analytical
arc known answers, duplicated page IDs, malformed and over-budget input,
unmatched assignment evidence, multi-turn angles, reference expansion budgets,
legacy duplicate visual references, forged codec records and framing, broker
denial, pixel/source buffer ownership, unreadable imagery and PNG disguised as
TIFF. The existing reference check also verifies genuine TIFF decoding.

The initial parser test run failed because one-edge fixture construction made
a JSON object instead of an array. Explicit arrays corrected the fixture;
subsequent parser checks passed. Failed logs are retained. Independent review
identified the reference-budget, duplicate-reference and TIFF-container defects;
all three received fixes and affected passing checks.

Worker transport checks launch a hidden task-owned child directly and inject
broker reports to test acceptance/refusal. They do **not** establish actual
AppContainer controls for installed Pinc import. No supplied Pinc executable,
installer, HTML runtime or historical project was executed.

## Required continuation

Detached native admission, exact unique face correspondence, page/context
review, source retention, semantic symbol crosswalk, independent dynamic
name/calculation callouts, text alignment, underlay transforms, cancellation,
dirty-project guards and native Save As protection remain to implement and
qualify. Then verify actual installed sandbox import, editing, Undo/Redo,
save/reopen and output. Synthetic fixtures are not historical compatibility or
full PINC-011 certification.
