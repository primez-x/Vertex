# Selected symbols and labels in clipboard operations

This checkpoint addresses APX-EDIT-001's individual-annotation gap. It does not
qualify the complete Apex replacement or production release.

## Behavior

Copy includes only selected label/symbol instances, merging selected children
of the same owner into one clipboard root. Saved artwork, placement, independent
size, rotation, mirroring, style, text and opaque metadata remain intact.
Presentation overrides targeting those children travel with them; unrelated
overrides do not. Paste allocates fresh owner and child identities and remaps
the selected overrides without rewriting ordinary text or opaque data.

Cut and Delete update annotation owners rather than erase their unselected
children. Mixed geometry/annotation selections use one admitted command,
including hosted dependencies. Explicit whole-owner selection dominates an
overlapping child selection. Empty owners retain their metadata. Failed and
read-only operations preserve source and clipboard; Copy remains available in
read-only projects. Cut publishes its prepared clipboard only after admission
and commit succeed.

Copied child subsets use optional new containers, even when their source owner
is protected. Source-owner protection remains intact and whole protected-owner
removal stays refused. Pasted label/symbol placements use the destination active
layer; an unavailable destination context refuses before document mutation.

The clipboard format/version is unchanged. Paste retains the existing behavior
of selecting newly created owner roots rather than individual children.

## Verification

The native baseline failed after copying one symbol because unselected siblings
and unrelated overrides appeared in the payload. The focused workflow uses the
actual MainWindow selection, copy, cut, paste and delete commands and the Qt
system clipboard, with multiple labels/symbols and a wall/hosted door.

Independent review identified two additional baseline failures: required-owner
child cuts produced unpasteable payloads, and a real SVG component copied from
a custom source layer could paste invisibly into another project. Both receive
explicit native regressions rather than relying on matching default layer IDs.

- Release application and affected native targets built successfully.
- Final focused checks passed 4/4 in 18.77 seconds: annotation clipboard,
  symbol transforms, source-kit coverage and requirement schema.
- The 4.45-second clipboard workflow covers selected subsets, multiple owners,
  whole-owner/child overlap, preserved raw metadata and transformations,
  selected override remapping, mixed wall/hosted door deletion, protected
  owners, actual SVG destination-layer visibility, history and save/reopen.
- The existing geometry/material clipboard regressions run in the same
  focused workflow, covering hosted links, dimensions, material dependencies,
  malformed payloads and legacy single-root data.
- Root reviewed the integrated implementation and resolved both independent
  review findings. These native checks do not claim user-observed resolution.

Release executable SHA-256:
`169f2eb0e82eb2d998d9d73332c7f189a2435fbf9c83541ed937ae455e9f5658`.
Launch with `scripts/run.ps1 -Configuration Release`; the previous installed
rotation checkpoint has not been repackaged with this change.

Manual tasks U066, U067 and U072 now include practical individual/multiple
annotation scenarios, mixed hosted geometry, history and cross-project paste.

## Remaining scope

Selection filtering, broader linked-object ownership rules, device/user testing
and production compatibility qualification remain open.
