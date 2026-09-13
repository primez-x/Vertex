# Placed boundary dimensions

Placed dimensions are stable `dimension` entities. A v1 segment-length
dimension stores a stable reference to the identified boundary entity and its
stable segment ID, together with the world position of the label:

```json
{
  "type": "dimension",
  "properties": {
    "dimension_version": 1,
    "dimension_kind": "segment_length",
    "target": {
      "entity_id": "boundary-1",
      "segment_id": "segment-a"
    },
    "text_position": [2.0, 0.75],
    "placement_origin": "manual"
  }
}
```

`text_position` is a finite pair of world coordinates in metres. The target
must refer to an identified v1 boundary and an exact stable segment ID. The
resolver reads the segment's canonical analytical line or arc and derives its
current length with `segment_length`; no stored length field is authoritative.
Reordering source segments therefore preserves the measurement, while a
canonical geometry edit changes the derived value.

Automatic placement uses the same fields and adds
`automatic_placement_version: 1`. Manual placement must omit that field.
Unknown placement versions or conflicting manual fields are rejected by the
strict v1 decoder.

Unknown entity properties, extensions, and nested target fields are retained by
the encoder when updating a known dimension. The entity ID and `type` remain
stable. An unsupported positive `dimension_version` or an unknown dimension
kind is returned as an opaque decode result carrying the original entity; it
cannot be encoded over or resolved as v1. Malformed known fields, invalid
identifiers, non-finite coordinates, and wrong JSON container types reject
without silently dropping data.

Document validation checks every supported dimension against its stable source
edge, including retained history. Deleting or retiring a referenced edge must
remove or retarget its dimensions atomically. Known dimensions targeting an
unknown boundary version are preserved in a read-only document. An unknown
dimension cannot hide malformed supported dimensions elsewhere in that state.
Format v2 protects dimension semantics even after all dimensions and boundaries
have been deleted from the current head but remain in undo history.

Version 2 retains those fields and requires a `presentation` object containing
exactly `text_height_mm`, `color`, `bold`, `italic`, `visible`, and
`rotation_radians`. Text height is a finite paper-space value from 0.5 to 20 mm;
color is a six-digit `#RRGGBB` string; the three switches are Booleans; rotation
is finite radians. Extra or missing presentation keys reject. V1 dimensions
keep their original encoding and rendering. A same-named vendor property in
v1 stays opaque and blocks promotion rather than being overwritten. Encoding
a v2 dimension without presentation also rejects rather than dropping style.

The dimension inspector edits position, paper text height, color, bold/italic,
visibility, and rotation. Style-only changes preserve the existing placement
origin and exact position. Moving the label sets manual placement and clears
the automatic-placement version. Edits retain the entity and target IDs, are
atomic, and participate in normal save/reopen and undo/redo. Boundary transforms
carry the presentation unchanged while moving the label with its source edge.

Both workspace canvases and the shared sheet-output renderer honor presentation.
Straight segment dimensions also regenerate a retained dimension line with two
extension lines from the referenced edge to the label offset. The linework is
derived from the same stable source segment on every refresh, so moving or
transforming the boundary cannot leave stale pixel geometry behind. Curved
dimensions use an offset arc with the source sweep and radial extension lines;
their displayed value remains the analytical source arc length.

The model also supports semantic `angle` and `area` dimensions. An angle
references two identified segments and their shared vertex; its value is
derived from the current analytical tangents and is rendered in degrees with a
shortest-path arc overlay. An area references the complete identified closed
boundary and derives the absolute signed area from its current geometry. Both
kind values persist through save/reopen, survive source geometry edits, and
render in both workspace canvases and fitted output using the active unit
system. Area dimensions intentionally have no segment target, so a boundary
owner remains the single source of truth.
Hidden dimensions retain their semantic references and remain editable through
selection in the project navigator. Paper text height uses the rendering
device's logical DPI, or the explicit fitted sheet paper scale in previews,
and remains independent of drawing/output scale; v1 labels
retain their existing model-space behavior. Global visibility filters still
apply. The shown value is derived from the referenced geometry in the selected
workspace units, not user-entered replacement measurement text.

The desktop workspace provides an offline creator for choosing angle rays or an
area owner. Full Apex workflow/output qualification remains open. The semantic
model, recalculation, persistence, shared rendering paths, and normal undoable
creation command are implemented, but the remaining qualification gate is
still required before the complete dimensioning requirement can be certified.
