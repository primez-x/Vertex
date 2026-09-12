# Property Studio project formats v1 through v6

Property Studio projects are standalone SQLite files containing one immutable logical
document snapshot and the complete command history known when that snapshot was captured.
The file is an interchange/save artifact. The current foundation keeps the working document
in memory; it does not claim to be a live SQLite working journal.

Version 2 retains the v1 table structure and adds a mandatory compatibility
boundary for identified geometry. Any identified boundary, boundary draft or
dimension in retained history requires v2, including an undone or deleted
identified boundary. Both SQLite `user_version` and `metadata.format_version`
must agree, and the logical digest includes that version. The reader accepts
v1 legacy history, v2 identity history, v3 construction-receipt history and v5
translation history and v6 transform history, plus v4/v5/v6 archives through
recovery-aware APIs. Under-versioned semantic data and versions above 6 reject. Legacy-only history
is still written as v1. Unknown boundary entity
versions in v2 remain preserved read-only. See `boundary-entity-format.md`.
Version 2 also recognizes `dimension` entities. Their supported segment-length
form refers to a stable child ID on an identified boundary; every retained
state validates those references. Unknown dimension versions and kinds remain
opaque and make the project read-only. A supported dimension on an unknown
boundary version is preserved read-only without guessing its geometry.
See `boundary-dimensions.md` for the typed dimension contract.

Version 3 retains the v1 and v2 tables and adds a storage guard for the reserved
`boundary_authoring` property. The guard first qualifies an explicit identified
boundary: a recognized boundary type with supported integer
`boundary_model_version: 1`. A `boundary_authoring` property on that owner
requires v3 anywhere in retained history, including an undone or deleted
entity, regardless of the envelope version or shape. Generic entities and
anonymous legacy boundaries may retain a vendor collision in v1. An unknown
positive boundary model remains v2 and opaque. The property is preserved as
opaque JSON through save and load; receipt envelope validation, replay
semantics, and editability belong to the document and receipt codec layers.
See `boundary-authoring.md` for the authoring contract.

## Boundary construction envelopes v1 and v2

On a supported identified boundary, `properties.boundary_authoring` has exactly
these fields: `version` (1 or 2), `replay_version: 1`, `boundary_id`, `anchor`,
`segments`, and `extensions`. `anchor` is a finite `[x, y]` point in metres;
`boundary_id` equals the owning entity ID. `extensions` is an opaque JSON
object. Both known schemas implement replay algorithm version 1. An unknown
positive replay version preserves the complete envelope and makes the retained
document read-only, as does an unknown positive schema version. Missing, zero,
negative or noninteger version fields reject for recognized schemas. Unknown
schemas remain opaque without assuming their payload shape.

`segments` is a nonempty ordered array. Each member contains exactly
`segment_id`, `start_vertex_id`, `end_vertex_id`, and `receipt`. Its identities
must match the corresponding canonical boundary segment. The receipt repeats
`segment_id` and contains `kind`, `start`, `clockwise`, and the fields below.
There are no optional or additional fields for a known kind.

| `kind` | Additional required fields |
| --- | --- |
| `line_heading` | `distance`, `heading` |
| `line_rise_run` | `rise`, `run` |
| `line_relative_turn` | `distance`, `turn` |
| `line_closure` | `closure_delta` |
| `arc_chord_angle` | `chord_end`, `angle` |
| `arc_chord_height` | `chord_end`, `height` |
| `arc_chord_length` | `chord_end`, `arc_length` |
| `arc_start_tangent` | `tangent`, `arc_length`, `sweep` |

Schema v2 adds `line_to_point`, requiring only `chord_end` beyond the common
receipt fields. Schema v1 remains closed to the eight kinds above and rejects
`line_to_point`. Current authoring sessions emit schema v2, while older schema
v1 records retain their version when re-encoded. The SQLite storage version
remains 3 for either envelope.

