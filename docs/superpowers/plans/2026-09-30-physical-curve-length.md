# Physical curve-length constraints

Extend the accepted production plan with actual arc-length locking and editing,
preserving the distinction between a curved measurement and its endpoint distance.
This is an internal implementation checkpoint, not replacement certification.

- Engine owner: constraint codec, authoring, integrity and their focused regressions.
  Add `fixed_arc_length` in entity version 3 for opposite endpoints of one curved
  wall or stable boundary edge. Preserve sweep and exact entered quantity; solve
  the equivalent chord target and independently validate the physical result.
- Desktop owner: constraint dialog and native interaction regressions. Add Curve
  length, physical prefill, relevant endpoint/anchor defaults, preview, apply,
  editing/removal and clear rejection of incompatible geometry.
- Root owner: persistence, exchange, documentation, integration, build and Git.
  Retained version-3 relations require project format 12 / exchange version 9,
  including undone/deleted history. Preserve earlier relation/format semantics.

Verification: baseline codec RED, engine quantity/binding/geometry checks,
native end-to-end dialog editing, exact save/reopen/undo/redo, downgrade refusal,
opaque future constraints, legacy format tests, whitespace and source-kit gates.
Review numerical conversions and persistence semantics before delivery.

Direct curved-wall resize receipts and tangent relations remain separate in-scope
gaps. The new anchored relation workflow must itself edit geometry; a stored label
or passive quantity field does not meet this checkpoint.
