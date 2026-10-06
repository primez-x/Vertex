# Pinc desktop import checkpoint: 2026-10-05

This implements the native Pinc import path. It is an internal Vertex checkpoint,
not full Pinc workflow parity, Apex compatibility certification or production
acceptance. The supplied installer and embedded HTML were inspected as data;
neither was executed or redistributed.

## User-visible behavior

Open Commands and choose **Import PincSketch project**. Import runs through the
protected worker and reviews fidelity notes and page-to-floor assignments before
creating an independent, unsaved native project. Cancel or invalid input leaves
the active project unchanged. Native Save As cannot overwrite the source `.pinc`.
The original source bytes are retained, unchanged and hashed, inside the project.

Pages have distinct calculation and interior layers, ordered native sheets and
restricted views. Multiple source pages may share a native floor. Page navigation
focuses the measurement canvas independently of user visibility settings and
printed views. Previous-page ghosts are screen references only. Explicit hidden
floors and layers remain hidden after page changes and in sheet output.

Straight and curved source geometry becomes editable native measurement
linework. Exactly equal source endpoints in the same page/lane retain native
coincidence joints; near-equal endpoints are not silently joined. Imported area
values are re-derived from current native geometry, classifications, real
deductions and factors, rather than copied from cached source totals. Imported
classification names do not supply appraisal eligibility facts or establish ANSI
compliance. Authored area names retain their capitalization and spacing.

Presentation preserves independent live name/value callouts, authored text
alignment and model height, colors, opacity, supported hatches and line patterns.
Symbols use pinned bundled SVG counterparts with source dimensions, rotation and
mirroring. Known artwork differences appear in the pre-import fidelity report.
Visual source-wall associations are retained as provenance; they are not physical
door/window hosting or wall cuts.

New native symbols and labels retain their page/layer context without replacing
the imported annotation carrier or its source records. Generated portrait sheets
reserve footer space. Automatic fitting measures painted geometry, SVGs, rotated
text and references, and includes later authored content without rewriting the
document. An explicit authored crop opts out of automatic fitting.

## Observed evidence

The final Release build succeeded. The 21 affected CTest entries finished with
20 passes and one expected `windows_import_worker` skip under the harness Job
Object. That worker and the real desktop import scenario both passed in the
separate immutable runtime outside the harness Job Object. The skip is not
counted as a pass.

The desktop scenario exercises actual import, review/progress Cancel, invalid
input, native save/reopen, source preservation, one-step import Undo/Redo,
connected editing, live quantity rendering, later native annotation authoring,
page selection, ghost/output isolation, explicit filters, malformed metadata
recovery, PNG and two-page PDF output. An 8-foot square recalculates from 64 to
72 square feet after extending one edge to 10 feet with endpoint coincidence;
the adjoining edge becomes slanted because no perpendicular constraint was
invented. PDF, native reopen and Undo/Redo preserve that result.

Additional real output cases include long left/right-aligned and rotated text,
distant live callouts, later distant labels, empty text, a blank page and a
decoded raster underlay. PDF pages and light/dark workspaces were captured for
visual inspection. The source fixtures are independently authored synthetic
inputs, not representative historical projects supplied by the user.

Evidence locations, excluded from the public source tree:

- `artifacts/pinc-import-20261005/twenty-first-build.{json,log}`.
- `artifacts/pinc-import-20261005/final-affected-tests.{json,log}`.
- `artifacts/import-worker-independent/release-e48bd4c5695d476fb280fd45ecc4340e/`:
  terminal process results, before/after hashes, worker and desktop logs, and
  `pinc-desktop-captures/` containing actual PDF/PNG output and the fidelity review.

Fourteen project-package Python checks pass, and the deterministic SVG generator
check validates 345 visible SVG entries. The 80 source symbol names all have
unique declared counterpart mappings; this does not certify equal artwork,
default sizing, hosting or editing quality for every item.

Source/runtime SHA-256 at this build:

| Input | SHA-256 |
| --- | --- |
| `src/desktop/main_window.cpp` | `e1adb6f5bf9d3358ca49df198581420367b332138abb52bb8766a86622174901` |
| `src/desktop/pinc_project_admission.cpp` | `9050c9bdf8ea3df1d379f8cff6c022f89ba389803b50f7a3c717de17982a77ab` |
| `vertex.exe` | `ce0876064858afde34036f62713f2716584e514812990e2797d50dde08ab3249` |
| `vertex-import-worker.exe` | `57eae405bb327c9f69fad2987b3c7f730b3ebb1a821ffeb672036b0bf59ef906` |

## Delivery and remaining scope

Installed-bundle qualification is recorded after staging. The source build and
protected import fixture alone do not prove a clean-machine installation or the
published installed application's Pinc workflow.

Representative historical `.pinc` migration, paired verification of all 134
inventoried operations, complete symbol/default-size/hosting comparisons and
large connected-chain responsiveness remain open. Automatic room correspondence,
multi-room repair, physical-room dimensions, batch presentation, fast jump/offset
and pen-up workflows, tentative-curve shortcuts and inline reusable labels also
remain in scope. The original full architectural, appraisal, Apex compatibility,
device, recovery, output and production requirements are unchanged. Human
checklist outcomes remain **Not tested** until observed by the user.
