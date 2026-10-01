# Pasted-object controls and headless build checkpoint

## Product behavior

Paste selects the actual newly created annotation children, rather than their
invisible storage containers. Geometry root order remains stable. A pasted
symbol can immediately move, resize and rotate through its canvas controls;
mixed geometry/annotation selections remain one atomic history operation.
Transforms preserve admitted owner, state and placement metadata and untouched
siblings. Ambiguous cross-owner child identities are refused before mutation.

Rotation uses the committed selection frame orientation. Its handle retains
that orientation after release, history navigation and reopen. Default rotation
snaps to 45-degree increments; Shift allows fine adjustment, and the canvas
shows the effective angle during the gesture. Returning a tilted boundary to
its original angle restores its original geometry.

## Headless build

The prior geometry-disabled CLI failed to link with LNK1181 for
`sketch_calculations.lib`. Storage recovery validates area deductions through
the exact geometry engine. The supported headless configuration now declares
that dependency, excludes Qt and the desktop visualization host, and uses
separate `windows-headless-*` directories. Explicitly disabling both geometry
and desktop fails at configuration with the dependency explanation.

`scripts/run-cli.ps1` supplies native DLL paths for one invocation and restores
the caller's PATH, including on failure. Existing project overwrite refusal
preserves the file bytes.

## Verification

- The baseline pasted-sofa native fixture failed because the invisible owner,
  rather than its visible child, was selected.
- Release headless CLI and five affected native targets built successfully.
  Calculations, appraisal document, workspace finish, recovery ledger and
  boundary commit passed 5/5 in 0.79 seconds.
- The actual headless runner created, inspected and validated a new project.
  A repeated create was refused; the project hash and caller PATH were unchanged.
- Final Release desktop targets built successfully. Annotation clipboard,
  measurement group movement, appraisal desktop workflow, symbol transforms
  and axis canvas controls passed 5/5 in 37.31 seconds. The clipboard suite
  includes actual v1/v2 side-handle promotion, duplicate-owner transform refusal,
  immediate pasted selection, exact raw metadata comparisons and history/reopen.
- Root inspected `artifacts/transform-controls-20261001-final/rotation-live-90.png`:
  the rotation handle follows the rotated frame to its left side, the component
  remains visible through the selection and the callout displays 90.0 degrees.
- Runtime import inspection covered 113 component binaries with zero unresolved
  imports. This is dependency evidence on the development host.
- The requirement contract and source-kit completeness checks passed; all 1,120
  required tracked source paths are allowlisted.
- Release executable SHA-256:
  `66c4bc0ed9a75d693910868908714be30a0021bd17e2de38d22d672fa9befa9e`.

This is a bounded implementation checkpoint. It does not certify Apex parity,
external integrations, all architectural workflows or a production release.
