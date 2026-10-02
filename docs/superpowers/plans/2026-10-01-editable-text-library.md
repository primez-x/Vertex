# Editable offline text library

Close U130/APX-ANNO-001's missing save/edit/reuse workflow. Keep named local
entries with category, multiline content and annotation style. Built-in room
entries can be copied; user entries can be saved, edited and deleted. Search
and category filtering serve selection. Insert uses a normal canvas placement
click; dragging pans and Escape cancels. Placed text is a complete independent
annotation instance, so later library edits/deletion cannot change a project.

Use a documented bounded version-1 local JSON file and atomic writes with
cooperating writer locks and changed-file refusal. Corrupt/future files stay
untouched. Existing built-in/free-text and drawing behavior remains available.
Annotation insertion preserves unrelated raw project records and metadata.
New library Insert labels use explicitly versioned model-plan anchors while
legacy labels retain their view-overlay coordinates. Verify rotated/reflected
placement, dragging, persistence and exclusion from other view kinds. Rotation
remains view-relative rather than flipping readable text with a reflected frame.

Worker owns new library codec/store/dialog/tests. Root owns MainWindow,
CMake, integration tests, docs, generators, builds, Git and delivery. Verify
actual Save/edit/Insert controls, two independent styled instances, pan/cancel,
history, save/reopen, output, malformed/external-change refusal and raw-state
preservation. Inspect the rendered dialog and canvas before delivery.

Implementation and the scoped verification above are complete. The actual
dialog, canvas and PDF were inspected, and named-plan placement and movement
pass for rotated and reflected frames. Delivery includes a new offline bundle
and the updated U130 user checklist.

The full production goal stays active. Independent
wall-dimension presentation, appraisal sheet ergonomics and Apex/device
compatibility still require their own implementation/evidence; this library
checkpoint is not a production release.