Receipt schema v3 adds a required `transforms` array. Each entry has exactly
`pivot`, `rotation_radians`, `flip_horizontal`, `flip_vertical`, and `offset`.
Points are finite two-number arrays, rotation is a finite number, and flips are
Booleans. Replay first reconstructs the original local receipts using schema-v2
rules, then applies each transform in order: rotate about the pivot, reflect X
and Y about the pivot as requested, and translate. An odd number of reflections
reverses arc sweep. Each resulting boundary must remain valid. Replay rejects
non-finite residuals, endpoint or analytical-length drift beyond its geometry
tolerance, and accumulated pure-translation rounding beyond that tolerance;
see `geometry-operations.md` for the precision contract.

The stored anchor, receipt coordinates, closure vectors, exact quantities, and
entered expressions remain local and unchanged. Replayed edges and anchor are
world coordinates; replayed receipts still contain their original local inputs.
Copies may remap typed identities. Extensions are preserved without interpreting
identifier-shaped user data. An empty transform array is valid. Schema v1/v2
reject the transforms field and retain their original encodings; new drawing
sessions still emit v2. The SQLite receipt storage minimum remains format 3;
explicit in-place translation history independently requires format 5.
Explicit in-place rotation/reflection history requires format 6.

Point construction copies the finite endpoint directly into a straight segment
after checking its exact start and minimum chord length. It performs no angle
conversion or synthetic quantity parsing. It records a coordinate-defined edge;
it does not establish click origin, a typed measurement, a snap relationship or
a geometric constraint. Snapping resolves coordinates before this command.

Points and deltas are finite two-number arrays. `clockwise` is a Boolean; it
must be false except for chord-length construction, whose unsigned length
needs this direction choice. Signed angle, height or sweep inputs carry
direction for the other arc forms.

A quantity object has exactly `metres`, `exact_metres`, `entered_unit`, and
`original_expression`. `exact_metres` contains signed 64-bit `numerator` and
positive signed 64-bit `denominator`. `entered_unit` is one of `metre`,
`millimetre`, `centimetre`, `foot`, or `inch`. Parsing the nonempty expression
with that default unit must reproduce the stored quantity exactly.

An angle object has exactly `radians`, `original_expression`, and
`normalized_expression`. Both nonempty expressions must parse to the stored
finite radians without a tolerance. The normalized expression is the canonical
round-trip decimal radians string produced by the v1 codec.

Replay derives each edge from these inputs, using the preceding edge for a
relative turn and the initial anchor for generated closure. It must reproduce
the canonical ordered topology and analytical coordinates exactly, and the
result must be a valid closed boundary. Receipts document a reproducible
construction; they do not prove that a person entered an expression. Display
rounding never changes these stored values.

## Document contract

Every semantic entity has a stable ID, a type, a JSON `properties` object, a `required` flag,
and a JSON `extensions` object. IDs are document identity and are never derived from geometry.
The v1 known types are:

`property`, `building`, `floor`, `layer`, `boundary`, `measurement_boundary`, `room_boundary`,
`wall`, `opening`, `room`, `slab`, `roof`, `stair`, `railing`, `column`, `beam`, `label`, `sheet`, `view`,
and `constraint`.

All geometry properties use metres and radians. A wall and opening can be represented as:

```json
{
  "id": "wall-1",
  "type": "wall",
  "required": false,
  "properties": {
    "floor_id": "floor-1",
    "baseline": {
      "start": [0.0, 0.0],
      "end": [5.0, 0.0],
      "sweep_radians": 0.0
    },
    "thickness_m": 0.14,
    "height_m": 2.4,
    "elevation_m": 0.0
  },
  "extensions": {}
}
```

```json
{
  "id": "opening-1",
  "type": "opening",
  "required": false,
  "properties": {
    "wall_id": "wall-1",
    "offset_m": 1.2,
    "width_m": 0.9,
    "sill_m": 0.0,
    "height_m": 2.0
  },
  "extensions": {}
}
```

A slab uses `boundary`, an array of the same `{start,end,sweep_radians}` segments; `holes` is
an array of boundary arrays. Its scalar fields are `thickness_m` and `elevation_m`.
Entered unit text and exact quantity fields are separate semantic properties; display units do
not change the metre geometry.

