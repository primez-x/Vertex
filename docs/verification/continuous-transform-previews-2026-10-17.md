# Continuous drag, rotate, and resize feedback

The reported failures crossed the plan canvas and native 3D view: a pending
geometry job could suppress the selected body, restore its source position, or
leave the last exact preview frozen while the pointer continued moving.

## Plan feedback and preparation

- Pending requests retain the last successful display. The selected body and
  its controls apply the residual from that display's proposal to the current
  pointer proposal. Before the first result, the captured source supplies the
  provisional display. Dependency geometry retains its last exact result.
- Consecutive identical effective proposals reuse pending or valid work. A
  rejected proposal can be retried with the same parameters.
- Each preparation lane permits one running job and one latest queued request.
  Pointer samples replace the queued request without repeatedly cancelling the
  running job.
- Once release is waiting, obsolete work is cooperatively cancelled so that the
  final proposal receives priority. In lanes using display tickets, work without
  a usable ticket is also cancelled when superseded.
- Superseded successful results have an opaque, gesture-scoped display ticket.
  They may advance displayed geometry while the current request remains pending.
  They cannot create a prepared command, mark the current request valid, finish
  release, or modify the document. Failed or invalid current results clear the
  entire displayed result; older tickets cannot resurrect it.
- Existing current-request source, selection, viewport, and preparation checks
  still govern release and commit. Cancellation and scene replacement retire
  display tickets.

Curved-host opening widths and corner-window legs retain their last exact ink
while the pointer controls and readouts advance. Provisional straight stretching
is limited to straight-host opening widths; it is not used to deform curved or
corner assemblies.

## Native 3D feedback

- A captured manipulation no longer performs idle hover detection. Passing
  across another handle therefore cannot deactivate the captured OCCT operation.
- Translation updates presentation transforms without rebuilding each body's
  presentation and selection on every pointer sample. Selection transforms are
  synchronized when the preview is reset.
- Interactive redraw invalidates the main scene before `RedrawImmediate()`.
  This refreshes model geometry while preserving the captured camera/depth range;
  calling `V3d_View::Redraw()` here would run AutoZFit during the gesture. Queued
  paint events use the same path while a transform owns the camera. Control
  attachment, selection changes, and interaction reset preserve that camera too,
  including the captured proof used by release picking and stationary Edit.
- Transform press preparation reserves scene and viewport depth before capturing
  the camera. Moving a solid along a depth axis no longer clips it against its
  source bounds. Plain selection settles the camera before capture, and a
  stationary context click captures after OCCT's rotation initialization.
- Roof/cohort similarity transforms retain candidate geometry and move selected
  candidate presentations by the residual proposal. A captured compound provides
  a provisional wire outline before exact geometry is ready. The provisional
  outline is not selectable and is hidden during native image output.
- Residual candidate solids use the topmost depth layer while pending. This
  keeps an uphill skylight visible above its previous host surface until exact
  manufacture establishes its new height. Exact candidate replacement restores
  ordinary depth; selected cohort members still depth-test against each other.
- Directional roof scaling retains exact candidate geometry while a lightweight
  proposed-extent outline and its controls/readouts follow the pointer. The
  outline stays out of image output and cannot authorize release. It does not
  affine-deform physical facets or claim their final manufactured bounds.
- `nativeRenderedImage()` reads the already presented native framebuffer. The
  regression checks use it before export or paint can hide a missing redraw.
- Native image output uses a private camera. OCCT's image capture restores camera
  parameters by copying them, which otherwise advances the live camera state and
  retires a still-active roof capture. Export retains the original live camera
  handle, state, and screen-to-world mapping, and invalidates the temporary
  control-free frame before the next hover redraw.

## Verification

Release builds completed for the application, native view tests, and the 24
selected canvas, desktop, preparation, and workload test targets. The application
and all selected test executables were relinked after the final native repairs.

The native functional matrix passed all 15 checks: `live-drag`, `gestures`,
`geometry`, `forms`, and `publication`, each at DPR 1, 1.5, and 2. The final full
matrix was run using PowerShell 7:

```powershell
./scripts/test-native.ps1 -Configuration Release -Scales @('1','1.5','2')
```

The three-scale `live-drag` scenario passed again after the final extent-outline
styling change and integrated Release build.

The regressions include successive native framebuffer samples before export or
queued painting, a scale drag crossing another actual OCCT handle, movement
beyond the source's depth range, hosted wall/opening and stair/railing lifecycle
checks, pending/exact/residual roof feedback, and directional extent feedback
across exact candidate replacement. Real desktop drag bursts verify
that a superseded worker can update display while the newest proposal remains
pending, followed by one final-pointer commit and one Undo.

The depth regression failed before depth reservation: the live frame contained
13,339 foreground pixels versus 30,488 in the privately refitted reference. It
passes with the reserved press-time range. The handle-crossing and pending-body
regressions also reproduced their failures before their corresponding repairs.
The uphill roof regression reproduced complete loss of the selected skylight's
2,078 glazing pixels before the residual depth-layer repair.

The selected 24 CTest suites have all passed. They cover axis and boundary
controls, deferred openings, connected walls and corners, object dragging,
symbol transforms, straight/curved opening resizing, directional alignment,
mixed component/roof cohorts, and native preparation/workload checks. The first
full run passed 23/24 in 2,537.61 seconds. `symbol_transform_desktop` failed once
with a combined export diagnostic, then passed an isolated rerun and three
consecutive `--repeat until-fail:3` runs after adding format-specific diagnostics.
The original export failure did not reproduce; no production export behavior
was changed.

Native logs and PNGs are retained under `artifacts/native-tests`; the integrated
CTest log, focused rerun logs, and plan PNGs are under
`artifacts/drag-preview-20261017`.

Functional hardware checks do not establish frame-rate qualification for every
model size or GPU, or user-observed resolution in an existing editing session.
