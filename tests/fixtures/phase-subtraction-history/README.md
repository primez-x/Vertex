# Historical Auto-Subtract fixtures

These ordinary Vertex project archives were captured before the semantic phase
admission correction, against production source at commit
`74c933f1acfd24945f272fd162e31997c760dc02` on 2026-10-04. They were created through
`Document.apply`, `ProjectWorkspace` checkpoint, Finish and Undo, and
`ProjectStore.save_archive`. No immutable snapshot or archive bytes were forged.
They contain generic synthetic geometry and IDs.

Both preserve an active-v2 Auto-Subtract owner and an undone Finish. One target
is proposed in an inactive alternative; the other is demolished in the active
alternative. Older Vertex accepted both. New edits must refuse both, while
load, exact envelope re-encoding, save/reopen, Undo and Redo preserve their
original history. The parent contains a floating-point opaque value `1.0` to
exercise representation-sensitive validation.

SHA-256:

- `inactive-parent.sketch`: `A2D73298E5804B05C70AAF46F3607A09CB1DC4322CE04FA3BA48EE4A859A425A`
- `demolished-parent.sketch`: `B6DB69F219991CC57DC017770ED2EF4438AF835FA96AB33B26B1C77923C0B1D8`

The current rules intentionally cannot regenerate these invalid new edits.
Preserve the fixture originals; tests write only round-trip copies to temporary
directories.