For known entity types, the document validates `refs` and `references` arrays as generic entity
references. It also validates canonical singular and plural reference fields for each known type,
such as `property_id`, `building_id`, `floor_id`, `wall_id`, `column_id`, `beam_id`, and `sheet_id`;
a referenced entity must exist and have the named type. `boundary_id` continues to mean the exact
`boundary` type; the distinct `measurement_boundary` and `room_boundary` types carry their own
`floor_id` and `layer_id` links. `parent_id`, `host_id`, `target_id`, and `entity_id` are generic
references. `asset_id` and `asset_ids` resolve against the pinned asset map; a missing asset or
deletion of an asset still in use rejects the complete command. This is structural referential
validation for ordinary entities. Known persisted wall constraints additionally enforce their
semantic bindings, hard residuals and host validity at the document boundary; see
[persistent wall constraints](constraint-entity-format.md). Full geometric, topology and solver
workflow validation remains incomplete.

Commands are the only mutable interface. `ApplyEntityChanges` atomically applies entity and asset
upserts/deletes against an exact expected revision. Duplicate operations, stale revisions,
invalid JSON, asset checksum failures, and dangling or mistyped references reject the whole
command without advancing the revision. Persisted action text is limited to 1,024 bytes and
revision names to 256 bytes; both require valid UTF-8 without embedded NUL. Media types have the
same encoding requirement and reject CR/LF. JSON floating-point NaN and infinity are rejected
before encoding because JSON cannot preserve them. `NameRevision` records an immutable named
revision.

Undo and redo create new, monotonically increasing revisions. An edit after undo clears the
navigation redo stack but retains every old revision and named branch in history. Each revision
currently stores a full entity and asset state. This is intentionally simple and lossless, but
large histories can use substantial space; delta compaction is a future format change.

`DocumentSnapshot` owns copies of every entity, history record, and asset byte. Its getters are
const-only. Saving a snapshot captured at revision R always writes R, even if the working document
has advanced to R+1. After publication, `mark_saved(R)` leaves an R+1 head dirty.

Unknown optional entity types and unknown JSON fields survive unrelated commands and a v1
save/load cycle. Property and extension JSON is encoded deterministically by nlohmann JSON, so
v1-generated unknown values re-encode with the same JSON value types and canonical bytes. Input
whitespace, source object-key order, and alternate numeric spellings are not retained. An unknown
required entity is loaded and exposed for inspection, but the document becomes read-only. An
unknown project `format_version` is rejected rather than opened unsafely.

## SQLite schema

The SQLite `application_id` is `0x50535444` (`PSTD`). `user_version` and metadata
`format_version` are both `1`, `2`, or `3`, according to the retained semantics.
All three versions use the following application tables:

| Table | Purpose |
| --- | --- |
| `metadata` | `format_version`, `document_id`, `head_revision`, `saved_revision`, and `logical_digest` |
| `revisions` | Revision identity, event parent/source, action/name, and undo/redo navigation stacks |
| `revision_entities` | Full entity state for each revision, including properties and extension JSON |
| `revision_assets` | Full pinned asset bytes, media type, metadata JSON, and SHA-256 for each revision |
| `named_revisions` | Stable name-to-revision mappings, including retained branches |

All application tables are SQLite `STRICT` tables with primary and foreign keys. A standalone
v1–3 file must have `saved_revision == head_revision`, use SQLite `DELETE` journal mode, and have no
`-journal`, `-wal`, or `-shm` sidecar. Load verifies the exact metadata-key set, `user_version`,
table columns, primary keys, foreign keys, and `STRICT` flags. It rejects missing or additional
schema objects, invalid column types, non-contiguous or impossible history transitions, a
non-bijective named-revision index, duplicate IDs, invalid JSON, broken references, oversized
data, asset hash mismatches, and a logical digest mismatch.

The logical digest is BCrypt SHA-256 over a deterministic JSON manifest of metadata, revision
structure, entities, asset metadata, asset sizes, and asset SHA-256 values. Each asset's SHA-256
binds its bytes to that manifest. This detects corruption and uncoordinated modification. It is
not a signature or message authentication code and does not prove who created a file.

