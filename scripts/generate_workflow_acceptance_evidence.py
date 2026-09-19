"""Write hash-bound acceptance evidence for implemented architecture workflows."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import pathlib
from typing import Any


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent


WORKFLOW_RULES: dict[str, dict[str, Any]] = {
    "ARCH-MOD-009": {
        "acceptance": (
            "A room can relate to, follow, or remain independent from a measurement "
            "boundary without silently conflating classifications or geometry."
        ),
        "sources": (
            "include/sketch/room_relationships.hpp",
            "src/core/room_relationships.cpp",
            "include/sketch/room_relationship_geometry.hpp",
            "src/core/room_relationship_geometry.cpp",
            "include/sketch/room_relationship_geometry_commit.hpp",
            "src/core/room_relationship_geometry_commit.cpp",
            "tests/room_relationships_tests.cpp",
            "tests/room_relationship_geometry_tests.cpp",
            "tests/room_relationship_geometry_commit_tests.cpp",
            "docs/room-relationships.md",
            "src/desktop/main_window.cpp",
            "tests/desktop_smoke.cpp",
            "docs/desktop-workflow.md",
        ),
        "anchors": (
            "RoomRelationshipSnapshot",
            "room_boundary",
            "appraisal_measurement_boundary",
            "retarget",
            "propagation",
            "independent",
            "undo",
        ),
        "tests": (
            "room_relationships",
            "room_relationship_geometry",
            "room_relationship_geometry_commit",
            "desktop_workflow",
        ),
        "qualification_boundary": (
            "This evidence covers deterministic relationship semantics, controlled retargeting, "
            "geometry propagation, and the Windows workflow on the development host. It does "
            "not certify imported Apex files, physical devices, printing, or the unified release gate."
        ),
    },
    "ARCH-MOD-011": {
        "acceptance": (
            "Alternative fixtures switch, compare, save, and print without contaminating one "
            "another or changing the shared existing state."
        ),
        "sources": (
            "include/sketch/model_phases.hpp",
            "src/core/model_phases.cpp",
            "src/desktop/main_window.cpp",
            "tests/model_phases_tests.cpp",
            "tests/desktop_smoke.cpp",
            "docs/phases-alternatives.md",
        ),
        "anchors": (
            "ModelPhases",
            "with_active",
            "compare",
            "remodelingComparisonList",
            "exportDraftPdf",
            "save and reopen",
        ),
        "tests": (
            "model_phases",
            "desktop_workflow",
        ),
        "qualification_boundary": (
            "This evidence covers the shared model phase value, comparison controls, coordinated "
            "plan/elevation/section output, undo/redo, and save/reopen on the development host. "
            "It does not certify imported Apex files, physical printers, or the unified release gate."
        ),
    },
    "CORE-DOC-009": {
        "acceptance": (
            "The CLI validates a good fixture, rejects malformed or incomplete data with "
            "actionable diagnostics, extracts assets, and produces a migration report."
        ),
        "sources": (
            "src/cli/main.cpp",
            "tests/test_cli.py",
            "docs/project-format.md",
        ),
        "anchors": (
            "inspect",
            "validate",
            "extract",
            "migrate",
            "source_preserved",
            "malformed",
        ),
        "tests": ("project_cli",),
        "qualification_boundary": (
            "This evidence covers the local inspect, validate, extract, and migrate CLI fixture "
            "including malformed input and source preservation. It does not certify Apex/native "
            "migration fidelity or a production release."
        ),
    },
    "APX-KEY-004": {
        "acceptance": (
            "Each point-jump, automatic-closure, and bay-window operation is observable in "
            "command history, produces the expected topology, and is undoable."
        ),
        "sources": (
            "include/sketch/geometry_operations.hpp",
            "src/core/geometry_operations.cpp",
            "tests/geometry_operations_tests.cpp",
            "docs/geometry-operations.md",
            "include/sketch/desktop/main_window.hpp",
            "src/desktop/main_window.cpp",
            "tests/desktop_smoke.cpp",
            "docs/desktop-workflow.md",
        ),
        "anchors": (
            "jump_to_boundary_vertex",
            "automatically_close_boundary",
            "complete_bay_window",
            "Auto close boundary",
            "Complete bay window",
            "undo",
            "redo",
        ),
        "tests": ("geometry_operations", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers the deterministic geometry operations and Windows workflow "
            "history/undo behavior. Physical Apex key mapping, native Apex fixtures, and the "
            "unified production gate remain open."
        ),
    },
    "REC-004": {
        "acceptance": (
            "Concurrent-open and external-change fixtures prevent silent overwrite and make the "
            "selected read-only or independent-copy path explicit."
        ),
        "sources": (
            "include/sketch/project_store.hpp",
            "src/core/project_store.cpp",
            "include/sketch/project_ownership.hpp",
            "src/core/project_ownership.cpp",
            "tests/project_ownership_tests.cpp",
            "tests/project_ownership_process_tests.cpp",
            "tests/desktop_smoke.cpp",
            "docs/project-format.md",
        ),
        "anchors": (
            "ProjectOwnershipSession",
            "external_change",
            "read-only",
            "case-insensitive",
            "second_open_is_read_only",
            "saveProject",
        ),
        "tests": ("project_ownership", "project_ownership_process", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers local and cross-process ownership, Windows path identity, "
            "external-change save blocking, and the desktop read-only second-open behavior. "
            "Clean-machine lifetime and hostile-writer qualification remain open."
        ),
    },
}


# These deterministic slices already have complete Debug/Release fixtures.  They
# are deliberately separate from the Apex/native and clean-machine gates: a
# local pass proves the implemented behavior, while the qualification boundary
# keeps external compatibility claims visible.
WORKFLOW_RULES.update({
    "OPS-PERF-002": {
        "acceptance": (
            "The owner-independent regeneration boundary runs derived work off the owner thread, "
            "cancels queued and running operations, discards cancelled receipts, captures failures, "
            "and preserves the valid source revision until the owner accepts a matching result."
        ),
        "sources": (
            "include/sketch/workspace_regeneration_queue.hpp",
            "src/core/workspace_regeneration_queue.cpp",
            "tests/regeneration_queue_tests.cpp",
            "tests/desktop_performance_tests.cpp",
            "docs/workspace-regeneration-queue.md",
            "docs/performance-benchmark.md",
        ),
        "anchors": (
            "WorkspaceRegenerationQueue",
            "RegenerationCancellationToken",
            "cancel",
            "source revision",
            "FIFO",
            "shutdown(false)",
        ),
        "tests": ("workspace_regeneration_queue", "desktop_performance"),
        "qualification_boundary": (
            "This evidence covers the deterministic queue/cancellation implementation on the "
            "development host, including bounded real canvas input/edit/paint capture. Integrated "
            "long-regeneration cancellation, native 3D and sheet presentation, reference hardware "
            "latency, representative workloads, and final production qualification remain open."
        ),
    },
})


WORKFLOW_RULES.update({
    "CORE-DOC-003": {
        "acceptance": (
            "Interleaved edits during save produce a file containing exactly the reported saved "
            "revision; a failed replacement leaves the previous valid file recoverable."
        ),
        "sources": (
            "include/sketch/project_store.hpp",
            "src/core/project_store.cpp",
            "tests/project_store_tests.cpp",
            "docs/project-format.md",
        ),
        "anchors": (
            "DocumentSnapshot",
            "expected_destination_sha256",
            "SaveFaultStage",
            "before_publish",
            "saved_revision",
            "FlushFileBuffers",
        ),
        "tests": ("project_storage",),
        "qualification_boundary": (
            "This evidence covers immutable snapshot saves, compare-and-swap publication, and "
            "pre-publication rollback on the development host. Clean-machine storage faults and "
            "the unified production gate remain open."
        ),
    },
    "CORE-DOC-004": {
        "acceptance": (
            "Changing a fingerprint input either regenerates output or produces a visible "
            "stale-output block; matching inputs reproduce the same output identity."
        ),
        "sources": (
            "include/sketch/output_fingerprint.hpp",
            "src/core/output_fingerprint.cpp",
            "tests/output_fingerprint_tests.cpp",
            "src/desktop/main_window.cpp",
            "tests/desktop_smoke.cpp",
            "docs/output-fingerprint.md",
        ),
        "anchors": (
            "OutputFingerprintInputs",
            "make_output_fingerprint",
            "check_output_fingerprint_current",
            "digest_sha256",
            "stale",
            "exportDraftPdf",
        ),
        "tests": ("output_fingerprint", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers deterministic fingerprint construction, dependency currentness, "
            "and desktop output binding on the development host. Physical output calibration and "
            "production authority qualification remain open."
        ),
    },
    "CORE-DOC-005": {
        "acceptance": (
            "A compound edit either commits as one revision with one undo entry or leaves the "
            "previous revision unchanged; stale worker results cannot overwrite newer edits."
        ),
        "sources": (
            "include/sketch/document.hpp",
            "src/core/document.cpp",
            "include/sketch/workspace_save_queue.hpp",
            "src/core/workspace_save_queue.cpp",
            "src/core/project_workspace_preview_adapters.cpp",
            "tests/project_workspace_preview_adapters_tests.cpp",
            "tests/document_tests.cpp",
            "docs/project-format.md",
        ),
        "anchors": (
            "command_to_json",
            "command_from_json",
            "ProjectWorkspace",
            "prepare",
            "undo",
            "redo",
            "stale",
        ),
        "tests": ("project_workspace_preview_adapters", "document_commands", "workspace_document_history"),
        "qualification_boundary": (
            "This evidence covers typed command envelopes, immutable preparation, atomic commits, "
            "revision fences, and undo/redo fixtures. Full production recovery and external worker "
            "qualification remain open."
        ),
    },
    "CORE-DOC-006": {
        "acceptance": (
            "Fixtures retain entered dimensions and calculate within the documented tolerance after "
            "display-format changes, save/reopen, and unit conversion."
        ),
        "sources": (
            "src/core/quantity.cpp",
            "tests/quantity_tests.cpp",
            "src/calculations/calculations.cpp",
            "tests/calculation_tests.cpp",
            "tests/project_store_tests.cpp",
            "docs/calculations.md",
        ),
        "anchors": (
            "ExactRational",
            "parse_quantity",
            "original_expression",
            "exact_metres",
            "round",
            "tolerance",
        ),
        "tests": ("quantity", "calculations", "project_storage"),
        "qualification_boundary": (
            "This evidence covers exact quantity parsing, unit conversion, display round trips, and "
            "snapshot persistence in the Debug/Release matrices. Hardware/input parity and the "
            "production gate remain open."
        ),
    },
    "CORE-DOC-008": {
        "acceptance": (
            "Each schema migration preserves the source fixture, reports unsupported content, and "
            "can be validated or rolled back without data loss."
        ),
        "sources": (
            "include/sketch/project_store.hpp",
            "src/core/project_store.cpp",
            "tests/project_store_tests.cpp",
            "docs/project-format.md",
        ),
        "anchors": (
            "migration",
            "unknown",
            "read-only",
            "future",
            "rollback",
            "opaque",
        ),
        "tests": ("project_storage", "project_archive", "project_exchange"),
        "qualification_boundary": (
            "This evidence covers copy-based version markers, opaque preservation, unsupported "
            "required content, and rollback fixtures. The complete migration matrix and production "
            "release qualification remain open."
        ),
    },
    "GEO-BASE-001": {
        "acceptance": (
            "Independent analytical and high-precision fixtures agree within the documented area "
            "and coordinate tolerances after save/reopen."
        ),
        "sources": (
            "include/sketch/geometry.hpp",
            "src/core/geometry.cpp",
            "tests/geometry_tests.cpp",
            "tests/architecture_tests.cpp",
            "tests/room_relationship_geometry_tests.cpp",
            "docs/geometry-operations.md",
        ),
        "anchors": (
            "Segment",
            "arc_from_chord_angle",
            "signed_area",
            "winding",
            "tangent",
            "holes",
            "validate_boundary",
        ),
        "tests": ("geometry", "architecture", "room_relationship_geometry", "slab_semantics"),
        "qualification_boundary": (
            "This evidence covers analytical lines, arcs, holes, winding, tangency, topology, and "
            "the named Debug/Release fixtures. Independent reference parity, native Apex projects, "
            "and production qualification remain open."
        ),
    },
    "GEO-BASE-002": {
        "acceptance": (
            "Each curve input mode produces the expected analytical arc, preserves its defining "
            "parameters, and edits without converting to a faceted stroke."
        ),
        "sources": (
            "include/sketch/geometry.hpp",
            "src/core/geometry.cpp",
            "tests/curve_construction_tests.cpp",
            "tests/boundary_authoring_session_tests.cpp",
            "src/desktop/main_window.cpp",
            "tests/desktop_smoke.cpp",
            "docs/boundary-authoring.md",
        ),
        "anchors": (
            "arc_from_chord_arc_length",
            "arc_from_start_tangent",
            "add_arc_chord_height",
            "add_arc_chord_angle",
            "sweep_radians",
            "arc_length",
        ),
        "tests": ("curve_construction", "boundary_authoring_session", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers true chord, arc-length, height, angle, and tangent construction "
            "plus desktop editing on the development host. Boundary-join fidelity and complete Apex "
            "parity remain open."
        ),
    },
    "GEO-BASE-003": {
        "acceptance": (
            "Fixtures entered in imperial and metric units reproduce expected endpoints and headings "
            "with documented tolerance."
        ),
        "sources": (
            "include/sketch/boundary_authoring_session.hpp",
            "src/core/boundary_authoring_session.cpp",
            "tests/boundary_authoring_session_tests.cpp",
            "tests/boundary_input_dialog_tests.cpp",
            "docs/boundary-authoring.md",
        ),
        "anchors": (
            "add_line_rise_run",
            "add_line_relative_turn",
            "parse_angle",
            "exact_metres",
            "Define First",
            "Draw First",
        ),
        "tests": ("boundary_authoring_session", "boundary_input_dialog", "boundary_workflow"),
        "qualification_boundary": (
            "This evidence covers Draw First and Define First construction receipts, rise/run, "
            "relative turns, explicit angles, and unit-aware precision entry. Physical keyboard "
            "mapping and production qualification remain open."
        ),
    },
    "GEO-BASE-004": {
        "acceptance": (
            "Keyboard and pointer fixtures show each behavior, expose the resulting constraint or "
            "closure, and reopen without drift."
        ),
        "sources": (
            "include/sketch/geometry_operations.hpp",
            "src/core/geometry_operations.cpp",
            "tests/geometry_operations_tests.cpp",
            "tests/boundary_canvas_tests.cpp",
            "src/desktop/plan_canvas.cpp",
            "src/desktop/main_window.cpp",
            "tests/desktop_smoke.cpp",
            "docs/geometry-operations.md",
        ),
        "anchors": (
            "jump_to_boundary_vertex",
            "automatically_close_boundary",
            "complete_bay_window",
            "snapped",
            "setSnapEnabled",
            "Auto Close",
            "Complete bay window",
            "save/reopen",
        ),
        "tests": ("geometry_operations", "boundary_canvas", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers analytical alignment/snap, point jumping, exact closure, bay-window "
            "completion, pointer rendering, and desktop history. Physical Apex key mapping and native "
            "compatibility fixtures remain open."
        ),
    },
    "GEO-CON-001": {
        "acceptance": (
            "Constraint fixtures report intended degrees of freedom and preserve each relation through "
            "supported edits and save/reopen."
        ),
        "sources": (
            "include/sketch/constraint_entity.hpp",
            "include/sketch/constraints.hpp",
            "src/core/constraint_entity.cpp",
            "src/core/constraint_integrity.cpp",
            "src/core/constraints.cpp",
            "include/sketch/constraint_authoring.hpp",
            "src/core/constraint_authoring.cpp",
            "src/desktop/constraint_dialog.cpp",
            "tests/constraint_entity_tests.cpp",
            "tests/constraints_tests.cpp",
            "tests/constraint_integrity_tests.cpp",
            "tests/constraint_authoring_tests.cpp",
            "tests/constraint_dialog_tests.cpp",
            "docs/constraint-authoring.md",
        ),
        "anchors": (
            "parallel",
            "perpendicular",
            "horizontal",
            "vertical",
            "coincident",
            "fixed_length",
            "degrees_of_freedom",
            "constraint_integrity",
        ),
        "tests": ("constraints", "constraint_entities", "constraint_integrity", "constraint_authoring", "constraint_dialog"),
        "qualification_boundary": (
            "This evidence covers persistent straight-wall relation authoring, solver previews, "
            "degrees-of-freedom checks, and save/reopen behavior in Debug and Release. Boundary-vertex "
            "and curve relations plus production qualification remain open."
        ),
    },
    "GEO-CON-002": {
        "acceptance": (
            "A 12-foot-to-14-foot edit previews both supported anchoring choices, shows dependent "
            "movement, and commits only after selection."
        ),
        "sources": (
            "include/sketch/constraint_authoring.hpp",
            "src/core/constraint_authoring.cpp",
            "src/desktop/constraint_dialog.cpp",
            "tests/constraint_authoring_tests.cpp",
            "tests/constraint_dialog_tests.cpp",
            "docs/constraint-authoring.md",
        ),
        "anchors": (
            "constraintAnchor",
            "Allow connected walls to move",
            "preview",
            "Apply",
            "source_snapshot_digest",
            "invalidate",
        ),
        "tests": ("constraint_authoring", "constraint_dialog"),
        "qualification_boundary": (
            "This evidence covers source-bound length previews, explicit endpoint anchoring, connected "
            "movement selection, invalidation, and atomic Apply. Boundary/curve editing and production "
            "qualification remain open."
        ),
    },
    "GEO-CON-006": {
        "acceptance": (
            "Changing a level elevation previews and applies expected dependent changes across plans, "
            "sections, and 3D without cycles."
        ),
        "sources": (
            "include/sketch/vertical_levels.hpp",
            "include/sketch/vertical_level_document_adapter.hpp",
            "src/core/vertical_levels.cpp",
            "src/core/vertical_level_document_adapter.cpp",
            "src/desktop/main_window.cpp",
            "tests/vertical_levels_tests.cpp",
            "tests/document_tests.cpp",
            "tests/desktop_smoke.cpp",
            "docs/vertical-levels.md",
        ),
        "anchors": (
            "VerticalLevel",
            "floor_to_floor",
            "elevation",
            "freeze",
            "disconnect",
            "total_rise_m",
            "preview",
            "apply",
        ),
        "tests": ("vertical_levels", "document_commands", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers the validated vertical dependency graph, level edits, connected "
            "stair rise updates, and plan/elevation/section/native-3D projections in Debug and Release. "
            "Site/terrain coordination and production qualification remain open."
        ),
    },
    "APX-KEY-002": {
        "acceptance": (
            "Equivalent imperial and metric entries create equivalent analytical geometry and preserve "
            "the original entered quantity and unit."
        ),
        "sources": (
            "src/core/quantity.cpp",
            "tests/quantity_tests.cpp",
            "docs/calculations.md",
        ),
        "anchors": (
            "fraction",
            "feet",
            "metres",
            "original_expression",
            "exact_metres",
            "display",
        ),
        "tests": ("quantity",),
        "qualification_boundary": (
            "This evidence covers exact fractional, feet-and-inches, metric, and display-round-trip "
            "behavior. Physical input devices, Apex native fixtures, and the production gate remain open."
        ),
    },
    "APX-KEY-003": {
        "acceptance": (
            "Shortcut fixtures pass under the Apex preset, conflicts are reported, and user-customized "
            "bindings persist in a workspace."
        ),
        "sources": (
            "include/sketch/geometry_operations.hpp",
            "src/core/geometry_operations.cpp",
            "src/desktop/main_window.cpp",
            "tests/geometry_operations_tests.cpp",
            "tests/desktop_smoke.cpp",
            "docs/geometry-operations.md",
            "docs/workspace-accessibility.md",
        ),
        "anchors": (
            "apex_operation_preset",
            "shortcut_conflicts",
            "keyboardShortcut",
            "Apex",
            "persist",
            "F4",
            "Ctrl+Shift+D",
        ),
        "tests": ("geometry_operations", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers deterministic Apex-compatible command IDs, conflict reporting, and "
            "local shortcut persistence. Physical key mapping, keyboard-only parity, and production "
            "qualification remain open."
        ),
    },
    "APX-EDIT-001": {
        "acceptance": (
            "Copy, cut, paste, multi-select, and selection filtering preserve semantic links and "
            "produce undoable commands."
        ),
        "sources": (
            "include/sketch/desktop/main_window.hpp",
            "src/desktop/main_window.cpp",
            "src/desktop/plan_canvas.hpp",
            "src/desktop/plan_canvas.cpp",
            "tests/desktop_smoke.cpp",
            "docs/desktop-workflow.md",
        ),
        "anchors": (
            "copy",
            "cut",
            "paste",
            "selected",
            "remap",
            "undo",
            "redo",
        ),
        "tests": ("desktop_workflow",),
        "qualification_boundary": (
            "This evidence covers the supported geometry graph clipboard, ordered multi-selection, "
            "semantic link remapping, and atomic undo/redo on the development host. Broader annotation "
            "and object policies plus production qualification remain open."
        ),
    },
    "APX-EDIT-002": {
        "acceptance": (
            "Transform fixtures preserve analytical dimensions, typed links, and expected handedness, "
            "and can be undone exactly."
        ),
        "sources": (
            "include/sketch/geometry_operations.hpp",
            "src/core/geometry_operations.cpp",
            "src/desktop/main_window.cpp",
            "tests/geometry_operations_tests.cpp",
            "tests/desktop_smoke.cpp",
            "docs/geometry-operations.md",
        ),
        "anchors": (
            "rotate_boundary",
            "flip_boundary",
            "PlanarTransform",
            "pivot",
            "flip_horizontal",
            "undo",
        ),
        "tests": ("geometry_operations", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers analytical rotation, reflection, pivot and offset semantics with "
            "typed desktop history. Reference/annotation breadth, linked propagation, and production "
            "qualification remain open."
        ),
    },
    "APX-EDIT-003": {
        "acceptance": (
            "Insertion and clone fixtures preserve valid topology, report any new degrees of freedom, "
            "and reopen identically."
        ),
        "sources": (
            "include/sketch/geometry_operations.hpp",
            "src/core/geometry_operations.cpp",
            "src/desktop/main_window.cpp",
            "tests/geometry_operations_tests.cpp",
            "tests/desktop_smoke.cpp",
            "docs/geometry-operations.md",
        ),
        "anchors": (
            "insert_boundary_vertex",
            "clone_boundary",
            "fresh",
            "stable",
            "topology",
            "undo",
        ),
        "tests": ("geometry_operations", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers validated boundary insertion, explicit-ID cloning, topology identity, "
            "and desktop save/reopen/undo. Wall/object insertion and broader constraint reporting remain "
            "open."
        ),
    },
    "APX-EDIT-005": {
        "acceptance": (
            "A mixed command sequence returns byte-equivalent semantic state after undo-all and redo-all, "
            "including derived recalculation state."
        ),
        "sources": (
            "include/sketch/document.hpp",
            "src/core/document.cpp",
            "src/desktop/main_window.cpp",
            "tests/document_tests.cpp",
            "tests/desktop_smoke.cpp",
            "tests/project_store_tests.cpp",
            "docs/desktop-workflow.md",
        ),
        "anchors": (
            "undo",
            "redo",
            "classification",
            "calibration",
            "compound",
            "revision",
        ),
        "tests": ("document_commands", "project_storage", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers typed history for classification, calibration, geometry, annotation, "
            "architectural, compound, and persisted edits in Debug and Release. Complete mixed-command "
            "crash-recovery replay and production qualification remain open."
        ),
    },
    "APX-AREA-001": {
        "acceptance": (
            "Nested, adjacent, overlapping, and disconnected area fixtures produce independently "
            "inspectable totals and deductions."
        ),
        "sources": (
            "include/sketch/geometry_operations.hpp",
            "src/core/geometry_operations.cpp",
            "src/calculations/calculations.cpp",
            "src/desktop/main_window.cpp",
            "tests/geometry_operations_tests.cpp",
            "tests/calculation_tests.cpp",
            "tests/desktop_smoke.cpp",
            "docs/calculations.md",
        ),
        "anchors": (
            "detect_closed_boundaries",
            "calculate_areas",
            "deduct",
            "holes",
            "overlap",
            "aggregate",
        ),
        "tests": ("geometry_operations", "calculations", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers analytical face detection, grouping, holes, overlap rejection, "
            "deductions, and the desktop calculation path. Same-floor production fixtures and complete "
            "Apex workflows remain open."
        ),
    },
    "APX-AREA-002": {
        "acceptance": (
            "Changing classification or factor produces a visible, attributed calculation revision "
            "without altering boundary geometry."
        ),
        "sources": (
            "include/sketch/calculations.hpp",
            "src/calculations/calculations.cpp",
            "src/desktop/main_window.cpp",
            "tests/calculation_tests.cpp",
            "tests/desktop_smoke.cpp",
            "docs/calculations.md",
        ),
        "anchors": (
            "CalculationProfile",
            "factor",
            "base_area",
            "net_area",
            "classification",
            "version",
        ),
        "tests": ("calculations", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers versioned profiles, classifications, rational factors, base/net area, "
            "and attributed desktop revisions. Apex standards fixtures and production qualification remain open."
        ),
    },
    "APX-AREA-003": {
        "acceptance": (
            "Reference fixtures match independently calculated perimeter and totals within the documented "
            "tolerance, with rounding shown separately."
        ),
        "sources": (
            "src/calculations/calculations.cpp",
            "tests/calculation_tests.cpp",
            "src/desktop/main_window.cpp",
            "tests/desktop_smoke.cpp",
            "docs/calculations.md",
        ),
        "anchors": (
            "perimeter",
            "living",
            "building",
            "round",
            "unrounded",
        ),
        "tests": ("calculations", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers unrounded perimeter and living/building aggregates with separate "
            "display rounding. Production output parity and external standard certification remain open."
        ),
    },
    "APX-AREA-004": {
        "acceptance": (
            "The calculation inspector identifies every contributing object and applies the same-type "
            "Auto-Subtract rule from the compatibility fixture, with no hidden deduction."
        ),
        "sources": (
            "src/calculations/calculations.cpp",
            "src/desktop/main_window.cpp",
            "tests/calculation_tests.cpp",
            "tests/desktop_smoke.cpp",
            "docs/calculations.md",
        ),
        "anchors": (
            "deduction",
            "provenance",
            "rounding_delta",
            "Auto-Subtract",
            "deduction_ids",
            "marginal",
        ),
        "tests": ("calculations", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers inspectable deduction contributions, profile provenance, factors, "
            "rounding deltas, and contained-boundary handling. Documented same-type Apex compatibility "
            "fixtures and production certification remain open."
        ),
    },
    "CORE-SCOPE-002": {
        "acceptance": (
            "Measurement and Architectural workspaces expose distinct task surfaces while editing "
            "one shared project and document model."
        ),
        "sources": (
            "src/desktop/main_window.cpp",
            "tests/desktop_smoke.cpp",
            "docs/workspace-ui.md",
        ),
        "anchors": (
            "Measurement",
            "Architectural",
            "shared",
            "workspace",
            "overview",
            "grid",
            "snap",
        ),
        "tests": ("desktop_workflow", "workspace_navigation"),
        "qualification_boundary": (
            "This evidence covers the two workspace surfaces, shared document binding, and local "
            "navigation controls on the development host. Clean-machine input and production UI "
            "qualification remain open."
        ),
    },
    "CORE-DOC-001": {
        "acceptance": (
            "A project retains stable document revisions, named revisions, sheets, alternatives, "
            "schedules, and export metadata across the desktop workflow."
        ),
        "sources": (
            "src/core/document.cpp",
            "src/desktop/main_window.cpp",
            "include/sketch/desktop/main_window.hpp",
            "tests/desktop_smoke.cpp",
            "docs/desktop-workflow.md",
        ),
        "anchors": (
            "Document",
            "revision",
            "stable",
            "NameRevision",
            "schedule",
            "sheet",
            "alternative",
            "export",
        ),
        "tests": (
            "desktop_workflow",
            "architectural_workflow_contract",
            "architectural_schedule",
            "project_resource_desktop",
        ),
        "qualification_boundary": (
            "This evidence covers shared-document revision and presentation resources in Debug and "
            "Release. Native Apex project compatibility, physical output, and production qualification "
            "remain open."
        ),
    },
    "CORE-DOC-002": {
        "acceptance": (
            "Typed relationships keep measurement, room, and wall references distinct, support "
            "freeze/disconnect and controlled propagation, and reject ambiguity or cycles."
        ),
        "sources": (
            "include/sketch/typed_relationships.hpp",
            "src/core/typed_relationships.cpp",
            "tests/typed_relationships_tests.cpp",
            "docs/typed-relationships.md",
            "include/sketch/room_relationship_geometry.hpp",
            "src/core/room_relationship_geometry.cpp",
            "tests/room_relationship_geometry_tests.cpp",
            "include/sketch/room_relationship_geometry_commit.hpp",
            "src/core/room_relationship_geometry_commit.cpp",
            "tests/room_relationship_geometry_commit_tests.cpp",
            "docs/room-relationships.md",
        ),
        "anchors": (
            "wall_derived",
            "room_boundary",
            "appraisal_measurement_boundary",
            "independent",
            "freeze",
            "disconnect",
            "cycle",
            "ambiguous",
        ),
        "tests": (
            "typed_relationships",
            "room_relationships",
            "room_relationship_geometry",
            "room_relationship_geometry_commit",
        ),
        "qualification_boundary": (
            "This evidence covers typed relationship graph validation, controlled propagation, and "
            "independence semantics. Native Apex relationship behavior and production qualification "
            "remain open."
        ),
    },
    "CORE-DOC-007": {
        "acceptance": (
            "Architectural documents persist native solid semantics for slabs, floors, ceilings, and "
            "foundations and expose consistent projections and schedules."
        ),
        "sources": (
            "include/sketch/architecture.hpp",
            "src/architecture/architecture.cpp",
            "src/architecture/document_solid.cpp",
            "src/core/document.cpp",
            "src/core/document_schedule_adapter.cpp",
            "src/desktop/main_window.cpp",
            "tests/architecture_tests.cpp",
            "tests/wall_join_tests.cpp",
            "tests/architectural_schedule_tests.cpp",
            "tests/document_tests.cpp",
            "tests/desktop_smoke.cpp",
            "docs/building-objects.md",
        ),
        "anchors": (
            "document_solid",
            "slab",
            "floor",
            "ceiling",
            "foundation",
            "projection",
            "native",
            "schedule",
        ),
        "tests": (
            "architecture",
            "slab_semantics",
            "architectural_schedule",
            "document_commands",
            "desktop_workflow",
        ),
        "qualification_boundary": (
            "This evidence covers persisted architectural solids, horizontal element kinds, native "
            "projections, and schedule rows. External CAD/native compatibility and production "
            "qualification remain open."
        ),
    },
    "CORE-DOC-010": {
        "acceptance": (
            "Integration adapters declare versioned capabilities, provenance, and licensing metadata "
            "and resolve deterministically or fail closed."
        ),
        "sources": (
            "include/sketch/integration_adapter.hpp",
            "src/core/integration_adapter.cpp",
            "tests/integration_adapter_tests.cpp",
            "docs/integration-adapters.md",
        ),
        "anchors": (
            "IntegrationAdapterDefinition",
            "IntegrationAdapterRegistry",
            "capability",
            "api_version",
            "format_version",
            "provenance",
            "license",
            "resolve",
        ),
        "tests": ("integration_adapter",),
        "qualification_boundary": (
            "This evidence covers deterministic local adapter metadata and capability resolution. "
            "Apex/native integrations, legal review, and production qualification remain open."
        ),
    },
    "COMP-GEO-001": {
        "acceptance": (
            "Architectural slabs and building objects retain analytical solids with holes, native "
            "projections, and schedule quantities derived from one canonical geometry."
        ),
        "sources": (
            "include/sketch/slab_semantics.hpp",
            "src/core/slab_semantics.cpp",
            "src/core/document.cpp",
            "src/architecture/architecture.cpp",
            "src/architecture/document_solid.cpp",
            "src/architecture/architectural_schedule.cpp",
            "src/desktop/main_window.cpp",
            "tests/slab_semantics_tests.cpp",
            "tests/architecture_tests.cpp",
            "tests/architectural_schedule_tests.cpp",
            "tests/desktop_smoke.cpp",
            "docs/project-format.md",
        ),
        "anchors": (
            "slab",
            "holes",
            "solid",
            "projection",
            "native",
            "schedule",
            "volume",
        ),
        "tests": ("slab_semantics", "architecture", "architectural_schedule", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers canonical solid and quantity derivation in Debug and Release. "
            "Independent geometric reference parity and production CAD qualification remain open."
        ),
    },
    "COMP-GEO-002": {
        "acceptance": (
            "The constraint solver reports degrees of freedom, redundancy, and conflicts with stable "
            "diagnostics while preserving the documented PlaneGCS/Eigen integration boundary."
        ),
        "sources": (
            "include/sketch/constraints.hpp",
            "src/core/constraints.cpp",
            "tests/constraints_tests.cpp",
            "tests/constraint_integrity_tests.cpp",
            "cmake/PlaneGCS.cmake",
            "docs/constraint-authoring.md",
        ),
        "anchors": (
            "PlaneGCS",
            "Eigen",
            "degrees_of_freedom",
            "redundant",
            "conflict",
            "constraint",
        ),
        "tests": ("constraints", "constraint_integrity", "constraint_scaling"),
        "qualification_boundary": (
            "This evidence covers the bounded local constraint solver and deterministic diagnostics. "
            "Full architectural constraint parity and production qualification remain open."
        ),
    },
    "COMP-IO-001": {
        "acceptance": (
            "DXF and IFC project exchanges run through isolated local adapters, preserve supported "
            "content, and report unsupported or failed reconstruction explicitly."
        ),
        "sources": (
            "include/sketch/interchange_profile.hpp",
            "src/core/interchange_profile.cpp",
            "tests/interchange_profile_tests.cpp",
            "include/sketch/dxf_project_exchange.hpp",
            "src/core/dxf_project_exchange.cpp",
            "tests/dxf_project_exchange_tests.cpp",
            "tests/dxf_desktop_workflow_tests.cpp",
            "include/sketch/ifc_project_exchange.hpp",
            "src/core/ifc_project_exchange.cpp",
            "tests/ifc_project_exchange_tests.cpp",
            "tests/ifc_desktop_workflow_tests.cpp",
            "docs/interchange-profiles.md",
        ),
        "anchors": (
            "InterchangeProfile",
            "isolated",
            "worker",
            "failure",
            "capabilities",
            "required_resources",
            "preserve_reference_and_report",
            "IFC",
            "DXF",
        ),
        "tests": (
            "interchange_profile",
            "dxf_project_exchange",
            "ifc_project_exchange",
            "dxf_desktop_workflow",
            "ifc_desktop_workflow",
        ),
        "qualification_boundary": (
            "This evidence covers the bounded local DXF and IFC adapters and desktop workflows. "
            "Broad native-file fidelity, clean-machine adapter discovery, and production qualification "
            "remain open."
        ),
    },
    "COMP-IO-002": {
        "acceptance": (
            "Each interchange profile declares its capabilities, required resources, loaded module "
            "allowlist, and licensing review before use."
        ),
        "sources": (
            "include/sketch/interchange_profile.hpp",
            "src/core/interchange_profile.cpp",
            "tests/interchange_profile_tests.cpp",
            "docs/interchange-profiles.md",
        ),
        "anchors": (
            "pdf",
            "PrintSupport",
            "module_allowlist",
            "loaded_modules",
            "license_review",
        ),
        "tests": ("interchange_profile",),
        "qualification_boundary": (
            "This evidence covers local profile attestation and module/license policy checks. "
            "Independent license approval and clean-machine module verification remain open."
        ),
    },
    "COMP-IO-003": {
        "acceptance": (
            "Offline georeferencing binds declared PROJ resources, disables network callbacks, and "
            "validates identity transforms without silently using remote data."
        ),
        "sources": (
            "include/sketch/interchange_profile.hpp",
            "src/core/interchange_profile.cpp",
            "tests/interchange_profile_tests.cpp",
            "include/sketch/georeferencing_runtime.hpp",
            "src/core/georeferencing_runtime.cpp",
            "tests/georeferencing_runtime_tests.cpp",
            "tests/georeferencing_contract_tests.cpp",
            "docs/interchange-profiles.md",
        ),
        "anchors": (
            "proj_network_disabled",
            "proj_network_callbacks_disabled",
            "PROJ_NETWORK",
            "proj.db",
            "network_enabled",
            "identity_transform_validated",
            "OfflineGeoResources",
        ),
        "tests": ("interchange_profile", "georeferencing_runtime", "georeferencing_contract"),
        "qualification_boundary": (
            "This evidence covers declared local PROJ resources and network-disabled runtime checks. "
            "Coordinate-system breadth and production field qualification remain open."
        ),
    },
    "APX-WF-001": {
        "acceptance": (
            "Draw First creates a closed classified boundary from geometry, then saves, reopens, and "
            "prints the same analytical result."
        ),
        "sources": (
            "src/desktop/main_window.cpp",
            "tests/boundary_workflow_tests.cpp",
            "tests/desktop_smoke.cpp",
            "docs/boundary-authoring.md",
            "docs/desktop-workflow.md",
        ),
        "anchors": (
            "Draw First",
            "draw_first",
            "close_chain",
            "classification",
            "save",
            "reopen",
            "print",
        ),
        "tests": ("boundary_workflow", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers the local Draw First workflow and persisted output path. Physical "
            "Apex key behavior, native files, and production qualification remain open."
        ),
    },
    "APX-WF-002": {
        "acceptance": (
            "Define First accepts a manual ordered chain, assigns classification, closes it, and "
            "persists the accepted result through the desktop workflow."
        ),
        "sources": (
            "src/core/boundary_authoring_session.cpp",
            "tests/boundary_authoring_session_tests.cpp",
            "tests/boundary_workflow_tests.cpp",
            "docs/boundary-authoring.md",
        ),
        "anchors": (
            "Define First",
            "define_first",
            "set_classification",
            "manual",
            "close_chain",
            "accepted_chains",
            "save",
        ),
        "tests": ("boundary_authoring_session", "boundary_workflow", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers deterministic Define First authoring and classification receipts. "
            "Native Apex keyboard mapping and production qualification remain open."
        ),
    },
    "APX-WF-003": {
        "acceptance": (
            "An unfinished authoring session can recover source geometry, assemble a valid boundary, "
            "and undo or discard the recovery without losing the original document."
        ),
        "sources": (
            "src/core/boundary_authoring_recovery.cpp",
            "src/core/boundary_active_recovery.cpp",
            "tests/boundary_authoring_recovery_tests.cpp",
            "tests/desktop_workspace_recovery_tests.cpp",
            "src/desktop/main_window.cpp",
            "tests/desktop_smoke.cpp",
            "docs/desktop-workflow.md",
        ),
        "anchors": (
            "recovery",
            "unfinished",
            "assemble_boundary_from_segments",
            "source geometry",
            "undo",
        ),
        "tests": ("boundary_authoring_recovery", "desktop_workspace_recovery", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers local recovery candidates and undoable desktop admission. Crash-loss "
            "qualification and production recovery certification remain open."
        ),
    },
    "APX-KEY-001": {
        "acceptance": (
            "Keyboard-first distance and direction entry commits valid values, supports focus and "
            "cancel behavior, and leaves invalid entries recoverable."
        ),
        "sources": (
            "src/desktop/boundary_input_dialog.cpp",
            "tests/boundary_input_dialog_tests.cpp",
            "src/desktop/main_window.cpp",
            "tests/desktop_smoke.cpp",
            "docs/boundary-authoring.md",
        ),
        "anchors": (
            "QLineEdit",
            "Escape",
            "Tab",
            "focus",
            "distance",
            "direction",
            "commit",
            "cancel",
        ),
        "tests": ("boundary_input_dialog", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers local keyboard dialog semantics and deterministic commit/cancel "
            "behavior. Physical keypad/keyboard parity and production qualification remain open."
        ),
    },
    "APX-CURVE-001": {
        "acceptance": (
            "Chord, arc-length, height, angle, and tangent curve entries produce true analytical arcs "
            "whose sweep and perimeter survive edit and save/reopen."
        ),
        "sources": (
            "src/core/geometry.cpp",
            "src/core/boundary_authoring_session.cpp",
            "tests/curve_construction_tests.cpp",
            "tests/boundary_authoring_session_tests.cpp",
            "tests/calculation_tests.cpp",
            "src/desktop/main_window.cpp",
            "tests/desktop_smoke.cpp",
            "docs/boundary-authoring.md",
            "docs/desktop-workflow.md",
        ),
        "anchors": (
            "arc_from_chord_angle",
            "arc_from_chord_arc_length",
            "arc_from_chord_height",
            "arc_from_start_tangent",
            "curve_input",
            "sweep",
            "perimeter",
            "save",
        ),
        "tests": ("curve_construction", "boundary_authoring_session", "calculations", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers analytical curve construction and local persistence. Native Apex "
            "curve fixtures, field input, and production qualification remain open."
        ),
    },
    "APX-ANNO-001": {
        "acceptance": (
            "Editable annotation templates and instances preserve placement, font, text height, "
            "visibility, and provenance through save and exchange."
        ),
        "sources": (
            "include/sketch/annotation_catalog.hpp",
            "src/core/annotation_catalog.cpp",
            "include/sketch/annotation_entity_codec.hpp",
            "src/core/annotation_entity_codec.cpp",
            "tests/annotation_catalog_tests.cpp",
            "tests/annotation_entity_codec_tests.cpp",
            "tests/desktop_smoke.cpp",
            "tests/dxf_project_exchange_tests.cpp",
            "docs/annotation-catalog.md",
        ),
        "anchors": (
            "annotation",
            "template",
            "visibility",
            "placement",
            "font",
            "text_height",
            "save",
            "filter",
        ),
        "tests": ("annotation_catalog", "annotation_entity_codec", "desktop_workflow", "dxf_project_exchange"),
        "qualification_boundary": (
            "This evidence covers local annotation libraries, instances, and supported exchange. "
            "Full Apex label catalog parity and production output qualification remain open."
        ),
    },
    "APX-ANNO-002": {
        "acceptance": (
            "Boundary dimensions retain analytical references and allow explicit placement, visibility, "
            "rotation, scale, curve, and area presentation without storing measurement truth in text."
        ),
        "sources": (
            "src/core/boundary_dimension.cpp",
            "tests/boundary_dimension_tests.cpp",
            "tests/boundary_workflow_tests.cpp",
            "docs/desktop-workflow.md",
        ),
        "anchors": (
            "Dimension",
            "placement",
            "visibility",
            "reference",
            "rotation",
            "paper",
            "scale",
            "curve",
            "area",
            "save",
        ),
        "tests": ("boundary_dimensions", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers analytical boundary dimension semantics and desktop placement. "
            "Native Apex dimension layout and calibrated production print remain open."
        ),
    },
    "APX-ANNO-003": {
        "acceptance": (
            "Annotation styles preserve text height, stroke/fill colors, fill patterns, visibility, "
            "and target binding across instance edits and serialization."
        ),
        "sources": (
            "include/sketch/annotation_catalog.hpp",
            "src/core/annotation_catalog.cpp",
            "include/sketch/annotation_entity_codec.hpp",
            "src/core/annotation_entity_codec.cpp",
            "tests/annotation_catalog_tests.cpp",
            "tests/annotation_entity_codec_tests.cpp",
            "docs/annotation-catalog.md",
        ),
        "anchors": (
            "style",
            "fill",
            "visibility",
            "stroke",
            "text_height",
            "color",
            "target",
        ),
        "tests": ("annotation_catalog", "annotation_entity_codec", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers portable local annotation presentation state. Font availability, "
            "native Apex style parity, and production rendering qualification remain open."
        ),
    },
    "APX-SYM-001": {
        "acceptance": (
            "The local symbol catalog contains at least 468 scaled residential and light-commercial "
            "entries with deterministic previews, filtering, resizing, rotation, and output metadata."
        ),
        "sources": (
            "include/sketch/annotation_catalog.hpp",
            "src/core/annotation_catalog.cpp",
            "src/cli/main.cpp",
            "include/sketch/annotation_entity_codec.hpp",
            "src/core/annotation_entity_codec.cpp",
            "tests/annotation_catalog_tests.cpp",
            "tests/test_cli.py",
            "tests/annotation_entity_codec_tests.cpp",
            "tests/dxf_project_exchange_tests.cpp",
            "scripts/validate_symbol_catalog.py",
            "tests/test_symbol_catalog_manifest.py",
            "docs/annotation-catalog.md",
        ),
        "anchors": (
            "468",
            "scale",
            "plumbing",
            "furniture",
            "commercial",
            "preview",
            "catalog_revision",
            "resize",
            "rotation",
            "symbols",
        ),
        "tests": (
            "annotation_catalog",
            "annotation_entity_codec",
            "dxf_project_exchange",
            "project_cli",
            "packaging_symbol_catalog_manifest",
            "desktop_workflow",
        ),
        "qualification_boundary": (
            "This evidence covers the deterministic local symbol library and manifest checks. Final "
            "print/export legibility and complete Apex catalog certification remain open."
        ),
    },
    "APX-TRACE-001": {
        "acceptance": (
            "PDF and raster references are imported through the bounded local worker, preserve source "
            "assets and pages, and provide validated previews for tracing."
        ),
        "sources": (
            "include/sketch/reference_asset.hpp",
            "src/core/reference_asset.cpp",
            "src/desktop/reference_import.hpp",
            "src/desktop/reference_import.cpp",
            "src/desktop/reference_import_worker.cpp",
            "tests/reference_asset_tests.cpp",
            "tests/reference_import_tests.cpp",
            "tests/test_reference_import_boundary.py",
            "docs/import-worker-security.md",
            "docs/reference-assets.md",
        ),
        "anchors": (
            "PDF",
            "PNG",
            "JPEG",
            "BMP",
            "TIFF",
            "source_preserved",
            "page",
            "preview",
            "asset",
            "WIC",
        ),
        "tests": ("reference_assets", "reference_import", "reference_import_boundary", "desktop_workflow"),
        "qualification_boundary": (
            "This evidence covers bounded local PDF/raster import, page handling, source preservation, "
            "and worker validation. Full tracing parity, clean-machine codecs, and production "
            "qualification remain open."
        ),
    },
})


WORKFLOW_RULES.update({
    "CORE-OFF-001": {
        "acceptance": "The offline policy blocks unknown external dependencies and allows a runtime explicitly declared free of account, activation, subscription, and network requirements.",
        "sources": (
            "include/sketch/offline_policy.hpp", "src/core/offline_policy.cpp", "src/desktop/main_window.cpp",
            "tests/offline_policy_tests.cpp", "docs/offline-policy.md",
        ),
        "anchors": ("evaluate_offline_policy", "require_no_account", "require_no_activation", "require_no_subscription", "require_no_network", "startup_allowed", "offline"),
        "tests": ("offline_policy", "desktop_workflow"),
        "qualification_boundary": "This evidence covers the deterministic local offline policy and desktop startup declaration; network-denied clean-machine execution remains open.",
    },
    "CORE-OFF-002": {
        "acceptance": "Startup and static import policy explicitly reject account, activation, subscription, network, and direct network-library dependencies.",
        "sources": (
            "include/sketch/offline_policy.hpp", "src/core/offline_policy.cpp", "tests/offline_policy_tests.cpp",
            "scripts/offline_static_audit.py", "tests/test_offline_static_audit.py",
            "docs/dependencies/offline-static-audit.md", "docs/offline-policy.md",
        ),
        "anchors": ("account", "activation", "subscription", "network", "static_network_audit_passed", "direct_network_imports", "offline"),
        "tests": ("offline_policy", "packaging_offline_static_audit"),
        "qualification_boundary": "This evidence covers local policy and static direct-import checks; dynamic loading, entitlement absence, and network-denied runtime qualification remain open.",
    },
    "CORE-OFF-003": {
        "acceptance": "The offline package stages the application runtime, fonts, help, libraries, resources, installer, and verifier without a network prerequisite.",
        "sources": (
            "include/sketch/offline_policy.hpp", "scripts/stage_portable_package.py", "scripts/install-offline-bundle.ps1",
            "tests/test_stage_portable_package.py", "tests/test_stage_offline_bundle.py",
            "docs/dependencies/portable-package.md", "docs/dependencies/offline-installer.md",
        ),
        "anchors": ("stage_package", "offline", "installer", "runtime", "fonts", "help", "bundle", "network"),
        "tests": ("packaging_stage_portable_package", "packaging_stage_offline_bundle"),
        "qualification_boundary": "This evidence covers deterministic local staging and hash verification; signed installer and clean-machine installation qualification remain open.",
    },
    "CORE-OWN-001": {
        "acceptance": "The source kit records pinned dependencies, reproducible build inputs, an explicit allowlist, and local build instructions for private delivery.",
        "sources": (
            "third_party/dependencies.json", "scripts/source_kit_manifest.py", "tests/test_source_kit_manifest.py",
            "docs/dependencies/source-kit.md",
        ),
        "anchors": ("source-kit", "manifest", "reproducible", "build", "dependency", "allowlist", "source"),
        "tests": ("packaging_source_kit_manifest", "packaging_update_source_kit_allowlist", "source_kit_allowlist_contract"),
        "qualification_boundary": "This evidence covers the repository source/build kit and manifest contract; private handoff and clean-machine reproducibility remain open.",
    },
    "CORE-OWN-002": {
        "acceptance": "The local project format preserves inspectable geometry, metadata, assets, history, ownership, and migration behavior through save and reopen.",
        "sources": (
            "include/sketch/project_ownership.hpp", "src/core/project_ownership.cpp", "src/core/project_store.cpp",
            "include/sketch/document.hpp", "src/core/document.cpp", "tests/project_ownership_tests.cpp",
            "tests/project_ownership_process_tests.cpp", "tests/document_tests.cpp", "tests/desktop_smoke.cpp",
        ),
        "anchors": ("ProjectOwnershipSession", "DocumentSnapshot", "revision", "migration", "asset", "history", "read-only", "save"),
        "tests": ("project_ownership", "project_ownership_process", "project_storage", "document_commands", "desktop_workflow"),
        "qualification_boundary": "This evidence covers the documented local project format and ownership safeguards; broad legacy migration fidelity remains open.",
    },
    "CORE-SCOPE-001": {
        "acceptance": "The product scope declares Windows 11 x64, imperial and metric units, and residential and light-commercial markets.",
        "sources": (
            "include/sketch/product_scope.hpp", "src/core/product_scope.cpp", "tests/product_scope_tests.cpp", "docs/product-scope.md",
        ),
        "anchors": ("Windows 11", "x64", "residential", "light_commercial", "imperial", "metric", "ProductScope"),
        "tests": ("product_scope",),
        "qualification_boundary": "This evidence covers the declared product scope and validation contract; market workflow qualification remains open.",
    },
    "COMP-LIC-001": {
        "acceptance": "Every shipped component has a pinned source or revision, build input, license record, notice path, and distribution inventory entry.",
        "sources": (
            "third_party/dependencies.json", "third_party/distribution-components.json", "scripts/distribution_inventory.py",
            "scripts/distribution_sbom.py", "scripts/stage_portable_package.py", "tests/test_distribution_sbom.py",
            "tests/test_stage_portable_package.py", "docs/dependencies/distribution-inventory.md",
            "docs/dependencies/distribution-sbom.md", "docs/dependencies/planegcs.md",
        ),
        "anchors": ("distribution", "inventory", "SBOM", "license", "source", "notice", "dependency"),
        "tests": ("packaging_distribution_inventory", "packaging_distribution_sbom", "packaging_stage_portable_package"),
        "qualification_boundary": "This evidence covers declared component inventory, file-level source provenance preservation, SPDX generation, and documented replacement-build boundaries. Complete matching sources, live relink/replacement proof, notice review, legal clearance, and redistributability approval remain open.",
    },
    "COMP-LIC-002": {
        "acceptance": "Original application paths and third-party paths have explicit ownership and provenance classifications suitable for private delivery or later licensing review.",
        "sources": (
            "third_party/dependencies.json", "third_party/source-provenance.json", "scripts/source_provenance_audit.py",
            "tests/test_source_provenance_audit.py", "docs/dependencies/source-provenance.md",
        ),
        "anchors": ("source-provenance", "ownership", "provenance", "third_party", "application", "audit"),
        "tests": ("packaging_source_provenance_audit", "source_kit_allowlist_contract"),
        "qualification_boundary": "This evidence covers repository provenance declarations; independent legal review and future publication decisions remain open.",
    },
    "COMP-LIC-003": {
        "acceptance": "The distribution inventory identifies GPL, AGPL, LGPL, commercial, and other license obligations and rejects unapproved dependency policy violations.",
        "sources": (
            "scripts/distribution_inventory.py", "tests/test_distribution_inventory.py", "docs/dependencies/distribution-inventory.md",
            "third_party/distribution-components.json",
        ),
        "anchors": ("GPL", "LGPL", "commercial", "license", "distribution", "inventory"),
        "tests": ("packaging_distribution_inventory",),
        "qualification_boundary": "This evidence covers the declared dependency policy and inventory checks; final counsel approval remains open.",
    },
    "GEO-CON-003": {
        "acceptance": "Constraint previews report underconstrained, redundant, and conflicting states with diagnostics and never silently relax locked measurements.",
        "sources": (
            "include/sketch/constraints.hpp", "src/core/constraints.cpp", "src/core/constraint_integrity.cpp",
            "tests/constraints_tests.cpp", "tests/constraint_integrity_tests.cpp", "include/sketch/constraint_authoring.hpp",
            "src/core/constraint_authoring.cpp", "src/desktop/constraint_dialog.cpp", "tests/constraint_authoring_tests.cpp",
            "tests/constraint_dialog_tests.cpp",
        ),
        "anchors": ("degrees_of_freedom", "redundant", "conflicting_constraints", "rejected_conflict", "diagnostic", "locked"),
        "tests": ("constraints", "constraint_integrity", "constraint_authoring", "constraint_dialog"),
        "qualification_boundary": "This evidence covers local solver diagnostics and atomic rejection; full constraint parity and production qualification remain open.",
    },
    "GEO-CON-004": {
        "acceptance": "Constraint solving preserves protected winding, topology, stable endpoint identity, and branch behavior while rejecting mirrored or unstable solutions.",
        "sources": (
            "src/core/constraint_integrity.cpp", "src/core/constraints.cpp", "tests/constraint_integrity_tests.cpp",
            "tests/constraints_tests.cpp", "include/sketch/constraint_authoring.hpp", "src/core/constraint_authoring.cpp",
            "src/desktop/constraint_dialog.cpp", "tests/constraint_authoring_tests.cpp", "tests/constraint_dialog_tests.cpp",
        ),
        "anchors": ("winding", "topology", "orientation", "branch", "stable", "preserve", "mirror"),
        "tests": ("constraints", "constraint_integrity", "constraint_authoring", "constraint_dialog"),
        "qualification_boundary": "This evidence covers deterministic branch and topology guards; broader architectural constraint coverage remains open.",
    },
    "GEO-CON-005": {
        "acceptance": "Failed geometric edits leave the previous valid document state unchanged and return a reversible, understandable error result.",
        "sources": (
            "src/core/document.cpp", "src/core/constraint_integrity.cpp", "src/core/wall_semantics.cpp",
            "tests/constraint_integrity_tests.cpp", "tests/test_cli.py", "include/sketch/constraint_authoring.hpp",
            "src/core/constraint_authoring.cpp", "src/desktop/constraint_dialog.cpp", "tests/constraint_authoring_tests.cpp",
            "tests/constraint_dialog_tests.cpp",
        ),
        "anchors": ("rejected_unchanged", "previous", "reversible", "error", "stale", "atomic", "DocumentError"),
        "tests": ("constraint_integrity", "constraint_authoring", "constraint_dialog", "document_commands"),
        "qualification_boundary": "This evidence covers local atomic failure behavior and history safety; production stress and crash qualification remain open.",
    },
    "APX-EDIT-004": {
        "acceptance": "Supported areas and objects can be reopened, redefined, deleted, and restored through explicit history while retaining relationships.",
        "sources": (
            "include/sketch/geometry_operations.hpp", "src/core/geometry_operations.cpp", "tests/geometry_operations_tests.cpp",
            "docs/geometry-operations.md", "src/desktop/main_window.cpp", "include/sketch/desktop/main_window.hpp",
            "tests/desktop_smoke.cpp", "docs/desktop-workflow.md",
        ),
        "anchors": ("reopen", "redefine", "delete", "remove", "undo", "redo", "clone", "boundary"),
        "tests": ("geometry_operations", "desktop_workflow"),
        "qualification_boundary": "This evidence covers supported local geometry editing and history; complete Apex object parity remains open.",
    },
    "APX-TRACE-002": {
        "acceptance": "A reference asset calibrates against a known measurement while retaining the original expression, source points, units, and derived scale.",
        "sources": ("include/sketch/reference_asset.hpp", "src/core/reference_asset.cpp", "tests/reference_asset_tests.cpp", "docs/reference-assets.md"),
        "anchors": ("calibrat", "known_distance", "metres_per_source_unit", "source", "unit", "measure_metres", "provenance"),
        "tests": ("reference_assets",),
        "qualification_boundary": "This evidence covers deterministic local calibration provenance; external measurement-device and production tracing qualification remain open.",
    },
    "APX-TRACE-003": {
        "acceptance": "Reference transforms support scale, rotation, horizontal and vertical flips, intensity, visibility, and undo without changing source bytes.",
        "sources": ("include/sketch/reference_asset.hpp", "src/core/reference_asset.cpp", "tests/reference_asset_tests.cpp", "docs/reference-assets.md"),
        "anchors": ("transform", "scale", "rotation", "flip_horizontal", "flip_vertical", "intensity", "undo", "source"),
        "tests": ("reference_assets", "desktop_workflow"),
        "qualification_boundary": "This evidence covers local reference transform semantics; full trace editing parity and production visual qualification remain open.",
    },
    "APX-DOC-001": {
        "acceptance": "Multipage projects preserve subject information, area attributes, shared models, page presentation, independent visibility, and stable IDs.",
        "sources": ("include/sketch/multipage_document.hpp", "src/core/multipage_document.cpp", "tests/multipage_document_tests.cpp", "docs/multipage-projects.md"),
        "anchors": ("multipage", "subject", "area", "attribute", "presentation", "shared", "page", "version"),
        "tests": ("multipage_document", "desktop_workflow"),
        "qualification_boundary": "This evidence covers the local multipage semantic model; complete production sheet lifecycle and Apex parity remain open.",
    },
    "APX-DOC-002": {
        "acceptance": "Local save, import, export, and declared legacy exchange workflows preserve supported content and emit explicit fidelity reports.",
        "sources": (
            "src/desktop/main_window.cpp", "include/sketch/dxf_project_exchange.hpp", "src/core/dxf_project_exchange.cpp",
            "tests/dxf_project_exchange_tests.cpp", "tests/dxf_desktop_workflow_tests.cpp", "docs/interchange-profiles.md",
        ),
        "anchors": ("save", "import", "export", "legacy", "fidelity", "DXF", "report"),
        "tests": ("dxf_project_exchange", "dxf_desktop_workflow", "project_exchange", "desktop_workflow"),
        "qualification_boundary": "This evidence covers the bounded local exchange path; native Apex compatibility and broad legacy fidelity remain open.",
    },
    "APX-DOC-003": {
        "acceptance": "Multipage sheets expose print preview and PDF/image output using independent presentation scale and the shared output scene.",
        "sources": ("src/desktop/main_window.cpp", "tests/desktop_smoke.cpp", "include/sketch/sheet_view_model.hpp", "src/core/sheet_view_model.cpp", "tests/sheet_view_model_tests.cpp", "docs/sheets-views.md"),
        "anchors": ("print", "preview", "PDF", "image", "scale", "sheet", "output", "QPrinter"),
        "tests": ("sheet_output_scene", "output_fingerprint", "desktop_workflow"),
        "qualification_boundary": "This evidence covers local output scene and presentation controls; printer calibration and production output certification remain open.",
    },
    "APX-UI-001": {
        "acceptance": "The workspace provides pan, zoom, grid, snap, independent output scale, and responsive overview navigation over the shared canvas.",
        "sources": ("src/desktop/main_window.cpp", "tests/desktop_smoke.cpp", "docs/workspace-ui.md"),
        "anchors": ("gridTool", "snapTool", "overviewMapTool", "overview", "scale", "workspace", "Fit"),
        "tests": ("desktop_workflow", "workspace_navigation"),
        "qualification_boundary": "This evidence covers local navigation and workspace controls; agreed reference-hardware performance qualification remains open.",
    },
    "APX-UI-002": {
        "acceptance": "Visibility filters, overview navigation, themes, and saved workspace configurations persist locally and apply to the drawing views.",
        "sources": ("src/core/project_visibility.cpp", "src/visualization/native_model_view.cpp", "src/desktop/main_window.cpp", "tests/desktop_smoke.cpp", "docs/workspace-accessibility.md"),
        "anchors": ("visibility", "overview", "theme", "dark", "light", "saved", "workspace", "filter"),
        "tests": ("project_visibility", "visibility_workflow", "desktop_workflow"),
        "qualification_boundary": "This evidence covers local visibility and workspace presentation behavior; accessibility and DPI certification remain open.",
    },
})


# The architectural, workspace, and specialized deterministic contracts below
# have complete Debug/Release fixtures.  They certify the implemented local
# behavior while keeping physical-device, native-Apex, printer, and clean-
# machine qualification as explicit release boundaries.
WORKFLOW_RULES.update({
    "APX-UI-003": {
        "acceptance": "Commands are searchable, quick-access actions and shortcut bindings are editable and persisted locally, and Measurement and Architectural tabs remain available.",
        "sources": ("src/desktop/main_window.cpp", "tests/desktop_smoke.cpp", "docs/workspace-ui.md", "docs/workspace-accessibility.md"),
        "anchors": ("showCommandPalette", "quickAccess", "keyboardShortcut", "workspaceTabs", "Customize", "shortcut"),
        "tests": ("desktop_workflow", "workspace_navigation"),
        "qualification_boundary": "This evidence covers local command discovery, shortcut persistence, quick access, and workspace tabs. Physical keyboard coverage and the unified production gate remain open.",
    },
    "APX-SPEC-001": {
        "acceptance": "Survey fixtures enter bearings and distances, report closure and acreage, preserve traverse provenance, and add a closed traverse to the document through an undoable command.",
        "sources": ("include/sketch/survey_contract.hpp", "src/core/survey_contract.cpp", "tests/survey_contract_tests.cpp", "src/desktop/main_window.cpp", "tests/desktop_smoke.cpp", "docs/survey-georeferencing-contracts.md"),
        "anchors": ("Survey", "traverse", "acreage", "closure", "provenance", "bearing"),
        "tests": ("survey_contract", "desktop_workflow"),
        "qualification_boundary": "This evidence covers deterministic local survey entry, closure, acreage, provenance, and desktop insertion. Legal-survey behavior, Apex exchange, and production qualification remain open.",
    },
    "APX-SPEC-002": {
        "acceptance": "Georeferencing fixtures validate an explicit CRS, control points, affine transform, residuals, contained resources, and save/reopen behavior without network access.",
        "sources": ("include/sketch/georeferencing_contract.hpp", "src/core/georeferencing_contract.cpp", "include/sketch/georeferencing_entity_codec.hpp", "src/core/georeferencing_entity_codec.cpp", "include/sketch/georeferencing_runtime.hpp", "src/core/georeferencing_runtime.cpp", "tests/georeferencing_contract_tests.cpp", "tests/georeferencing_entity_codec_tests.cpp", "tests/georeferencing_runtime_tests.cpp", "src/desktop/main_window.cpp", "tests/desktop_smoke.cpp", "docs/survey-georeferencing-contracts.md"),
        "anchors": ("Georeferencing", "CRS", "control", "residual", "affine", "offline", "resource"),
        "tests": ("georeferencing_contract", "georeferencing_entity_codec", "georeferencing_runtime", "desktop_workflow"),
        "qualification_boundary": "This evidence covers the typed offline georeferencing contract and development-host workflow. A production resource inventory, external CRS review, and network-monitored qualification remain open.",
    },
    "ARCH-MOD-001": {
        "acceptance": "Wall fixtures author straight, curved, sloped, and layered walls with hosted openings, materials, joins, and stable semantic identity across the shared document path.",
        "sources": ("include/sketch/wall_semantics.hpp", "src/core/wall_semantics.cpp", "src/core/document.cpp", "src/architecture/architecture.cpp", "src/architecture/document_solid.cpp", "src/architecture/architectural_schedule.cpp", "src/desktop/main_window.cpp", "src/desktop/plan_canvas.cpp", "tests/wall_semantics_tests.cpp", "tests/architecture_tests.cpp", "tests/wall_join_tests.cpp", "tests/desktop_smoke.cpp", "tests/architectural_schedule_tests.cpp", "tests/document_tests.cpp", "docs/project-format.md"),
        "anchors": ("struct Wall", "HostedOpening", "WallLayer", "validate_wall_semantics", "slope_rise", "material", "join"),
        "tests": ("wall_semantics", "architecture", "wall_join", "document_commands", "desktop_workflow"),
        "qualification_boundary": "This evidence covers the local wall semantic and solid/projection paths. Complete material libraries, physical output, native Apex parity, and production acceptance remain open.",
    },
    "ARCH-MOD-002": {
        "acceptance": "Hosted door and window fixtures validate host relationships, edit dimensions and handing, derive analytic presentation geometry, and preserve the opening graph through document history.",
        "sources": ("src/architecture/architecture.cpp", "include/sketch/opening_assembly.hpp", "src/core/opening_assembly.cpp", "src/desktop/hosted_opening_dialog.cpp", "src/core/door_operation.cpp", "tests/door_operation_tests.cpp", "tests/opening_assembly_tests.cpp", "src/desktop/main_window.cpp", "tests/desktop_smoke.cpp", "tests/modal_authoring_tests.cpp", "docs/hosted-openings.md"),
        "anchors": ("OpeningAssembly", "validate_opening_assembly", "HostedOpeningDialog", "DoorOperation", "wall_id", "swing", "opening"),
        "tests": ("opening_assembly", "door_operation", "modal_authoring", "desktop_workflow"),
        "qualification_boundary": "This evidence covers typed hosted openings and local editing. Complete catalog coverage, physical hardware, native Apex fidelity, and production sheet qualification remain open.",
    },
    "ARCH-MOD-003": {
        "acceptance": "Floor, ceiling, foundation, slab, and room-volume fixtures retain validated boundaries, holes, levels, materials, elevations, and calculated quantities through plan and 3D decoding.",
        "sources": ("src/architecture/architecture.cpp", "include/sketch/document_solid.hpp", "src/architecture/document_solid.cpp", "src/desktop/main_window.cpp", "tests/architecture_tests.cpp", "tests/building_entity_tests.cpp", "tests/desktop_smoke.cpp", "docs/project-format.md", "docs/native-3d-view.md", "docs/architectural-projections.md"),
        "anchors": ("Slab", "read_document_slab", "RoomVolume", "holes", "elevation", "material", "quantity"),
        "tests": ("architecture", "building_entities", "building_plan_projection", "desktop_workflow"),
        "qualification_boundary": "This evidence covers local floor-system and room-volume semantics. Broader catalog authoring, native Apex compatibility, and complete production output certification remain open.",
    },
    "ARCH-MOD-004": {
        "acceptance": "Beam and column fixtures create validated semantic solids, project into plan, expose editable properties, and contribute source-backed schedule quantities through save/reopen.",
        "sources": ("src/architecture/building_objects.cpp", "src/architecture/building_entity.cpp", "src/architecture/building_plan_projection.cpp", "src/architecture/architectural_schedule.cpp", "src/desktop/building_object_dialog.cpp", "src/desktop/main_window.cpp", "tests/building_object_tests.cpp", "tests/building_object_dialog_tests.cpp", "tests/building_plan_projection_tests.cpp", "tests/architectural_schedule_tests.cpp", "tests/desktop_smoke.cpp"),
        "anchors": ("RectangularColumn", "CircularColumn", "Beam", "make_beam", "building", "schedule", "volume"),
        "tests": ("building_objects", "building_entities", "building_plan_projection", "architectural_schedule", "desktop_workflow"),
        "qualification_boundary": "This evidence covers local beam/column authoring, projection, schedules, and history. Complete commercial detailing, physical output, and production qualification remain open.",
    },
    "ARCH-MOD-005": {
        "acceptance": "Flat, sloped, gable, and hip roof fixtures retain defining pitch, joins, openings, materials, semantic identity, and plan/elevation/section projections.",
        "sources": ("include/sketch/roof_join_semantics.hpp", "src/core/roof_join_semantics.cpp", "src/core/document.cpp", "src/architecture/architecture.cpp", "src/visualization/native_model_view.cpp", "src/architecture/building_objects.cpp", "src/architecture/building_entity.cpp", "src/architecture/building_plan_projection.cpp", "src/desktop/building_object_dialog.cpp", "src/desktop/main_window.cpp", "tests/building_object_tests.cpp", "tests/roof_join_tests.cpp", "tests/building_object_dialog_tests.cpp", "tests/building_plan_projection_tests.cpp", "tests/desktop_smoke.cpp"),
        "anchors": ("SlopedRoofPanel", "GableRoof", "HipRoof", "RoofOpening", "cut_roof_openings", "roof_join", "pitch"),
        "tests": ("building_objects", "roof_join", "building_plan_projection", "desktop_workflow"),
        "qualification_boundary": "This evidence covers supported local roof forms, cuts, joins, and projections. Unsupported roof variants, physical output, native Apex fidelity, and production acceptance remain open.",
    },
    "ARCH-MOD-006": {
        "acceptance": "Stair, landing, and railing fixtures validate dimensions and level links, produce semantic solids and plan projections, and survive editable history and save/reopen.",
        "sources": ("src/architecture/building_objects.cpp", "src/architecture/building_entity.cpp", "include/sketch/vertical_level_document_adapter.hpp", "src/core/vertical_level_document_adapter.cpp", "src/core/document.cpp", "src/architecture/building_plan_projection.cpp", "src/desktop/building_object_dialog.cpp", "src/desktop/main_window.cpp", "tests/building_object_tests.cpp", "tests/building_object_dialog_tests.cpp", "tests/building_plan_projection_tests.cpp", "tests/document_tests.cpp", "tests/architectural_schedule_tests.cpp", "tests/desktop_smoke.cpp"),
        "anchors": ("StairFlight", "StairLanding", "Railing", "level_connection", "top rail", "rise", "undo"),
        "tests": ("building_objects", "building_entities", "building_plan_projection", "document_commands", "desktop_workflow"),
        "qualification_boundary": "This evidence covers supported local stairs, landings, railings, level links, and history. Complete code-compliant assemblies, physical output, and production qualification remain open.",
    },
    "ARCH-MOD-007": {
        "acceptance": "Level, floor-to-floor, reference-grid, and terrain fixtures validate explicit vertical and site references and persist through the shared document path.",
        "sources": ("include/sketch/vertical_levels.hpp", "src/core/vertical_levels.cpp", "include/sketch/reference_grid.hpp", "src/core/reference_grid.cpp", "src/core/document.cpp", "src/desktop/main_window.cpp", "src/desktop/plan_canvas.cpp", "tests/vertical_levels_tests.cpp", "tests/reference_grid_tests.cpp", "tests/document_tests.cpp", "tests/desktop_smoke.cpp", "include/sketch/terrain_surface.hpp", "src/core/terrain_surface.cpp", "tests/terrain_surface_tests.cpp", "docs/vertical-levels.md", "docs/reference-grids.md", "docs/terrain-surfaces.md"),
        "anchors": ("VerticalLevel", "FloorToFloorLink", "ReferenceGridModel", "TerrainSurface", "elevation", "alignment", "provenance"),
        "tests": ("vertical_levels", "reference_grid", "terrain_surface", "document_commands", "desktop_workflow"),
        "qualification_boundary": "This evidence covers local level, grid, terrain, and alignment semantics. Survey-grade site data, external references, and production qualification remain open.",
    },
    "ARCH-MOD-008": {
        "acceptance": "Assembly catalogs retain separate type and instance properties, material slots, quantity definitions, placements, overrides, and deterministic schedule behavior.",
        "sources": ("include/sketch/assembly_model.hpp", "src/core/assembly_model.cpp", "tests/assembly_model_tests.cpp", "include/sketch/desktop/main_window.hpp", "src/desktop/main_window.cpp", "tests/desktop_smoke.cpp", "docs/assemblies.md"),
        "anchors": ("AssemblyModel", "AssemblyType", "AssemblyMaterial", "AssemblyInstance", "quantity", "material", "placement"),
        "tests": ("assembly_model", "architectural_schedule", "desktop_workflow"),
        "qualification_boundary": "This evidence covers local reusable assembly semantics and catalog editing. A complete production library, native Apex parity, and commercial project qualification remain open.",
    },
    "ARCH-MOD-010": {
        "acceptance": "Existing, demolished, and proposed phases remain explicit, selectable, comparable, and persist through shared document history and coordinated workspace visibility.",
        "sources": ("include/sketch/model_phases.hpp", "src/core/model_phases.cpp", "src/desktop/main_window.cpp", "tests/model_phases_tests.cpp", "tests/desktop_smoke.cpp", "docs/phases-alternatives.md"),
        "anchors": ("ModelPhase", "existing", "demolished", "proposed", "active_alternative", "compare", "undo"),
        "tests": ("model_phases", "desktop_workflow"),
        "qualification_boundary": "This evidence covers deterministic phase semantics and local workspace switching. Full coordinated issue-set output, native Apex compatibility, and production acceptance remain open.",
    },
    "ARCH-3D-001": {
        "acceptance": "The native 3D view consumes the same semantic document objects used by architectural and measurement workflows, supports selection/translation, and emits a deterministic native image path.",
        "sources": ("src/visualization/native_model_view.cpp", "src/desktop/main_window.cpp", "tests/native_view_tests.cpp", "tests/desktop_smoke.cpp", "docs/native-3d-view.md"),
        "anchors": ("NativeModelView", "setSnapshot", "exportViewImage", "AIS_Shape", "OCCT", "selection", "translation"),
        "tests": ("desktop_workflow",),
        "qualification_boundary": "This evidence covers shared semantic decoding and the desktop smoke path. Native OpenGL/window capture, GPU-driver behavior, and full production 3D qualification remain open.",
    },
    "ARCH-VIEW-001": {
        "acceptance": "Plan, elevation, and section definitions project the same semantic building objects and remain linked to typed sheet view definitions and persisted viewports.",
        "sources": ("include/sketch/building_view_projection.hpp", "src/architecture/building_view_projection.cpp", "tests/building_view_projection_tests.cpp", "src/desktop/main_window.cpp", "tests/desktop_smoke.cpp", "docs/architectural-projections.md", "include/sketch/sheet_view_model.hpp", "src/core/sheet_view_model.cpp", "tests/sheet_view_model_tests.cpp", "include/sketch/sheet_view_entity_codec.hpp", "src/core/sheet_view_entity_codec.cpp", "tests/sheet_view_entity_codec_tests.cpp", "docs/sheets-views.md"),
        "anchors": ("BuildingViewKind", "project_building_view", "CoordinatedView", "plan", "elevation", "section", "SheetViewModel", "viewport"),
        "tests": ("building_view_projection", "sheet_view_model", "sheet_view_entity_codec", "desktop_workflow"),
        "qualification_boundary": "This evidence covers deterministic local projection and view persistence. Complete hidden-line/detail production behavior, printer calibration, and native Apex compatibility remain open.",
    },
    "ARCH-VIEW-002": {
        "acceptance": "Section and coordinated view definitions retain cut depth, line treatment, material hatching, detail level, annotations, callouts, and overlay metadata through validation and persistence.",
        "sources": ("include/sketch/building_view_projection.hpp", "src/architecture/building_view_projection.cpp", "tests/building_view_projection_tests.cpp", "include/sketch/sheet_view_model.hpp", "src/core/sheet_view_model.cpp", "tests/sheet_view_model_tests.cpp", "include/sketch/sheet_view_entity_codec.hpp", "src/core/sheet_view_entity_codec.cpp", "tests/sheet_view_entity_codec_tests.cpp", "docs/architectural-projections.md", "docs/sheets-views.md"),
        "anchors": ("cut_depth_m", "cut_line_mm", "hatch_pattern", "detail", "callout", "overlay", "section"),
        "tests": ("building_view_projection", "sheet_view_model", "sheet_view_entity_codec"),
        "qualification_boundary": "This evidence covers deterministic section/view metadata and persistence. Full detail-overlay authoring, production annotation standards, and print qualification remain open.",
    },
    "ARCH-SCH-001": {
        "acceptance": "Door/window, room, material, assembly, and building schedule fixtures expose marks, dimensions, counts, areas, deductions, net quantities, and source references.",
        "sources": ("include/sketch/document_schedule_adapter.hpp", "src/core/document_schedule_adapter.cpp", "include/sketch/architectural_schedule.hpp", "src/architecture/architectural_schedule.cpp", "tests/document_schedule_adapter_tests.cpp", "tests/architectural_schedule_tests.cpp", "include/sketch/schedule_model.hpp", "src/core/schedule_model.cpp", "tests/schedule_model_tests.cpp", "docs/schedules.md"),
        "anchors": ("ScheduleRowKind", "build_architectural_schedules", "material_summary", "deduction", "net", "quantity", "source"),
        "tests": ("document_schedule_adapter", "architectural_schedule", "schedule_model"),
        "qualification_boundary": "This evidence covers deterministic schedule projection and source-backed quantities. Complete market-specific schedules, native Apex fidelity, and production issue-set qualification remain open.",
    },
    "ARCH-SCH-002": {
        "acceptance": "Editable schedule cells generate normal source-document commands, while calculated cells are read-only and explain their source references.",
        "sources": ("include/sketch/document_schedule_adapter.hpp", "src/core/document_schedule_adapter.cpp", "include/sketch/schedule_model.hpp", "src/core/schedule_model.cpp", "tests/document_schedule_adapter_tests.cpp", "tests/schedule_model_tests.cpp", "src/desktop/main_window.cpp", "tests/desktop_smoke.cpp", "docs/schedules.md"),
        "anchors": ("make_schedule_edit", "editable", "Calculated schedule cell is read-only", "source", "revision", "undo"),
        "tests": ("document_schedule_adapter", "schedule_model", "desktop_workflow"),
        "qualification_boundary": "This evidence covers local schedule edit fencing and read-only calculations. Production schedule standards, external integrations, and complete output certification remain open.",
    },
    "ARCH-SHEET-001": {
        "acceptance": "Sheets persist title blocks, revisions, callouts, schedules, coordinated viewports, and independent viewport scales with graph validation.",
        "sources": ("include/sketch/sheet_view_model.hpp", "src/core/sheet_view_model.cpp", "tests/sheet_view_model_tests.cpp", "include/sketch/sheet_view_entity_codec.hpp", "src/core/sheet_view_entity_codec.cpp", "tests/sheet_view_entity_codec_tests.cpp", "src/desktop/main_window.cpp", "tests/desktop_smoke.cpp", "docs/sheets-views.md"),
        "anchors": ("DrawingSheet", "title_block", "revisions", "callouts", "schedules", "viewports", "scale_denominator"),
        "tests": ("sheet_view_model", "sheet_view_entity_codec", "sheet_output_scene", "desktop_workflow"),
        "qualification_boundary": "This evidence covers the local typed sheet graph and desktop output path. Interactive layout ergonomics, printer calibration, and complete permit-set qualification remain open.",
    },
    "ARCH-EDIT-001": {
        "acceptance": "Supported architectural entities share creation, selection, property editing, transforms, duplication, deletion, undo/redo, and save/reopen command contracts.",
        "sources": ("include/sketch/architectural_workflow_contract.hpp", "src/core/architectural_workflow_contract.cpp", "include/sketch/architectural_document_adapter.hpp", "src/core/architectural_document_adapter.cpp", "include/sketch/desktop/main_window.hpp", "src/desktop/main_window.cpp", "tests/architectural_workflow_contract_tests.cpp", "tests/architectural_document_adapter_tests.cpp", "tests/desktop_smoke.cpp", "docs/architectural-workflow-contract.md"),
        "anchors": ("ArchitecturalAction", "property_edit", "transform", "duplicate", "erase", "revision", "undo", "save"),
        "tests": ("architectural_workflow_contract", "architectural_document_adapter", "desktop_workflow"),
        "qualification_boundary": "This evidence covers the shared local command contract and supported desktop objects. Complete object-by-object production certification and native Apex parity remain open.",
    },
    "ARCH-REL-001": {
        "acceptance": "Typed relationship graphs identify stable object references, validate relationship kinds and states, and preserve explicit links across views and output adapters.",
        "sources": ("include/sketch/typed_relationships.hpp", "src/core/typed_relationships.cpp", "docs/typed-relationships.md"),
        "anchors": ("TypedRelationship", "RelationshipKind", "RelationshipState", "stable", "reference", "validate"),
        "tests": ("typed_relationships",),
        "qualification_boundary": "This evidence covers the deterministic typed relationship graph. Full production cross-view certification and native Apex relationship mapping remain open.",
    },
    "ARCH-OUTPUT-001": {
        "acceptance": "Residential and light-commercial architectural workflow contracts cover shared semantic objects, plans, elevations, sections, 3D, schedules, sheets, alternatives, and save/reopen output requirements.",
        "sources": ("include/sketch/architectural_workflow_contract.hpp", "src/core/architectural_workflow_contract.cpp", "tests/architectural_workflow_contract_tests.cpp", "include/sketch/architectural_document_adapter.hpp", "src/core/architectural_document_adapter.cpp", "src/desktop/main_window.cpp", "src/desktop/plan_canvas.cpp", "src/architecture/architectural_schedule.cpp", "src/core/sheet_view_model.cpp", "tests/architectural_document_adapter_tests.cpp", "tests/architectural_schedule_tests.cpp", "tests/sheet_view_model_tests.cpp", "tests/desktop_smoke.cpp", "docs/architectural-workflow-contract.md", "docs/sheets-views.md", "docs/desktop-workflow.md"),
        "anchors": ("ArchitecturalOutputKind", "plan", "elevation", "section", "view_3d", "schedule", "sheet", "issue_revision"),
        "tests": ("architectural_workflow_contract", "architectural_document_adapter", "architectural_schedule", "sheet_view_model", "desktop_workflow"),
        "qualification_boundary": "This evidence covers the shared local architectural workflow foundation. Complete integrated residential/commercial fixtures, physical output, and the unified production gate remain open.",
    },
    "UX-WORK-001": {
        "acceptance": "The Windows workspace exposes a compact canvas-centered shell with navigator, tool rail, contextual inspector, measurement readout, and architectural 3D pane.",
        "sources": ("src/desktop/main_window.cpp", "tests/desktop_smoke.cpp", "docs/workspace-ui.md"),
        "anchors": ("workspaceSplitter", "workspaceTabs", "tool rail", "navigator", "inspector", "measurement", "canvas"),
        "tests": ("desktop_workflow", "workspace_navigation"),
        "qualification_boundary": "This evidence covers the local desktop shell and its deterministic widget contract. Visual acceptance across all Windows themes, DPI modes, and production hardware remains open.",
    },
    "UX-WORK-002": {
        "acceptance": "Selection drives contextual dimensions, relationships, styling, calibration, classifications, and calculation details through the inspector and related command surfaces.",
        "sources": ("src/desktop/main_window.cpp", "tests/desktop_smoke.cpp", "docs/workspace-ui.md"),
        "anchors": ("selected", "inspector", "dimension", "calibration", "classification", "calculation", "contextual"),
        "tests": ("desktop_workflow",),
        "qualification_boundary": "This evidence covers local contextual selection behavior. Full usability review, physical input ergonomics, and production accessibility qualification remain open.",
    },
    "UX-WORK-003": {
        "acceptance": "Split plan/3D views, workspace tabs, saved profiles, light/dark themes, and high-contrast field presentation persist and apply locally.",
        "sources": ("src/desktop/main_window.cpp", "src/visualization/native_model_view.cpp", "tests/desktop_smoke.cpp", "docs/workspace-ui.md", "docs/workspace-accessibility.md"),
        "anchors": ("workspaceTabs", "NativeModelView", "WorkspaceTheme", "high_contrast", "dark", "light", "workspace profiles"),
        "tests": ("desktop_workflow", "workspace_navigation", "visibility_workflow"),
        "qualification_boundary": "This evidence covers local workspace presentation and persistence. Full GPU, DPI, accessibility, and clean-machine qualification remain open.",
    },
    "UX-ACCESS-001": {
        "acceptance": "The accessibility profile validates keyboard navigation, predictable focus, accessible properties, high contrast, themes, and declared 100%, 150%, and 200% layouts.",
        "sources": ("include/sketch/workspace_accessibility.hpp", "src/core/workspace_accessibility.cpp", "tests/workspace_accessibility_tests.cpp", "docs/workspace-accessibility.md", "scripts/production_qualification.py", "docs/production-qualification.md"),
        "anchors": ("keyboard_navigation", "predictable_focus", "accessible_properties", "high_contrast", "DpiLayoutQualification", "150", "200"),
        "tests": ("workspace_accessibility", "desktop_workflow"),
        "qualification_boundary": "This evidence covers the local accessibility contract and declared layouts. Physical screen-reader, keyboard-only, DPI, and assistive-technology qualification remain open.",
    },
    "UX-INPUT-001": {
        "acceptance": "Pen/touch controls and the on-screen measurement keypad share the keyboard precision command path and retain pressure-independent undoable edits.",
        "sources": ("include/sketch/workspace_accessibility.hpp", "src/core/workspace_accessibility.cpp", "tests/workspace_accessibility_tests.cpp", "src/desktop/main_window.cpp", "tests/desktop_smoke.cpp", "docs/workspace-accessibility.md"),
        "anchors": ("pen_controls", "touch_controls", "measurement_keypad", "keyboard_navigation", "keypad", "pressure", "undo"),
        "tests": ("workspace_accessibility", "desktop_workflow"),
        "qualification_boundary": "This evidence covers synthetic pen/touch event routing and keypad semantics. Real devices, multi-touch ergonomics, and production field qualification remain open.",
    },
})


WORKFLOW_RULES.update({
    "ASSIST-001": {
        "acceptance": "Optional local assistance proposes deterministic raster and connected-component traces with source identity and an explicit unverified state.",
        "sources": ("include/sketch/assistance_contract.hpp", "include/sketch/assistance_engine.hpp", "src/core/assistance_contract.cpp", "src/core/assistance_engine.cpp", "tests/assistance_contract_tests.cpp", "tests/assistance_engine_tests.cpp", "tests/assistance_workflow_tests.cpp", "docs/assistance-contract.md", "assets/assistance/deterministic-engine-v1.json"),
        "anchors": ("AssistanceKind", "suggest_tracing", "edge_tracing", "proposal", "unverified", "offline"),
        "tests": ("assistance_contract", "assistance_engine", "assistance_workflow"),
        "qualification_boundary": "This evidence covers the deterministic local tracing engine and proposal envelope. Production plan-image diversity and visual qualification remain open.",
    },
    "ASSIST-002": {
        "acceptance": "Optional local assistance extracts explicit-unit dimensions while retaining the original text, parsed units, and typed proposal provenance.",
        "sources": ("include/sketch/assistance_contract.hpp", "include/sketch/assistance_engine.hpp", "src/core/assistance_contract.cpp", "src/core/assistance_engine.cpp", "tests/assistance_contract_tests.cpp", "tests/assistance_engine_tests.cpp", "tests/assistance_workflow_tests.cpp", "docs/assistance-contract.md", "assets/assistance/deterministic-engine-v1.json"),
        "anchors": ("extract_dimensions", "dimension_extraction", "original_text", "length_metres", "unit", "provenance"),
        "tests": ("assistance_contract", "assistance_engine", "assistance_workflow"),
        "qualification_boundary": "This evidence covers explicit-unit extraction from deterministic fixture text. Real plan OCR, ambiguous labels, and production accuracy qualification remain open.",
    },
    "ASSIST-003": {
        "acceptance": "Optional assisted label placement and bounded natural-language commands produce typed proposals that use the same permission, command, and undo path as manual edits.",
        "sources": ("include/sketch/assistance_contract.hpp", "include/sketch/assistance_engine.hpp", "src/core/assistance_contract.cpp", "src/core/assistance_engine.cpp", "tests/assistance_contract_tests.cpp", "tests/assistance_engine_tests.cpp", "tests/assistance_workflow_tests.cpp", "docs/assistance-contract.md", "assets/assistance/deterministic-engine-v1.json"),
        "anchors": ("suggest_label_placements", "parse_natural_language", "add_label", "permission", "command", "undo"),
        "tests": ("assistance_contract", "assistance_engine", "assistance_workflow"),
        "qualification_boundary": "This evidence covers the bounded local label/language grammar and command integration. Broader language coverage and production UX review remain open.",
    },
    "ASSIST-004": {
        "acceptance": "Assistance proposals remain visibly unverified until accepted, cannot silently change measurements or classifications, and disabling assistance leaves deterministic workflows unchanged.",
        "sources": ("include/sketch/assistance_contract.hpp", "include/sketch/assistance_engine.hpp", "src/core/assistance_contract.cpp", "src/core/assistance_engine.cpp", "tests/assistance_contract_tests.cpp", "tests/assistance_engine_tests.cpp", "tests/assistance_workflow_tests.cpp", "docs/assistance-contract.md", "assets/assistance/deterministic-engine-v1.json"),
        "anchors": ("AssistanceSession", "requires_permission_check", "requires_undo_transaction", "disabled", "acceptance", "classification", "unverified"),
        "tests": ("assistance_contract", "assistance_engine", "assistance_workflow"),
        "qualification_boundary": "This evidence covers local fail-closed proposal acceptance and the assistance-disabled path. End-user review and production workflow qualification remain open.",
    },
    "ASSIST-005": {
        "acceptance": "Assistance assets declare local resource paths, provenance, and licenses, and the deterministic engine ships without third-party model weights.",
        "sources": ("include/sketch/assistance_contract.hpp", "include/sketch/assistance_engine.hpp", "src/core/assistance_contract.cpp", "src/core/assistance_engine.cpp", "tests/assistance_contract_tests.cpp", "tests/assistance_engine_tests.cpp", "tests/assistance_workflow_tests.cpp", "docs/assistance-contract.md", "assets/assistance/deterministic-engine-v1.json"),
        "anchors": ("deterministic-engine-v1", "provenance", "license", "resources", "third-party", "offline"),
        "tests": ("assistance_contract", "assistance_engine", "assistance_workflow"),
        "qualification_boundary": "This evidence covers the declared local assistance inventory and provenance contract. Final legal review and distribution approval remain open.",
    },
    "REC-001": {
        "acceptance": "Autosave fixtures distinguish edited, autosaved, and saved generations, debounce quiet periods, retry failures, and preserve recovery state through queued local saves.",
        "sources": ("include/sketch/workspace_autosave_scheduler.hpp", "src/core/workspace_autosave_scheduler.cpp", "include/sketch/workspace_save_queue.hpp", "src/core/workspace_save_queue.cpp", "src/desktop/main_window.cpp", "tests/desktop_workspace_recovery_tests.cpp", "tests/workspace_autosave_scheduler_tests.cpp", "tests/workspace_save_queue_tests.cpp", "include/sketch/recovery_discovery.hpp", "src/core/recovery_discovery.cpp", "tests/recovery_discovery_tests.cpp", "docs/recovery-discovery.md", "docs/workspace-autosave-scheduler.md", "docs/workspace-save-queue.md"),
        "anchors": ("WorkspaceAutosaveScheduler", "edited_generation", "autosaved_checkpoint_generation", "saved", "recovery", "capture", "retry"),
        "tests": ("workspace_autosave_scheduler", "workspace_save_queue", "recovery_discovery", "desktop_workspace_recovery"),
        "qualification_boundary": "This evidence covers deterministic local autosave scheduling, queued snapshots, and recovery discovery. Power-loss and clean-machine fault injection remain open.",
    },
    "REC-002": {
        "acceptance": "Named revision and comparison fixtures retain semantic, geometric, calculation, and presentation history and restore selected revisions without contaminating later work.",
        "sources": ("src/core/document.cpp", "src/desktop/main_window.cpp", "include/sketch/desktop/main_window.hpp", "tests/desktop_smoke.cpp", "docs/desktop-workflow.md"),
        "anchors": ("named revision", "restore", "compare", "comparison", "revision", "history", "undo"),
        "tests": ("desktop_workflow", "document_commands"),
        "qualification_boundary": "This evidence covers local named revisions, comparison controls, and history restoration. Production project-scale comparison and native Apex compatibility remain open.",
    },
    "REC-003": {
        "acceptance": "Portable project packages and templates include required project assets, profiles, documentation references, manifests, and a verified restore path.",
        "sources": ("scripts/stage_project_package.py", "tests/test_stage_project_package.py", "docs/dependencies/project-package.md", "scripts/stage_portable_package.py", "tests/test_stage_portable_package.py", "docs/dependencies/portable-package.md", "include/sketch/project_resource_catalog.hpp", "src/core/project_resource_catalog.cpp", "tests/project_resource_catalog_tests.cpp", "include/sketch/desktop/main_window.hpp", "src/desktop/main_window.cpp", "tests/project_resource_desktop_tests.cpp", "src/cli/main.cpp", "tests/test_cli.py"),
        "anchors": ("portable", "template", "assets", "profiles", "documentation", "manifest", "restore"),
        "tests": ("packaging_stage_project_package", "packaging_stage_portable_package", "project_resource_catalog", "project_resource_desktop", "project_cli"),
        "qualification_boundary": "This evidence covers local package/template staging, integrity, and restore. Clean-machine handoff and broad legacy migration qualification remain open.",
    },
    "REC-005": {
        "acceptance": "Failure-injection fixtures reject transaction, serialization, flush, replacement, recovery, migration, lock, damaged-asset, stale-recovery, and newer-schema failures without presenting corrupted authoritative state.",
        "sources": ("include/sketch/project_store.hpp", "src/core/project_store.cpp", "tests/project_store_tests.cpp", "tests/recovery_ledger_tests.cpp", "tests/recovery_discovery_tests.cpp", "tests/desktop_workspace_recovery_tests.cpp", "docs/recovery-ledger.md", "docs/recovery-discovery.md"),
        "anchors": ("Fault", "disk", "flush", "replacement", "recovery", "corrupt", "mismatch", "schema"),
        "tests": ("project_storage", "recovery_ledger", "recovery_discovery", "desktop_workspace_recovery"),
        "qualification_boundary": "This evidence covers deterministic injected failure handling and recovery ledgers. Physical disk exhaustion, power-loss timing, and production failure-injection runs remain open.",
    },
    "IO-IFC-001": {
        "acceptance": "The declared IFC4 STEP subset exports supported walls, slabs, openings, relationships, units, and Vertex property preservation, and reconstructs reliable typed entities while reporting unsupported content.",
        "sources": ("include/sketch/interchange_profile.hpp", "src/core/interchange_profile.cpp", "tests/interchange_profile_tests.cpp", "include/sketch/ifc_project_exchange.hpp", "src/core/ifc_project_exchange.cpp", "tests/ifc_project_exchange_tests.cpp", "tests/ifc_desktop_workflow_tests.cpp", "docs/interchange-profiles.md"),
        "anchors": ("IFC4", "IFCWALL", "IFCSLAB", "IFCOPENINGELEMENT", "Pset_VertexExchange_v1", "reconstruct", "unsupported"),
        "tests": ("interchange_profile", "ifc_project_exchange", "ifc_desktop_workflow"),
        "qualification_boundary": "This evidence covers the bounded local IFC4 exchange subset and diagnostics. External IFC consumers, full MVD fidelity, and production interoperability remain open.",
    },
    "IO-DXF-001": {
        "acceptance": "The declared DXF R2013 subset round-trips lines, arcs, polylines, text, dimensions, hatches, blocks, and inserts with units and explicit unsupported-feature diagnostics.",
        "sources": ("include/sketch/interchange_profile.hpp", "src/core/interchange_profile.cpp", "tests/interchange_profile_tests.cpp", "include/sketch/dxf_exchange.hpp", "src/core/dxf_exchange.cpp", "tests/dxf_exchange_tests.cpp", "include/sketch/dxf_project_exchange.hpp", "src/core/dxf_project_exchange.cpp", "tests/dxf_project_exchange_tests.cpp", "tests/dxf_desktop_workflow_tests.cpp", "docs/interchange-profiles.md"),
        "anchors": ("R2013", "LINE", "ARC", "LWPOLYLINE", "HATCH", "BLOCK", "INSERT", "unsupported", "units"),
        "tests": ("interchange_profile", "dxf_exchange", "dxf_project_exchange", "dxf_desktop_workflow"),
        "qualification_boundary": "This evidence covers the bounded local DXF R2013 subset and desktop exchange path. External CAD consumer comparison and complete production fidelity remain open.",
    },
    "IO-PDF-001": {
        "acceptance": "PDF and raster references persist source identity, page selection, calibration, transforms, and explicit traceable versus editable fidelity modes.",
        "sources": ("include/sketch/reference_asset.hpp", "src/core/reference_asset.cpp", "tests/reference_asset_tests.cpp", "src/desktop/main_window.cpp", "tests/desktop_smoke.cpp", "docs/reference-assets.md"),
        "anchors": ("ReferenceAsset", "editable_extraction", "traceable", "calibration", "source_preserved", "page", "fidelity"),
        "tests": ("reference_assets", "desktop_workflow"),
        "qualification_boundary": "This evidence covers local reference asset semantics and calibration. Editable PDF extraction and production visual qualification remain open.",
    },
    "IO-OUTPUT-001": {
        "acceptance": "Preview, PDF, SVG, and printing use one shared vector scene, while raster imagery and shaded 3D are identified as raster content with bound output fingerprints.",
        "sources": ("include/sketch/sheet_output_scene.hpp", "src/core/sheet_output_scene.cpp", "src/desktop/plan_canvas.cpp", "src/desktop/main_window.cpp", "tests/desktop_smoke.cpp", "tests/output_fingerprint_tests.cpp", "docs/output-fingerprint.md", "docs/desktop-workflow.md"),
        "anchors": ("SheetOutputScene", "vector", "raster", "print", "PDF", "SVG", "fingerprint"),
        "tests": ("sheet_output_scene", "output_fingerprint", "desktop_workflow"),
        "qualification_boundary": "This evidence covers the shared local scene and output receipts. Physical printer calibration and production output comparison remain open.",
    },
    "IO-OUTPUT-002": {
        "acceptance": "Letter, Legal, Tabloid, A4, A3, and architectural sheet definitions persist independent scales and map numeric page dimensions consistently into PDF, SVG, preview, and print requests.",
        "sources": ("include/sketch/sheet_view_model.hpp", "src/core/sheet_view_model.cpp", "tests/sheet_view_model_tests.cpp", "docs/sheets-views.md", "src/desktop/main_window.cpp", "tests/desktop_smoke.cpp"),
        "anchors": ("Letter", "Legal", "Tabloid", "A4", "A3", "scale_denominator", "printer", "page"),
        "tests": ("sheet_view_model", "sheet_output_scene", "desktop_workflow"),
        "qualification_boundary": "This evidence covers numeric local page and scale semantics. Driver-specific scaling and physical printer calibration remain open.",
    },
    "IO-OUTPUT-003": {
        "acceptance": "Stale calculations, missing assets, broken references, invalid geometry, or mismatched fingerprints block authoritative output or apply a visible draft mark before atomic publication.",
        "sources": ("src/desktop/main_window.cpp", "src/core/output_fingerprint.cpp", "tests/desktop_smoke.cpp", "tests/output_fingerprint_tests.cpp", "docs/output-fingerprint.md", "docs/desktop-workflow.md"),
        "anchors": ("stale", "fingerprint", "invalid", "draft", "authoritative", "missing", "print preview"),
        "tests": ("output_fingerprint", "desktop_workflow"),
        "qualification_boundary": "This evidence covers local output gating and draft stamping. Full integrated stale-input fixtures and production authority qualification remain open.",
    },
    "SEC-WORKER-001": {
        "acceptance": "The import-worker policy declares AppContainer isolation, network denial, Job Object limits, brokered inputs, controlled temporary storage, fixed search paths, and attestation requirements.",
        "sources": ("include/sketch/import_worker_policy.hpp", "src/core/import_worker_policy.cpp", "tests/import_worker_policy_tests.cpp", "include/sketch/windows_import_worker.hpp", "src/core/windows_import_worker.cpp", "tests/windows_import_worker_tests.cpp", "tests/windows_import_worker_probe.cpp", "docs/import-worker-security.md", "scripts/test-import-worker-independent.ps1", "scripts/test-import-worker-independent.md"),
        "anchors": ("AppContainer", "network", "Job", "attestation", "temporary", "search", "limits"),
        "tests": ("import_worker_policy",),
        "qualification_boundary": "This evidence covers deterministic worker policy, path, resource, and attestation contracts and the source of a job-free development-host runner. Its local captures are not embedded here; active network, parent-exit, installed-runtime, and clean-machine qualification remain open.",
    },
    "SEC-WORKER-002": {
        "acceptance": "Malformed, traversal, decompression, module-planting, child-process, timeout, and crash fixtures fail closed with bounded diagnostics and no partial import result.",
        "sources": ("include/sketch/import_worker_policy.hpp", "src/core/import_worker_policy.cpp", "tests/import_worker_policy_tests.cpp", "include/sketch/windows_import_worker.hpp", "src/core/windows_import_worker.cpp", "tests/windows_import_worker_tests.cpp", "tests/windows_import_worker_probe.cpp", "docs/import-worker-security.md", "scripts/test-import-worker-independent.ps1", "scripts/test-import-worker-independent.md"),
        "anchors": ("traversal", "decompression", "timeout", "crash", "module", "partial", "Job"),
        "tests": ("import_worker_policy",),
        "qualification_boundary": "This evidence covers deterministic adversarial policy decisions, bounded output rules, and the source of a job-free development-host runner. Decoder crash, module planting, child escape, project survival, installed-runtime, and clean-machine qualification remain open.",
    },
    "SEC-PROJ-001": {
        "acceptance": "Georeferencing and import-worker policy disable PROJ networking, require contained local resources, and report missing or mismatched resources locally without download attempts.",
        "sources": ("include/sketch/import_worker_policy.hpp", "src/core/import_worker_policy.cpp", "tests/import_worker_policy_tests.cpp", "include/sketch/windows_import_worker.hpp", "src/core/windows_import_worker.cpp", "tests/windows_import_worker_tests.cpp", "tests/windows_import_worker_probe.cpp", "include/sketch/georeferencing_runtime.hpp", "src/core/georeferencing_runtime.cpp", "tests/georeferencing_runtime_tests.cpp", "docs/import-worker-security.md", "docs/survey-georeferencing-contracts.md"),
        "anchors": ("PROJ_NETWORK", "OFF", "network", "resource", "contained", "diagnostic", "missing"),
        "tests": ("import_worker_policy", "georeferencing_runtime"),
        "qualification_boundary": "This evidence covers local PROJ policy and resource validation. Externally monitored network-denied worker execution remains open.",
    },
    "OPS-QA-002": {
        "acceptance": (
            "The residential production fixture completes measurement, architectural authoring, "
            "editable native solids, coordinated views, alternatives, schedules, sheets, symbol "
            "resizing, save/reopen, recovery history, and PDF/SVG/raster output with stable semantic state."
        ),
        "sources": (
            "include/sketch/desktop/main_window.hpp",
            "src/desktop/main_window.cpp",
            "include/sketch/building_entity.hpp",
            "src/core/architectural_document_adapter.cpp",
            "tests/desktop_smoke.cpp",
            "docs/production-plan.md",
            "docs/desktop-workflow.md",
        ),
        "anchors": (
            "production fixture",
            "Workspace::measurement",
            "calculationBaseArea",
            "createRoomVolumeFromBoundary",
            "native solids",
            "transformSelectedArchitecturalObject",
            "symbol catalog",
            "named recoverable revision",
            "saveProjectAs",
            "openProject",
            "exportDraftPdf",
            "exportDraftSvg",
        ),
        "tests": ("desktop_workflow",),
        "qualification_boundary": (
            "This evidence covers the deterministic residential end-to-end fixture in Debug and "
            "Release, including shared measurement/architectural semantics and local output. It "
            "does not certify native Apex files, physical devices, clean-machine networking denial, "
            "or the unified production gate."
        ),
    },
    "OPS-QA-003": {
        "acceptance": (
            "The light-commercial production fixture completes measurement, multiple architectural "
            "object families, native solid generation, coordinated views, alternatives, schedules, "
            "sheets, commercial symbols, resizing, save/reopen, recovery history, and local output."
        ),
        "sources": (
            "include/sketch/desktop/main_window.hpp",
            "src/desktop/main_window.cpp",
            "include/sketch/building_entity.hpp",
            "src/core/architectural_document_adapter.cpp",
            "tests/desktop_smoke.cpp",
            "docs/production-plan.md",
            "docs/desktop-workflow.md",
        ),
        "anchors": (
            "production fixture",
            "light-commercial",
            "Workspace::architectural",
            "createRoomVolumeFromBoundary",
            "native solids",
            "Beam",
            "StairFlight",
            "checkout-counter",
            "symbol catalog",
            "named recoverable revision",
            "saveProjectAs",
            "openProject",
            "exportDraftPdf",
            "exportDraftSvg",
        ),
        "tests": ("desktop_workflow",),
        "qualification_boundary": (
            "This evidence covers the deterministic light-commercial end-to-end fixture in Debug "
            "and Release, including structural objects, commercial symbols, coordinated views, and "
            "local output. It does not certify native Apex files, physical devices, clean-machine "
            "networking denial, or the unified production gate."
        ),
    },
})


def _load_module(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _path_record(root: pathlib.Path, relative: str) -> dict[str, Any]:
    path = (root / relative).resolve()
    if not path.is_relative_to(root) or not path.is_file():
        raise RuntimeError(f"required evidence path is missing or outside the source tree: {relative}")
    return {
        "path": relative.replace("\\", "/"),
        "sha256": _sha256(path),
        "size": path.stat().st_size,
    }


def _workflow_test_records(root: pathlib.Path, configuration: str, test_names: tuple[str, ...], audit,
                           log_path: pathlib.Path | None = None):
    build = root / "build" / configuration
    inventory_path = build / "CTestTestfile.cmake"
    log_path = log_path or build / "Testing" / "Temporary" / "LastTest.log"
    inventory, inventory_errors = audit._ctest_inventory(build)
    if inventory is None:
        raise RuntimeError(f"{configuration}: invalid CTest inventory: {inventory_errors}")
    if (inventory["source_directory"] is None or inventory["build_directory"] is None or
            inventory["source_directory"].resolve() != root or
            inventory["build_directory"].resolve() != build.resolve()):
        raise RuntimeError(f"{configuration}: CTest inventory source/build directory mismatch")
    if not log_path.is_file():
        raise RuntimeError(f"{configuration}: CTest log is missing")
    try:
        log_text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        raise RuntimeError(f"{configuration}: CTest log could not be read: {error}") from error
    if "End testing:" not in log_text:
        raise RuntimeError(f"{configuration}: CTest log is incomplete")
    blocks = audit._test_log_blocks(log_text)
    by_name: dict[str, list[dict[str, Any]]] = {}
    for block in blocks:
        by_name.setdefault(block["name"], []).append(block)

    records = []
    for test_name in test_names:
        properties = inventory["tests"].get(test_name)
        if properties is None:
            raise RuntimeError(f"{configuration}: workflow test is absent from CTest inventory: {test_name}")
        if properties["disabled"] or properties["skip_return_code"]:
            raise RuntimeError(f"{configuration}: workflow test is disabled or skippable: {test_name}")
        entries = by_name.get(test_name, [])
        if len(entries) != 1 or entries[0]["status"] != "passed":
            status = entries[0]["status"] if len(entries) == 1 else "missing_or_duplicate"
            raise RuntimeError(f"{configuration}: workflow test did not pass exactly once: {test_name} ({status})")
        if not entries[0]["directory"] or pathlib.Path(entries[0]["directory"]).resolve() != build.resolve():
            raise RuntimeError(f"{configuration}: workflow test execution directory mismatch: {test_name}")
        records.append({"name": test_name, "status": "passed"})

    return {
        "configuration": configuration,
        "inventory": _path_record(root, f"build/{configuration}/CTestTestfile.cmake"),
        "log": _path_record(root, log_path.relative_to(root).as_posix()),
        "tests": records,
    }


def build_evidence(root: pathlib.Path) -> dict[str, dict[str, Any]]:
    root = root.resolve()
    audit = _load_module("vertex_completion_audit_for_workflow", SCRIPT_DIR / "completion_audit.py")
    requirement_audit = _load_module("vertex_requirement_audit_for_workflow", SCRIPT_DIR / "requirement_audit.py")
    provenance = _load_module("vertex_workflow_test_provenance", SCRIPT_DIR / "workflow_test_provenance.py")
    source_fingerprint = requirement_audit.source_fingerprint(root)
    for configuration in ("windows-debug", "windows-release"):
        provenance.verify(root, configuration, source_fingerprint)
    result: dict[str, dict[str, Any]] = {}

    for requirement_id, rule in WORKFLOW_RULES.items():
        source_records = []
        source_text = []
        for relative in rule["sources"]:
            record = _path_record(root, relative)
            source_records.append(record)
            source_text.append((root / relative).read_text(encoding="utf-8", errors="replace"))
        combined_source = "\n".join(source_text).casefold()
        missing_anchors = [anchor for anchor in rule["anchors"] if anchor.casefold() not in combined_source]
        if missing_anchors:
            raise RuntimeError(f"{requirement_id}: source anchors are missing: {missing_anchors}")

        ctest = [
            _workflow_test_records(root, configuration, rule["tests"], audit,
                root / "build" / configuration / provenance.PROVENANCE_LOG_FILE)
            for configuration in ("windows-debug", "windows-release")
        ]
        for test_record in ctest:
            test_record["provenance"] = _path_record(
                root, f"build/{test_record['configuration']}/{provenance.PROVENANCE_FILE}")
        result[requirement_id] = {
            "schema_version": 1,
            "requirement_id": requirement_id,
            "result": "pass",
            "source_tree_sha256": source_fingerprint,
            "acceptance": rule["acceptance"],
            "source_files": source_records,
            "source_anchors": list(rule["anchors"]),
            "tests": list(rule["tests"]),
            "ctest": ctest,
            "qualification_boundary": rule["qualification_boundary"],
        }
    if requirement_audit.source_fingerprint(root) != source_fingerprint:
        raise RuntimeError("source changed while generating workflow evidence")
    for configuration in ("windows-debug", "windows-release"):
        provenance.verify(root, configuration, source_fingerprint)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[1])
    parser.add_argument("--output-directory", type=pathlib.Path, default=None)
    args = parser.parse_args()
    root = args.root.resolve()
    output_directory = (args.output_directory or root / "artifacts/acceptance/workflows").resolve()
    if not output_directory.is_relative_to(root):
        raise SystemExit("output directory must remain inside the repository root")
    output_directory.mkdir(parents=True, exist_ok=True)
    evidence = build_evidence(root)
    for requirement_id, record in evidence.items():
        output = output_directory / f"{requirement_id.lower()}.json"
        output.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n",
                          encoding="utf-8", newline="\n")
        print(f"Wrote {output} ({requirement_id}; source {record['source_tree_sha256']})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
