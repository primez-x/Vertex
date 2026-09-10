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

Dimension-line rendering, styles, unit formatting, other dimension kinds and
authoring UI remain separate integration work. These codecs and reference
checks do not constitute a finished dimensioning workflow.