Current safety bounds are 128 bytes for IDs, 64 bytes for entity types, 1 MiB per JSON object,
64 levels and 100,000 values per JSON object, 256 MiB per asset, 4 GiB per project file, 10,000
revisions, 250,000 entity rows, 100,000 asset rows, 64 MiB of encoded JSON, 2,000,000 aggregate
JSON values, and 512 MiB of aggregate decoded asset bytes. Physical size and SQL aggregates are
checked before graph/blob allocation; allocation failures are translated to typed resource-limit
errors. The SQL preflight measures JSON as UTF-8 bytes (`length(CAST(value AS BLOB))`), rather than
SQLite text characters. IDs allow ASCII letters, digits, dash, underscore, dot, and colon. Project
paths must name ordinary files under an existing, non-reparse-point parent directory. Windows
device names and alternate data streams are rejected; ambiguous trailing-dot or trailing-space
names are rejected when creating a destination.

The separate recovery-aware v4 format adds a recovery-record table and preserves
the optional captured saved revision. Its complete schema, digest and opaque
load contract are documented in [project-archive-v4.md](project-archive-v4.md).
Document-only APIs refuse v4 rather than discard its recovery ledger.

Version 5 is required when any retained revision contains an explicit boundary
translation proof, including an undone command or an abandoned branch. It adds
one nullable `TEXT` column, `revisions.boundary_translation_json`, after
`redo_stack_json`. SQL `NULL` means no proof; a JSON `null` value is invalid.
A present proof has exactly `{"version":1,"boundary_id":"...","offset":[x,y]}`.
The version is an integer, the boundary ID obeys the ordinary identifier rules,
and both offsets are finite numbers in metres. Unknown versions, extra keys,
duplicate keys and malformed values reject. Proof JSON participates in the
aggregate byte, value and recovery string budgets.

A v5 file may contain the same `project_recovery_records` table as v4. Its
presence makes the file an archive: recovery-aware APIs preserve its ledger and
optional saved revision, and document-only load or replacement rejects it.
Without that table, v5 has the standalone document saved-revision rules. The
exact expected schema is checked in both cases. Histories without proofs retain
the existing v1-v4 schemas and digest encodings.

The proof is included in logical and document/source digests. On restore the
document recomputes the complete next entity state from the previous revision
and the offset; it rejects unrelated edits, changed assets, forged offsets or
missing proofs even after an attacker recomputes the logical digest. Human
action text grants no authority. Exact undo/redo references retain the original
proof on its command revision rather than copying it onto navigation records.
JSON/assets extraction uses exchange version 2 when proofs occur and emits
`boundary_translation` on the corresponding revision; proof-free extraction
remains exchange version 1.

Version 6 is required when any retained revision contains an explicit boundary
transform proof, including undone commands and abandoned branches. It retains
the v5 translation column and adds nullable `TEXT`
`revisions.boundary_transform_json`. A transform proof has exactly `version`
(integer 1), `boundary_id`, `pivot`, `rotation_radians`, `flip_horizontal`,
`flip_vertical`, and `offset`. Points are finite two-number arrays in metres,
the angle is finite radians, and both flip fields are Booleans. Unknown versions,
duplicate or extra keys, and malformed values reject. SQL NULL is absence;
JSON null is invalid. A revision cannot contain both proof types.

Transform proofs participate in the same digests, resource budgets, complete
state reconstruction, and navigation restrictions as translation proofs.
The command preserves identities and local receipt inputs, appends an ordered
schema-3 frame, and transforms attached dimension positions. Unrelated entities
and assets must remain identical. A v6 archive is distinguished by its recovery
table, as in v5; document-only APIs cannot discard that ledger. Histories without
transform proofs keep their earlier minimum format and digest representation.
JSON/assets extraction uses exchange version 3 when a transform proof occurs
and emits `boundary_transform` on its command revision. Translation-only and
proof-free histories retain exchange versions 2 and 1, respectively.

## Save and replacement protocol

`ProjectStore::save` takes an immutable snapshot. If the destination exists,
`expected_destination_sha256` is mandatory and must equal the fingerprint returned by the
preceding load or save. A missing, replaced, or modified destination fails closed.

The save sequence is:

1. Open the parent directory and reject a reparse point. For an existing destination, open the
   complete destination and use its handle-resolved, volume-backed normalized path; this expands
   extended, trailing-dot, and available DOS 8.3 aliases. For a missing destination, append only a
   non-ambiguous final filename to the handle-resolved parent. Apply Windows invariant case folding
   and acquire the corresponding `Global` named mutex. Failure to resolve or access the identity
   fails closed. One expected fingerprint authorizes at most one cooperating save.
