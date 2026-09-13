# Architectural workflow descriptor foundation

`architectural_workflow_contract.hpp` supplies detached immutable transaction and output requirement values for ARCH-EDIT-001 and ARCH-OUTPUT-001. `architectural_document_adapter.hpp` now translates those descriptors into one revision-fenced, atomic `Document` command, with preview, undo, and ProjectStore save/reopen coverage. These are still foundations, not completed product workflows.

Transactions preserve ordered create, select, property edit, transform, duplicate and delete intent. IDs are checked against a supplied initial ID snapshot, simulated in order and never reused during a transaction. Invalid enum values, missing targets, collisions, irrelevant payloads and nonfinite transforms fail before a descriptor is returned. The undo label declares one undoable transaction; the adapter delegates history and inverse state capture to `Document`. Selection denotes intent for one object; multi-selection policy belongs to the adapter.

Transforms applied to a canonical building entity are semantic operations. The
adapter scales dimensions and local openings, rotates horizontal positions and
orientation, and applies the translation to model coordinates before rebuilding
the validated object through the building codec. Beam endpoints and direction
vectors are transformed together. The old generic `transform` property is not
left beside changed geometry. Application-owned marks, material assignments,
quantity receipts, extensions, and other unrelated properties survive.

Wall duplication carries every opening whose `wall_id` names the source wall.
Cloned openings receive deterministic `duplicate-wall-id:original-opening-id`
identities and point only to the cloned wall. Deleting a wall removes its
hosted opening graph in the same atomic Document command; deleting another
entity does not remove unrelated openings.

The transaction descriptor keeps property payloads as transport-friendly text. The Document adapter decodes valid JSON scalar, object, and array text back to typed values while preserving non-JSON semantic strings such as `4m`. Type IDs and property keys have lexical validation, not a building-type/property registry. Transforms declare translation in metres, Z rotation in radians and a positive uniform scale; adapters must enforce type-specific legality, units, geometry validity and hosted relationships. Measurement boundaries are neither imported nor converted into architectural objects here.

Output contracts scope plans, elevations, sections, 3D views and schedules to known architectural IDs and sheets under one explicit model revision and issue revision. Each sheet must have requirements. A package can intentionally request only some output kinds; it does not certify a complete permit set. Outputs and identity sets serialize canonically; transaction operation order is retained. JSON export is a deterministic descriptor, with no import or persistence codec yet.

The Windows desktop integration fixture now exercises the contract against one
editable project: semantic walls, hosted openings, slabs, and building objects
are projected into plan/elevation/section views, scheduled, placed on sheets,
issued with revisions and callouts, exported to PDF/SVG/PNG, and saved and
reopened. This proves the development-host command and renderer path, while
remaining separate from production market fixtures and physical output review.

Still open: property/type schema enforcement; output revision freshness checks;
complete residential and light-commercial fixture provenance; physical printer
calibration and output fidelity; full 3D authoring/gizmos; and production
qualification. Architectural inspector property edits and the semantic object
transform dialog route through the adapter and preserve typed JSON values in
ordinary Document history. The adapter's semantic building transforms and wall
hosted-object policy are covered by atomic preview/history tests; generated
deliverable correctness remains a separate acceptance requirement.
