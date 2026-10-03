# Per-instance SVG component colors

This checkpoint fills the SVG-color authoring gap within APX-ANNO-003. It does
not establish production acceptance, full Apex parity or ANSI certification.

Double-click an SVG component for quick properties and choose **Colors…**.
Clear **Use library colors** to author its outline and primary surface. Apply
changes only that instance; reset restores its library colors. Cancel and
unchanged Apply create no history. Accepted edits are one Undo step and retain
the exact owner policy, opaque metadata, siblings, pinned artwork and geometry.
Manual tasks U349–U350 describe the workflow.

The documented white-outline-2 paint vocabulary preserves gradients, opacity,
glass, recesses and dark details. A safe streaming XML parser makes a derived
render copy; the original 322 catalog assets and pinned bytes stay unchanged.
The cache includes exact source identity and palette intent. Canvas and shared
sheet/PDF rendering use that same derivative. Unknown source profiles refuse
editing, visibly diagnose saved intent, and block incorrect output until reset.

Explicit palettes emit annotation state 7/native 25/extraction 23. Their
all-history qualification includes Undo and deleted owners. Absent palettes
retain old wire behavior. Recovery archives preserve source and intent, and
recomputed-digest downgrade attempts are refused without changing the file.

The baseline core fixture reproduced unsupported annotation version. After
implementation, the Release application and affected targets built. Ten focused
suites passed: annotation_catalog_tests, annotation_entity_codec_tests,
project_store_tests, project_exchange_tests, symbol_svg_palette_tests,
symbol_palette_desktop_tests, symbol_svg_desktop_tests,
output_view_appearance_desktop_tests, object_appearance_desktop_tests and
appraisal_details_panel_tests. The palette renderer checks all 322 assets,
protected paint roles, both cache orders, absent appearance, transparency,
screen and actual PDF pixels. The native fixture uses actual double-click
properties, controls, two distinct palettes of the same sofa, output, reset,
history, save/reopen, clipboard and invalid/stale/read-only refusal.

Independent review requested persisted-failure coverage and corrected docs.
The added native fixture saves and reopens a valid palette with an unsupported
source profile, confirms its diagnostic and edit/export refusal, preserves an
existing PDF and fingerprint sidecar, and recovers through palette reset.
Root inspected native dialog/canvas/PDF captures. All fixtures are offscreen
with isolated settings and noninteractive error handling. Evidence is retained
under artifacts/symbol-svg-palette-20261003, including historical failures.

An additional clipboard regression exposed annotation-only group motion being
refused by a configured exact-preview provider. The provider now prepares the
same validated candidate and asynchronous projection used for mixed motion.
Its refusal guard remains in place. The existing group fixture includes explicit
palette intent and observes the completed proposal before asserting atomic
cross-owner movement, sibling/metadata preservation and exact Undo. The
annotation-clipboard scenarios and connected_wall_canvas_tests pass. All six
affected desktop suites were rerun after that change; the five unchanged core
and renderer suites retain their passing evidence, for twelve distinct checks.

Full human artwork review, broad family/print qualification, clean-machine
offline acceptance, ANSI normative validation and Apex file/integration
compatibility remain open. Scoped technical checks do not prove those gates.