2. Reserve a new, unique file with Windows `CREATE_NEW` in the destination directory and retain
   that file-object handle through SQLite writing, flush, validation, and publication upgrades.
3. Create the complete SQLite schema and content in one `BEGIN IMMEDIATE` transaction with
   `synchronous=FULL` and `journal_mode=DELETE`, flush SQLite's page cache, close SQLite, and call
   `FlushFileBuffers` before sealing the staging bytes against further writes.
4. Hold a read handle to the staging file while SQLite runs `integrity_check` and
   `foreign_key_check`, decodes and validates the complete document, and verifies asset and logical
   SHA-256 values. After SQLite closes, `ReOpenFile` acquires `DELETE` access to that same file
   object while denying other writes, rename, and deletion. The publication handle is hashed again
   and must equal the validation digest.
5. Recheck the destination fingerprint while holding its file identity read-only, with deletion
   sharing enabled for replacement. Copy that locked expected destination by handle to a unique
   `.bak.<uuid>` sibling, flush it, and verify its SHA-256. Immediately before publication, open the
   current destination name again, require the same Windows file identity and fingerprint, and keep
   both destination read handles through replacement so content writes remain denied. Keep the
   verified backup read handle with no write/delete sharing through publication and receipt return,
   so its exact bytes cannot be changed, renamed, deleted, or replaced in that interval.
6. Atomically rename the validated staging file object on the same volume with
   `SetFileInformationByHandle(FileRenameInfoEx)`. Replacement uses the Windows replace and POSIX
   flags so the verified destination may remain read-open; new-target saves request neither flag.
7. Return the already verified publication-handle digest, the exact saved revision, and the backup
   path.

Failures before publication run checked cleanup for the exact temporary database, its exact
`-journal`, `-wal`, and `-shm` siblings, and any not-yet-published backup. If Windows prevents
removal, the save error retains the original failure and reports the exact residual paths; it does
not claim cleanup succeeded. When more than one removal fails, the diagnostic lists every exact
path and `StorageError::residual_paths()` exposes the complete structured list while preserving the
original error code and message. A process termination can leave a uniquely named staging file or
backup for later recovery cleanup. The fault-injection stages `after_journal_creation`,
`after_database_write`, `after_validation`, and `before_publish`, plus a validation barrier, exist
for deterministic rollback and lock tests. Coverage includes denial of external writes,
rename/deletion, and path replacement while the publication handle is held; exact published digest
and backup bytes; destination write denial plus rename-and-plant revalidation after backup; stale
external fingerprints; backup write/delete/rename/replacement denial; simultaneous locked backup
and staging-sidecar cleanup residuals; multibyte JSON budgets; and cooperating writers split across
normal, extended, trailing-dot, Unicode, and available DOS 8.3 aliases.

A noncooperating process cannot write destination content while the prepublication read handles are
held, but it can still rename or delete a deletion-shared destination in the small interval after
the final pathname-identity check and before handle-based replacement. Windows does not expose a
generic path-level compare-and-swap replace. The verified prior-file backup is the recovery boundary
for that rename-only OS race. Antivirus or file-indexer handles can also make replacement fail, in
which case the original stays in place and the save reports an error.

## Explicitly pending

The in-memory `Document` is a serialized single writer but does not yet have a durable live working
journal. A WAL/FULL edit journal, recovery after process termination during editing, and compaction
remain future work. There is no long-lived edit-session lease: two sessions may open the same file,
but mandatory expected fingerprints and the save mutex prevent a cooperating stale session from
silently overwriting a newer save. A user-facing second-open read-only policy remains UI/session
work.

The atomic replacement path has deterministic injected-failure coverage, but it has not been
qualified against real machine power loss, filesystem filter drivers, or disk-full conditions at
every write. Save fails closed unless Windows identifies the destination as a local fixed disk
using NTFS or ReFS; UNC paths, mapped network drives, removable media, and other filesystems are
outside the durability boundary and are rejected before staging.
