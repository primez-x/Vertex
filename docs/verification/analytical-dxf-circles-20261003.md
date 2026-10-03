# Analytical DXF circles and annotation-layer retention

The baseline Release project-mapping fixture failed on a supported planar
CIRCLE being omitted. The added codec and mapper now retain circles as two
analytical semicircles and export annotation children on their assigned layers.
This is original 2D exchange scope, not native Apex compatibility certification.

Independent review found expanded INSERT amplification and unrepresentable
small circles at extreme coordinates. Checked work budgets now run before
candidate creation or native graph activation; circle admission checks final
native geometry against intended center and radius. A second regression
observed refusal of a valid recentered/upscaled block circle before repair.
INSERT reconstruction now constructs final-coordinate endpoints directly,
preserving rotation, uniform signed scale and reflection orientation.

The current full Release `dxf_exchange_tests` and `dxf_project_exchange_tests`
both exited 0. They cover hand-written standard records, default OCS, invalid
or unsupported fields, main/block counts, all six supported unit families,
exact analytical areas/perimeters, base points, uniform/reflected INSERTs,
nonuniform diagnostics, explicit topology upgrade and editing, geometric DXF
round trips, native-block extra-circle refusal, annotation layer names and
broken-reference diagnostics, exact-limit and excessive mixed block expansion,
direct/final-placement collapse and valid upscale/recentering controls.

Native desktop verification is recorded separately below after its terminal
result. Runtime artifacts are under `artifacts/analytical-dxf-circles-20261003`.
The development harness's parent job and a writable copied runtime correctly
refused isolated import. These refusals are not successful-import evidence.
The fixture uses a task-owned runtime copy protected with the existing installer
module ACL policy, and a hidden independent Windows process; no sandbox guard
is bypassed. The original installed runtime and unrelated files are preserved.

The first complete desktop run exposed a production-path omission: the Python
library normalizer still filtered CIRCLE before the strict mapper. Its supported
set now retains circles, and the adapter suite covers ASCII/binary geometry,
units/layers, uniform and reflected blocks, nonuniform diagnostics and retained
tilted/source-nondefault OCS. A foreign reflected copy of a default planar circle
is converted from negative-Z OCS into its equivalent world center and default
normal without changing radius, style or native metadata. The reflected OCS
regression failed before this correction. Independent production-path review
also identified the library's approximate-uniform scale tolerance and repair of
unsupported source normals, elevations or negative radii during transforms.
Exact XY uniformity and original-circle admission now precede acceptance of
virtual copies. Rejected geometry keeps fidelity diagnostics and original bytes.
All 35 adapter tests passed after these corrections; independent review approved
the final scoped production path with no remaining actionable findings.

The final full Release `dxf_desktop_workflow_tests` exited 0 through the real
isolated Windows importer. It covers direct circle layer review, exact retained
source bytes, one-command Undo/Redo, topology upgrade, physical curve-length
editing and Undo, exact curved DXF output and native save/reopen. Independent
block records verify uniform and mirrored circle centers, area and perimeter.
Two unequal-scale controls and four unsupported-source transform controls admit
no editable circle and retain byte-exact source with diagnostics. Existing layer,
annotation, physical-wall and opening exchange cases also pass. The full
`reference_import_tests` suite exited 0 after the final adapter change.

Root inspected `ui/dxf-circle-import.png` from the native window. The curve
renders smoothly against the grid and remains selectable after reopening.
The imported CAD classification still requires an explicit calculation rule;
it does not become a living-area classification automatically.
The runtime inspection found 113 binaries and zero unresolved imports. The
requirement contract has 130 requirements, 10 mandatory gates and zero errors;
the production audit still reports 208 unresolved acceptance entries. These
include missing or stale evidence as well as remaining implementation and
external compatibility work. This workflow does not certify the release.
Source-kit coverage and Git whitespace checks pass.

User checks U362–U364 and `tests/fixtures/dxf/circle-mm.dxf` describe ordinary
import, edit and export tasks. No user-observed result, complete CAD fidelity,
native Apex compatibility, ANSI certification or unified production acceptance
is claimed by this change.
