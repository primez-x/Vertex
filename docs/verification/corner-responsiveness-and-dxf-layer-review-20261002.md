# Corner responsiveness and DXF layer review

This is focused implementation evidence for the existing production scope. It
does not certify the full replacement release, external interoperability or
ANSI approval. User-observed acceptance remains pending.

## Implemented behavior

Corner previews now resolve level-bound walls through a batch-local project
organization. No result is cached across snapshots. Derived placements match
the singular path; invalid multi-wall input retains the original first
diagnostic. Contact, topology, coordinated edit and appraisal checks remain.

On the same 366-wall fixture (360 unrelated level-bound walls), the baseline
preview was 2191.2 ms. The reviewed build measured 130.295 ms, approximately 94%
less time. This is a development-machine observation, not a reference-hardware
frame-rate qualification. Pairwise contact scanning remains unchanged.

Tools > Import DXF now reviews every editable source CAD layer before applying
changes. Each source layer can use an existing project layer or create a
source-named layer on the active floor. Text, dimensions, plan geometry and
verified native wall/opening graphs receive the chosen destinations. Original
coordinates, elevations and source bytes remain intact. A source receipt records
the mapping. New layers and imported content share one Undo/Redo transaction;
cancelled or stale reviews do not mutate the project.

Hosted doors/windows may use a different drawing layer on their host wall's
floor. Physical hosting and world placement still follow the wall. Their own
layer and the shared floor control visibility in 2D and native 3D.

The review shares one destination model and creates editors on demand, verified
with 240 source layers and 120 destination layers. It explicitly identifies
source-only imports without inventing editable geometry.

Actual worker testing exposed a pre-existing dimension fidelity defect. Export
now places extension points and rotation in the correct DXF subclasses, matching
the [Autodesk linear/rotated dimension definition](https://help.autodesk.com/cloudhelp/2024/ENU/AutoCAD-DXF/files/GUID-F0004556-493C-48D5-8619-61D6ADF05C04.htm).
Normalization preserves absence of style/version/attachment fields instead of
introducing library defaults. Explicit source fields still reach the unchanged
strict parser. Unsupported styles and associativity remain disclosed limitations.

## Verification

- Release build completed. Thirteen affected core executables passed, including
  exact placement equivalence, first-error preservation, organization, visibility,
  constraints, wall measurement, document/store/digest and DXF exchange.
- Seven affected desktop executables and native geometry preparation passed:
  corner/vertex editing, connected-wall canvas, wall measurement, appraisal,
  Details panel and visibility. The native preparation fixture checks a meshed
  window on a separate layer and shared-floor hiding.
- The actual isolated import runner passed all five fixtures: Windows broker,
  assistance, DXF desktop workflow, IFC desktop workflow and CAD library worker.
  The host was outside a parent job; no fixture skipped, timed out or lost output.
- Fifteen real-library DXF adapter tests passed, including absent versus explicit
  dimension metadata and preserved coordinates.
- Independent source review covered snapshot lifetime, placement semantics,
  import atomicity, identity collisions, source lineage, shared-model allocation,
  hosted-opening context/visibility and dimension normalization. Required findings
  were corrected. Root reviewed the integrated changes and native dialog capture.

Ignored local evidence is under `artifacts/corner-preview-performance-20261002`.
The successful import capture is
`artifacts/import-worker-independent/release-69b4cccfa58542cd90d70b027f946ebf`.
Earlier failed runs remain as historical evidence. Two regression fixtures were
corrected to exercise the actual annotation owner and the derived organization
resolver rather than being rejected by earlier canonical-reference admission.

The release audit still reports production acceptance false and 208 unresolved
release-evidence gaps. Apex native-file compatibility, normative measurement
validation and full third-party certification remain open. This delivery does
not reduce the agreed final release requirements.
