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


def _workflow_test_records(root: pathlib.Path, configuration: str, test_names: tuple[str, ...], audit):
    build = root / "build" / configuration
    inventory_path = build / "CTestTestfile.cmake"
    log_path = build / "Testing" / "Temporary" / "LastTest.log"
    inventory, inventory_errors = audit._ctest_inventory(build)
    if inventory is None:
        raise RuntimeError(f"{configuration}: invalid CTest inventory: {inventory_errors}")
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
        records.append({"name": test_name, "status": "passed"})

    return {
        "configuration": configuration,
        "inventory": _path_record(root, f"build/{configuration}/CTestTestfile.cmake"),
        "log": _path_record(root, f"build/{configuration}/Testing/Temporary/LastTest.log"),
        "tests": records,
    }


def build_evidence(root: pathlib.Path) -> dict[str, dict[str, Any]]:
    root = root.resolve()
    audit = _load_module("vertex_completion_audit_for_workflow", SCRIPT_DIR / "completion_audit.py")
    requirement_audit = _load_module("vertex_requirement_audit_for_workflow", SCRIPT_DIR / "requirement_audit.py")
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
            _workflow_test_records(root, configuration, rule["tests"], audit)
            for configuration in ("windows-debug", "windows-release")
        ]
        result[requirement_id] = {
            "schema_version": 1,
            "requirement_id": requirement_id,
            "result": "pass",
            "source_tree_sha256": requirement_audit.source_fingerprint(root),
            "acceptance": rule["acceptance"],
            "source_files": source_records,
            "source_anchors": list(rule["anchors"]),
            "tests": list(rule["tests"]),
            "ctest": ctest,
            "qualification_boundary": rule["qualification_boundary"],
        }
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
