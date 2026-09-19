# Durable boundary recovery

This document defines the implementation and reference contract for durable
unfinished boundary recovery in Vertex.

The contract below includes both implemented core APIs and planned integration.
The recovery ledger and internal v4 archive save/load routes are implemented;
desktop open/save now uses guarded recovery-aware paths, while autosave,
restart recovery, live workspace editing and ownership integration remain open.
This does not certify production or Apex parity.

## Current boundary

The current application already has shared Draw First and Define First
authoring, construction receipts, sealed commits, and storage format v3. Its
working document is still an in-memory state rather than a durable live
journal. Unfinished sessions are currently kept in memory and reported as
unsaved. The existing v1–3 document-only routes remain unchanged. Separate
recovery-aware v4 routes are documented in `project-archive-v4.md`; the desktop
now opens supported ordinary v4 archives and preserves their ledger on save.
Direct edits to a restored recovery document fail closed until all desktop
mutations use workspace commands.

The recovery work is staged as three dependency packages:

1. **A1, undergoing integration:** the session checkpoint/action codec and
   `document_authoring_source_digest_v1` binding are implemented. Focused
   Debug/Release checks pass for save-stable source binding, receipt encoding,
   session operations, construction, commits, and an unfinished 256-edge
   checkpoint with its full undo/redo tail and exact point inputs. Expanded
   action/phase/fault tests and the full v24a 48-test suite pass in both builds.
   Resource review found that the existing scalar limit misses automatic
   dimensions, duplicated strings and snapshot amplification. The compact
   history repair now passes nine focused suites in both configurations,
   including the frozen 13-case, 66-position byte/bit reference corpus.
   Isolated 256/512/1,024/2,048-edge measurements show approximately linear
   retained memory. At 1,024 edges, Release private memory after construction
   fell from 926 MB to 53 MB. A separate 100,000-action history teardown check
   passes in both configurations. Shared byte admission now covers live edits,
   encoding, decoding and bounded estimator replay; allocation-fault regressions
   cover returned values and retired metadata disposal.
   These checks do not establish aggregate disk recovery.
   The integrated v24d checkpoint passes all 55 tests in Debug and Release
   with 181 recorded source inputs unchanged during build and test. Eighteen
   offscreen boundary canvas/input/workflow runs also pass across DPR 1, 1.5,
   and 2 in both configurations. These are regression checks, not native
   GPU, device-input, or finished desktop recovery certification.
   The [memory investigation](boundary-recovery-performance.md) records the
   measured workload, memory stages, limitations and reproduction commands.
   `Document::fork(snapshot)` is also implemented through the existing full
   restore validator; focused tests preserve saved markers, both history stacks,
   source isolation and read-only protection while rejecting malformed history.
2. **A2, in integration:** the aggregate archive, storage, history and
   workspace coordinator core. Internal v4 archive save/load and ledger
   validation are implemented and pass focused Debug/Release checks. Windows
   ownership, role promotion and opaque-copy services remain integration work.
   The dedicated-thread ownership broker passes its configured Debug/Release
   tests, but independent review found required shutdown,
   mutex release/close retry, and abandonment-observation repairs. Real
   Windows subprocess regressions pass in Debug/Release against the frozen
   v1 source, covering real contention, namespace independence, partial
   rollback and abandonment. The v4 repair passes both configured ownership
   tests in Debug/Release, including allocation-free shutdown signaling,
   preallocated rollback bookkeeping, retryable release/close cleanup, and
   unexpected owner-exit cases. The subsequent review found a shutdown-drain
   race and lost abandonment observations on registry allocation failure.
   Both are repaired with focused regressions; the shutdown regression failed
   before the fix and passes afterward. `ProjectOwnershipSession` now binds
   that broker to normalized project paths and Windows volume/file identities,
   with explicit external-digest revalidation and read-only second-open
   behavior in the desktop. The cross-process project-ownership fixture also
   verifies native mutex contention, case-insensitive Windows paths, and clean
   retry after the owner exits. Restart recovery, hostile replacement races,
   and production qualification remain required.
3. **B, in integration:** guarded desktop save/open and Save As paths now use
   the v4 archive and acknowledgement boundaries. Full workspace-command
   mutation routing, autosave, restart recovery, shutdown and recovery UI
   remain required.

These are implementation checkpoints inside the existing full production
scope. They do not reduce that scope or turn the current v3 tests into durable
recovery evidence.

## Authority and invariants

`ProjectWorkspace` is the planned application owner above `Document`, the
authoring session, and project organization. It owns the current document,
recovery lifecycle state, workspace identity, and generation watermarks.
`Document` continues to own document entities and document history; it does
not own recovery records. `BoundaryAuthoringSession` owns semantic draft
state, normalized local actions, local undo/redo, and stable draft identities.
The storage layer owns archive validation, staged publication, destination
compare-and-swap (CAS), backups, and atomic replacement.

The workspace is the only mutable application authority. Planned callers use
const or snapshot access and explicit workspace commands for apply, undo, redo,
mark-saved, replacement, and test mutation. There is no mutable
`Document` escape, mutable shared-document injection, or observer that waits
for a mutation and repairs bookkeeping afterward. Desktop code must migrate
away from mutable `MainWindow::document()` access. This is an internal API
boundary; no third-party mutable-`Document` ABI is promised.

The initial `ProjectWorkspace` document-publication API is now implemented and
independently reviewed. It owns a validated private Document and an immutable
instance identity. Edit, undo and redo preparation produce sealed move-only
tickets with detached preview snapshots. Commit checks the exact identity,
epoch and full source digest, then swaps the private candidate into place and
advances the epoch once. Foreign, stale, consumed and moved-from tickets reject
without changing the receiver. Caller changes to commands or preview copies do
not change the sealed candidate. The retired Document remains in the consumed
ticket until that ticket is released, avoiding JSON cleanup in the publication
step. Ticket and workspace access are serialized on the owning application
thread; this is not a concurrent container or an I/O-worker interface.

Focused Debug/Release tests cover publication, rejected tickets, preview
isolation, exact Document navigation, read-only enforcement and invalid-history
construction (`artifacts/reviews/workspace-publication-*-v1-tests.log`). This
API now also has the in-memory activation/discard navigation described below;
persisted-ledger validation is now implemented; desktop migration remains pending. Publication's source-reviewed swap
boundary is not a claim of universal allocation-failure safety throughout
Document construction and destruction.

The workspace also owns an optional active checkpoint in the same candidate
bundle. `prepare_boundary_checkpoint` validates canonical replay and the
enclosing record under the workspace's resource policy before retaining a
detached copy. A replacement requires a current source, the same source,
namespace and mode, and nondecreasing identity counters. Exact repeats reject.
First capture and semantic changes advance edited and checkpoint generations;
pointer-only replacement advances checkpoint generation and epoch only. Local
draft undo/redo changes the checkpoint without adding a Document revision.
Document edits and navigation retain the active checkpoint unchanged, including
its original source binding; a now-stale draft cannot be silently reattached
even when undo restores identical geometry. Finish, discard, stale-session load
and explicit rebinding are still pending lifecycle operations. This staging API
is not yet connected to desktop pointer input and has no frame-rate claim.

Focused tests cover sealed checkpoint copies, detached getters, pointer and
semantic publication, local undo/redo, stale and foreign tickets, malformed
records, identity/counter rejection, policy limits and preservation across
Document navigation. Source review found no unresolved publication defect;
allocation-failure execution with populated active state remains unverified.

`ProjectWorkspace::capture()` returns one detached `ProjectWorkspaceSnapshot`
containing the document, active checkpoint, document-event history, instance
identity, epoch, content generations and resource policy. Owner-thread capture
keeps those fields associated with one state before handoff to a worker. Copies
remain usable after workspace destruction and do not expose mutable workspace
data. Capture is neither the v4 archive codec nor authority to acknowledge a
save; role, destination and ownership correlation remain separate prerequisites.

A command is prepared against an immutable snapshot. Preparation uses the
validated `Document::fork(snapshot)` helper, validates the candidate document
and recovery state, and captures the expected workspace identity,
`workspace_epoch`, and full source digest. Commit rejects a stale identity,
epoch, or digest and otherwise publishes the complete candidate with one
exception-safe swap. A successful semantic or lifecycle operation advances the
workspace state once. Failed validation or commit changes neither the current
workspace nor its recovery state.

The authoring source and the saved-file marker are separate facts. The full
document snapshot digest remains the existing sealed-commit integrity binding.
Recovery bookkeeping must never make an otherwise current authoring source
stale, and changing save markers must not change the authoring source digest.

## Session checkpoint and normalized replay

The planned recovery checkpoint is a versioned typed value, separate from the
existing `boundary_authoring` envelope versions and from SQLite storage
versions. It contains the authoring mode and options, identity namespace,
pointer, normalized semantic action timeline, local `history_position`, four
next-ID counters, and an opaque `extensions` object. It retains the current
anchor, cursor, classification, pending dimension, stable identities, and
enough exact receipts to validate the reconstructed draft. A pointer is model
space state; it is not a second coordinate authority.

Actions represent semantic transitions rather than raw mouse events. The
known action families are classification changes (including current and last
classification), explicit anchoring, pen up/down, the eight existing line and
arc receipt constructors, point-native line construction, explicit closure,
manual or automatic dimension placement, and `close_chain`. Existing numeric
overloads normalize exactly as the current construction code does. An anchor
action records its resolved explicit point. A successful no-op creates no
action. Restore never calls desktop finish and never synthesizes a closing
edge.

The four counters are unsigned 64-bit next-ID values in boundary, vertex,
segment, and dimension order. They are allocation watermarks, not counts.
Zero, overflow, and decreasing values are invalid. Every retained action
records counter-before and counter-after checkpoints plus its generated IDs,
receipts, and identity references. Replay restores a counter-before value only
when it dominates the prior high-water value, performs the canonical operation,
and requires exact counter-after values, generated IDs, and receipts. The
canonical allocator, including automatic dimensions, is the only source of
allocation deltas; replay does not maintain a second handwritten formula.

Abandoned branches may leave forward counter gaps. A gap alone is not forged
data. Decreasing, overflowing, colliding, namespace-inconsistent, or
allocation-inconsistent values are malformed. Exported high-water values must
dominate every retained checkpoint, including redo-tail actions and values
kept after a reset. Reused references remain references and do not allocate a
new identity.

Undo and redo move `history_position` through the normalized action timeline.
Starting a new semantic branch truncates reachable redo actions while keeping
monotonic identity high-water values. `discard_redo_branch` has the same
counter rule. Pointer movement stays outside local semantic history and is
restored after semantic replay. It schedules a recovery checkpoint but does
not become a document undo command or explicit-save dirty change.

Every semantic mutator and history operation runs on a private candidate that
contains state, counters, stacks, and the action timeline. Canonical replay
must produce exactly one matching normalized action for each input action;
nested construction helpers must not append duplicate actions. The candidate
is published only by a no-throw swap after all allocations and validation have
succeeded. Fault injection before mutation, before timeline append, and before
publication must leave exported bytes, visible state, counters, and stacks
unchanged.

For value-returning operations, constructing the caller's result is part of
the transaction. A prepared candidate remains unpublished until the return
object is constructed; a scope-success guard publishes only on normal exit.
Explicit copy prvalues avoid relying on optional return-value optimization or
on allocation-free library moves. This matters in MSVC Debug, whose string
and vector moves may allocate iterator proxies even when declared `noexcept`.
The isolated return-safety regression arms an allocation failure at the final
prepublication hook, verifies unchanged state after a rejected return copy,
then retries successfully. All six cases pass in Debug and Release with
`/Zc:nrvo-`; evidence is retained in
`artifacts/reviews/authoring-return-safety-v3/`.

Checkpoint restoration has a separate publication regression that exposed
allocations in Debug option moves and retired JSON DOM destruction. Fieldwise
string swaps avoid allocating iterator proxies; the session retains validated
extension JSON text so retired metadata disposal does not invoke the JSON
library's allocating traversal stack. Parsing occurs when producing a public
checkpoint, before any publication. Extension JSON values and serialized output
are preserved; as with a wire round trip, nonnegative signed integers may parse
as unsigned DOM numbers. Internal numeric storage tags are not preserved.

The restore probe arms allocation failure immediately before publication and
checks complete replacement, including nested extensions and retired undo/redo
history. The original failing evidence remains in the integrated v24c Debug log
and `artifacts/reviews/restore-publication-debug-red-tests.log`. Focused repair
evidence is in `artifacts/reviews/restore-metadata-*-v*-tests.log`. This covers
publication and retired-session disposal, not universal persistent-out-of-memory
safety during JSON parsing or candidate construction. The six earlier
value-return tests remain distinct from this restoration case.

`reset` clears semantic actions and local history while retaining the identity
namespace and high-water behavior. A reset after any ID allocation therefore
has to be persisted. An unpersisted cancel may abandon the session without a
recovery row. A persisted cancel or discard is a reversible workspace
lifecycle event and cannot silently disappear. Recovery omission is allowed
only for a never-persisted session with initial counters, empty semantic,
action, and redo histories, no classification or pending dimension, and no
meaningful pointer state. Classification-only sessions, fully undone sessions
with redo data, and sessions with allocated identities remain recoverable.

Known checkpoint variants are strict: malformed phases, duplicate identities,
nonfinite points, invalid counters, extra fields, and resource-limit
violations reject. Unknown positive checkpoint or replay versions remain
opaque and make the containing archive read-only; they are never partly
replayed.

## Source binding

`document_authoring_source_digest_v1` is a separate canonical digest from
`document_snapshot_digest`. Its canonical object includes the explicit domain
`vertex.authoring-source`, version `1`, document ID, head revision,
the complete retained history and entity/asset serialization, and named
revisions using the same byte contract as the full snapshot digest. It omits
only saved-revision bookkeeping, derived editability or read-only reason,
storage version, and recovery metadata.

Each checkpoint stores this digest, the source revision, and the exact
`DrawingContext` for the recorded layer. A checkpoint is current only when the
document ID, source head revision, source digest, drawing context, and
editability all match. Returning to identical geometry through a different
undo or redo revision remains stale; an identical retained semantic history
is current. A save-marker change alone does not make it stale.

A stale checkpoint remains recoverable but cannot finalize until an explicit
rebind. Rebinding uses a fresh identity namespace and fresh IDs, records the
old-to-new correspondence and prior source digest, and rejects direct
same-ID refinalization. Missing or mismatched context is stale rather than
silently repaired. Forged retained history under the same document ID and
revision fails the digest check.

The typed `BoundaryRecoverySource` capture/inspection adapter is implemented in
`boundary_recovery_source.hpp`. Capture requires an editable source and a
complete, exactly resolved drawing context. Inspection returns an explicit
current/stale/read-only classification without changing the document or
repairing context. Focused Debug/Release tests cover save-marker invariance,
foreign identity, changed content under the same revision, undo lineage,
missing/mismatched contexts and read-only content. This value is provenance,
not authorization to commit; the workspace must enforce it when finalizing.
Archive encoding and aggregate ledger validation are implemented. Explicit
rebind and desktop enforcement remain pending.

## Recovery archive and storage format v4

The planned persistence value is `ProjectArchiveSnapshot`, containing an
immutable `DocumentSnapshot` and a `RecoveryLedger`. A nonempty ledger selects
storage format v4. V4 retains the existing five metadata/document tables and
adds exactly one SQLite `STRICT` table:

| Table | Columns and rule |
| --- | --- |
| `project_recovery_records` | `record_id TEXT PRIMARY KEY`, `record_kind TEXT NOT NULL`, `envelope_json TEXT NOT NULL` |

Both `PRAGMA user_version` and `metadata.format_version` are `4`. A v4 file
with zero recovery rows is malformed. Rows sorted by `record_id`, together
with their parsed canonical JSON values, enter the logical digest. Existing
JSON/resource limits apply before graph or blob allocation. The aggregate
digest covers recovery values as well as document values.

The only known v4 record kinds are:

| Record kind | Purpose |
| --- | --- |
| `boundary_active` | One persisted active session checkpoint plus its source context. |
| `workspace_history` | One baseline fence, inline lifecycle event history, current generations/epoch, and event undo/redo stacks. |
| `recovery_copy` | One recovery-archive identity and save/recovery anchors, required for a recovery-copy role and prohibited in an ordinary named archive. |

There are no separate top-level finish or discard records. Finish and discard
are typed immutable event variants inside `workspace_history`. At most one
record of each known kind is allowed. A
recovery archive carries `recovery_copy` and its `workspace_history`; it may
also carry `boundary_active` when a live boundary exists. Record combinations
are validated against the archive role and lifecycle graph. No orphan payload
rows exist.

`boundary_active` holds the strict checkpoint and source binding described
above. `workspace_history` holds the only event order and lifecycle payload
owner. `recovery_copy` carries an archive role, owner token, original document
identity, optional source path and hash, optional
`explicitly_saved_document_revision`, explicit-save generation, edited and
checkpoint generations, autosave and saved watermarks, and ownership
metadata. The explicit-save revision is nullable: `null` means the source has
never been explicitly saved. Load validates a non-null revision against
retained document history and preserves it; it is never normalized to the
autosave head.

The version-one `recovery_copy` envelope uses positive integer `version` and
`replay_version` discriminators, the literal role `recovery_copy`, and required
fields `archive_id`, `owner_token`, `document_id`, `source_path`,
`source_sha256`, `explicitly_saved_document_revision`,
`explicit_save_generation`, `workspace_epoch`, `edited_generation`,
`checkpoint_generation`, `autosaved_checkpoint_generation`,
`saved_edited_generation`, `ownership`, and `extensions`. Optional values are
represented explicitly as null. Source path and hash are independently
optional provenance; neither implies a successful explicit save. The optional
explicit-save revision must occur in the associated document's retained
history. Decoding or checking the record never marks that document saved.

Generation values are unsigned-range integers. Autosaved checkpoint generation
cannot exceed checkpoint generation; saved edited generation and captured
explicit-save generation cannot exceed edited generation. No additional
ordering between epoch, edited and checkpoint counters is inferred from their
numeric magnitudes. Ownership metadata is an opaque object for diagnostics,
not evidence that a kernel reservation is held. Path strings are provenance
only; the codec does not open or resolve them.

The `recovery_copy_record.hpp` codec and document-anchor validator implement
these record-level checks. Focused Debug/Release tests preserve never-saved and
older retained explicit-save anchors without changing dirty or read-only state;
they also cover independently nullable provenance, integer limits, malformed
UTF-8, bounded opaque versions and caller-supplied resource policies. Evidence
is in `artifacts/reviews/recovery-copy-*-v*-tests.log`. This does not yet
implement autosave scheduling, aggregate persistence or acknowledgement routing.

The version-one `boundary_active` envelope has exactly `version`,
`replay_version`, `source`, `checkpoint`, and `extensions`. Both outer versions
are positive integers and currently equal one. `source` has exactly
`document_id`, `revision`, `authoring_digest`, and `context`; context has exactly
`property_id`, `building_id`, `floor_id`, and `layer_id`. Identifiers are at most
128 UTF-8 bytes, document ID is nonempty, revision is an unsigned-range integer,
and the digest is 64 lowercase hexadecimal characters. Empty context strings
remain representable so missing-context recovery is not silently repaired or
discarded. Such a source cannot be captured as current or authorize completion.
`checkpoint` uses the existing strict session codec, and `extensions` is an
opaque JSON object. The entire borrowed envelope is checked against portable
JSON budgets before copying or semantic decoding. An unsupported positive
outer or nested checkpoint version preserves the whole record opaquely.

The `boundary_active_recovery.hpp` codec now implements this record envelope.
Focused Debug/Release tests cover both authoring modes and undo/redo, exact
source and extension round trips, strict fields and discriminators, numeric
limits, opaque future schemas, and whole-record resource rejection. A separate
integration test captures a real document binding, round-trips it with a draft,
and checks save-stable versus stale-source behavior. Evidence is retained in
`artifacts/reviews/active-recovery-*-v*-tests.log`. The codec is now exercised by
the recovery-aware v4 aggregate archive, while desktop restart recovery remains
an integration gate.

This record codec does not establish archive validity by itself: record
uniqueness, workspace-history relationships, archive role and the aggregate
digest require enclosing validation. The internal `recovery_ledger.hpp`
validator now implements uniqueness, relationships, role handling, document
anchors, shared-counter agreement and aggregate resource admission. The
recovery-aware v4 archive persists that validated ledger with a canonical
aggregate digest through the locked atomic publication path. See
`recovery-ledger.md` and `project-archive-v4.md` for the wire contracts. Source
freshness is checked separately against the target document; stale records
remain readable. Live-workspace admission, long-lived ownership and desktop
routing still need to use these core boundaries.

Known record envelopes validate positive integer schema and replay versions,
required fields, field types, references, and resource limits. Missing,
noninteger, zero, or negative version fields in a recognized envelope reject.
An unknown record kind or unknown positive envelope/replay version is parsed
only generically under the same resource limits, preserved as an opaque JSON
value, and makes the entire archive read-only. It is not partially decoded,
silently dropped, or resaved through a guessed schema. Exact JSON value
semantics are preserved; source whitespace and object-key order are not a
format promise.

The document-only load route refuses every v4 archive rather than returning
an editable document stripped of its ledger. Document-only save refuses to
overwrite an existing v4 destination even when the destination CAS hash is
correct. Recovery-aware aggregate load/save is the only route for v4. There is
no generic bypass for dropping unknown records.

The current document-only writer now checks the SQLite user-version marker
through the hash-verified destination handle before making a backup or
publishing. Versions 4 and higher are refused even with a correct expected
hash. A regression demonstrates the previous overwrite and checks unchanged
bytes and absence of staging/backup remnants after rejection. The separate
aggregate v4 routes now perform complete ledger validation and atomic
publication, while this document-only protection remains in force.

## One history authority and lifecycle operations

The in-memory workspace now implements document commands, activation, discard, finish,
undo/redo and semantic redo-clear barriers through one candidate bundle. Its
snapshots carry navigation and lifecycle events alongside the document-event
projection. Seven focused tests pass in Debug and Release. Cross-operation input
sharing was corrected after a failing retention regression; repeated discard and
undo activation now reference the same immutable semantic input, with exact
pointer overrides. Reused session namespaces must be restored through navigation,
not submitted as new activations. Finish commits the owned completed checkpoint
in one Document revision. Undo retains its exact input as retired; redo restores
the original geometry and dimension IDs without recommitting. Activation
navigation preserves retired status, including multiple retired inputs across
branches. Retired input cannot be submitted as an ordinary new activation.
Explicit Revise Input now validates and replays retired input with a fresh
namespace, rebinds the original context to the current document, and records
the origin namespace and finish event on the new activation. It preserves the
original retired view and rejects replacement of an unresolved active session.
Aggregate history budgets, persisted generation checks and disk codecs remain
unimplemented. The runtime types are not a certified persisted schema.
The first validation prerequisites now exist separately: lifecycle order checks
replay navigation and require an exact complete Document-event projection;
archival checks validate canonical payload owners, direct backward references,
pointer overrides and preceding finish provenance. Neither entry point alone
validates the active/retired slot state machine, historical source bindings or
aggregate limits, and neither grants permission to load an archive.
Historical source validation additionally checks the complete retained document,
the original revision digest and that revision's resolved drawing context.
Workspace source validation applies it to activation, archived and active input,
requiring activation to bind its event revision and prohibiting future sources.
Later movement or deletion of a layer does not invalidate genuine historical
provenance, but the existing current-source check still rejects it for finish.
The combined slot validator now invokes these prerequisites, replays active and
retired slots, enforces observed counter floors and restoration provenance, and
reconciles final state with every reachable redo operation. It accounts for
unrecorded pointer/semantic updates while their original source was current,
including updates immediately before a later document edit made that source stale.
Finish validation reconstructs the retained document prefix and compares the
canonical boundary result with the actual saved delta. Prefix reconstruction
preserves historical read-only restrictions; later unsupported history does not
retroactively invalidate a previously permitted finish. This still grants no
archive-load authority without aggregate admission, generation and wire checks.
The aggregate typed-state preflight now precedes combined validation through
`validate_workspace_recovery`. It bounds event/input/action counts, estimated
JSON storage, strings/values, retained document rows and asset bytes, and a
conservative repeated-validation work charge. Immutable input objects are counted
once; active copies and distinct reference payloads are also charged. Defaults
include 10,000 events, 1,000 input objects, 100,000 actions, 64 MiB estimated JSON,
2,000,000 JSON values, 16 MiB strings and 20,000,000 construction-work units.
Document limits include 10,000 revisions, 250,000 entity rows, 100,000 asset rows
and 512 MiB of asset bytes across retained revisions. The combined work ceiling
is 200,000,000 accounting units. These interacting ceilings are engineering
limits, not promises that every configuration at an individual ceiling fits,
nor an RSS or timing certification. They do not replace wire-parser limits.
Raw measurement checks borrowed fields before constructing JSON, rejects action
fields incompatible with their kind, and counts serialized bytes through a
bounded sink rather than allocating the entire encoded string. Generation and
wire-format checks remain required before archive loading.
Opaque extension representations are compared through canonical JSON for
sharing and no-op classification. Regressions reject conflating integer `1`
with floating `1.0`, both in the active envelope and nested checkpoint
extensions, and verify exact restoration after discard.

Implementation detail for the lifecycle ledger: baseline command
references must distinguish an original command revision from a physical
Document snapshot target. Undo/redo append revisions, so `source_revision` may
name a navigation snapshot rather than the original command. The implemented
`derive_workspace_baseline_navigation` replays the validated retained prefix
and pairs stable command identities with both physical baseline stacks. Tests
include imported redo, repeated navigation, later branches, named revisions and
ordinary edits whose messages are "undo" or "redo". The persisted
workspace-history codec and core archive-restoration API now carry this
validated lifecycle state; desktop workspace-command routing remains pending.

The runtime lifecycle history uses typed baseline-command references and post-fence
event IDs. First session activation is an undoable lifecycle operation; otherwise
undoing an older discard could overwrite a newly started session. A session
slot must distinguish active input from retired finish input. Undo activation
archives the exact removed slot, and redo restores its status, so navigating
activation around an undone finish cannot turn retired input into active input.

Local checkpoint updates do not retain a full checkpoint in every global
event. The current active checkpoint already contains local normalized history.
A semantic update appends a constant-size, non-undoable redo-clear barrier only
when global redo is nonempty. Pointer-only updates need no ledger event and
preserve redo. Global redo is authoritative even when Document retains an
inaccessible older physical redo prefix; callers must not fall back to raw
Document redo availability after a workspace branch has been abandoned.

Full lifecycle inputs are retained only when an execution removes a previously
unarchived semantic state, such as discard, finish or undo activation. Repeated
restoration uses a backward reference directly to its full input owner. A
pointer-only change before redo discard records a pointer override, including
explicit absence, rather than another copy of the action timeline. These
owners and references remain inline in `workspace_history`; no additional
recovery row or second history authority is introduced. Validation must reject
missing/forward/wrong-session references, enforce restored slot status and
counter floors, and reconcile the final active checkpoint with reachable redo
obligations. These lifecycle payload and transition rules are design details
for implementation, not an already supported wire format. Epoch/generations
cannot be inferred from global event count because checkpoint updates and save
acknowledgements also advance them.

`workspace_history` begins with an immutable baseline fence containing the
document ID, baseline head revision, source digest, and exact Document undo
and redo revision-ID stacks. Existing Document history is imported in its
original order as baseline state; imported entries are not duplicated as new
commands, and the workspace ledger does not duplicate baseline entity
snapshots. Every post-fence workspace event has an immutable event ID,
monotonic sequence, before/after revision and command lineage where
applicable, and an inline typed lifecycle payload.

The `WorkspaceHistoryFence` capture/validation value is implemented. It stores
only identity, baseline revision/digest, and exact navigation stacks. Validation
first invokes the existing complete Document validator, then compares the
retained baseline and its historical source digest. The historical digest
serializes the retained prefix and only names introduced through that revision;
later names, edits and save markers do not invalidate the original fence.
Frozen current-head digest vectors remain unchanged. Focused Debug/Release
tests compare historical hashes with genuinely captured snapshots and reject
modified stacks, altered bindings and invalid later document history. Evidence
is in `artifacts/reviews/historical-digest-*-green-tests.log` and
`artifacts/reviews/history-fence-*-green-tests.log`. This implements baseline
validation only; event encoding, transitions and the enclosing ledger remain
required. Full validation currently uses a temporary Document fork, not an
additional persisted copy of baseline entity/asset state.

The workspace owns global undo/redo ordering. Local draft undo remains inside
the session. Crossing the baseline fence delegates to the exact Document undo
or redo operation, records the navigation event, and validates the resulting
Document revision stacks. The ledger also stores current epoch/generations
and event undo/redo event-ID stacks. Active-only archives require this history;
recovery copies with no active boundary carry it too. Load validates the
baseline against the retained revision, every event and stack transition
against retained Document history, event links, source links, stack
exclusivity, and ordering.

New work after undo clears reachable redo while retaining immutable lineage for
validation. Reopening with a nonempty Document redo stack must preserve it.
Finish, discard, document edits, and navigation therefore remain reversible
in one ordered workspace history instead of competing row types.

Finish validates a cloned document and ledger, performs the existing sealed
one-command boundary commit, removes the active state, adds the finish lineage
event, and publishes the candidate as one workspace generation. A failed
finish changes nothing. Undoing finish invokes exact Document undo and exposes
the archived input as a retired, non-finalizable view; it must not intercept
global redo as an ordinary active session. Redo restores the original IDs.
`Revise Input` explicitly rebinds and replays into a fresh namespace and
records provenance. Direct recommit using retired IDs remains rejected.

Discard archives the active checkpoint as a reversible lifecycle transition;
undo restores the exact session IDs, phase, history, counters, and pointer.
Cancelling an already persisted session follows the same durable lifecycle
rules. Cancelling before the first persistence acknowledgement may omit a
recovery row under the pristine rule above.

## Generations, anchors, and publication acknowledgement

The workspace tracks independent state dimensions:

| State | Advances when | Does not mean |
| --- | --- | --- |
| `workspace_epoch` | Every authoritative state mutation, including semantic/lifecycle changes, pointer checkpoints, save acknowledgements, and ownership/path changes. | A saved or edited generation. |
| `edited_generation` | An explicit-save-relevant document or workspace change. | That the change has reached disk. |
| `checkpoint_generation` | A semantic/lifecycle change or pointer checkpoint. | A document undo entry for pointer movement. |
| `autosaved_checkpoint_generation` | A matching recovery publication completes. | That the named source is clean. |
| `saved_edited_generation` | An exact current explicit-save acknowledgement. | That every later checkpoint is saved. |

Each watermark is bounded by its corresponding current generation. An
explicit-save generation captured in a publication is a fact about that
publication; it is not a substitute for the current epoch or either current
generation. Pointer movement advances epoch and checkpoint generation,
coalesces recovery scheduling, and leaves explicit-save dirty state unchanged.
The implementation must flush a pointer-only update within the proposed
maximum 30-second interval even when no later semantic action occurs. A move
immediately before power loss is not presumed durable.

Named projects and untitled projects both autosave the complete aggregate to
an owned per-user LocalAppData recovery archive. Autosave never overwrites the
named saved source. The recovery archive preserves the optional explicit-save
anchor, source path/hash, document identity, and all current watermarks. A
never-saved untitled project remains never-saved after recovery; a successful
autosave does not invent an explicit-save revision. Ordinary named publication
retains the existing saved=head behavior when an exact current explicit save
is acknowledged. An autosave, recovery load, or stale completion never
clears named-source dirty state.

Each immutable save publication captures its role, owner token, document ID,
target path, epoch, edited generation, checkpoint generation, and source
digest. Completion processing distinguishes two facts:

1. The file that finished is a valid published archive at its captured
   generation.
2. The completion may acknowledge current workspace state only if its token,
   role, document ID, path, epoch, and captured generations still match the
   workspace.

An older successful publication remains a useful older file, but it never
clears current dirty state or advances a newer watermark. It schedules the
current generation again when needed. Save bookkeeping never changes the
semantic authoring source digest. Explicit Save, Save As, and recovery cleanup
are barriers in the same serialized I/O queue as autosave: an older autosave
may finish before a barrier, never publish after that barrier's cleanup, and
must be retained or requeued when newer edits or checkpoints exist. A
successful explicit save followed by cleanup failure is reported as saved
with a recovery-cleanup issue; it is not reported as unsaved and does not
authorize deletion of another generation.

The initial scheduling candidate is a two-second quiet debounce with a
30-second maximum dirty interval, subject to large-history latency and disk
growth measurements. Explicit Save and close capture the latest pointer.
One dedicated worker serializes immutable archive I/O; the GUI owns all
workspace generations and session mutation. Shutdown stops new scheduling,
captures the current generation, drains or reconciles queued publications,
and joins the worker before destroying the owner. A failed final flush keeps
the UI alive with retry/discard choices and never reports success merely
because the worker joined.

## Windows ownership and archive roles

The current v4 broker repair is recorded in the local
`artifacts/reviews/ownership-broker-v4/` snapshot. Its dedicated-thread unit and
real subprocess tests pass in Debug and Release. This broker checkpoint does
not implement the filesystem resolver or archive coordinator described below.

The workspace owner sits above the existing `ProjectStore` save guards. It
reserves a normalized destination name. If the destination exists, ownership
also reserves its volume and Windows `FILE_ID` with a process-lifetime kernel
mutex. For a missing Save As destination, it reserves the normalized parent
and final name. Diagnostic owner metadata may contain a random session token
and PID, but the kernel-held reservation is authoritative: a live lock is
never stolen or killed based on age or PID, and a crash releases the handle.

This ownership layer reuses `ProjectStore`'s handle-based path and identity
resolution, destination hash/CAS, scoped save mutex, staged validation, and
same-volume atomic replacement. It must not hold the project file exclusively
for the process lifetime. During replacement it retains the prior identity
reservation, acquires and validates the replacement identity, and only then
releases obsolete ownership. Staging remains on the destination volume.
Cross-volume Save As is a new explicit publication and does not rename or
delete the old source.

Case variants, extended paths, DOS aliases, hardlinks, replacement identity
changes, missing destinations, recovery aliases, restart, and failed
ownership transfer are explicit tests. A second open through an alias offers
read-only handling or an independent copy. Cleanup acts only on exact owned
files after a verified promotion or explicit discard; age alone never deletes
unsaved work.

Lifetime reservations are managed by one process-wide
`WorkspaceOwnershipService` with a dedicated owner thread. That thread alone
creates, waits on, releases, and normally closes the lifetime mutex handles. Workspaces
and the archive I/O worker receive opaque reservation IDs and transfer tickets;
they never receive ownership of those handles. The service outlives all
workspaces and the publication worker.

The owner thread also holds a registry of typed path and file-identity keys.
It rejects a key already reserved by another workspace before an OS wait.
Ensuring an existing key for the same reservation is idempotent. This registry
is necessary because Windows permits recursive mutex acquisition by the same
thread: a successful wait alone cannot distinguish two workspaces in this
process. Key bundles are deduplicated, acquired in deterministic order with
zero-time waits, and rolled back on that thread if any acquisition fails.

Lifetime names use separate `Global\\Vertex.Owner.Path.*` and
`Global\\Vertex.Owner.File.*` namespaces. They never reuse the existing
`Global\\Vertex.Save.*` namespace: the synchronous storage publication
continues to acquire and release its scoped save mutex on the I/O-calling
thread. Lifetime ownership authorizes that publication but does not replace
its destination CAS or staged-file guards.

Identity transfer retains the prior keys until the replacement identity has
been reserved and its pathname revalidated. A failure after file publication
preserves the published file and all still-owned reservations, disables
writable continuation for the conflicted target, and requires recovery or
read-only handling. It never authorizes deletion or another overwrite.
The same rules cover pending Save As destinations and queued completion events.

Shutdown rejects new acquisitions/transfers, drains and joins the publication
worker, reconciles pending transfers, acknowledges release of every workspace
reservation, and then stops and joins the owner thread. In normal or retryable
cleanup, every successful wait is balanced by one release on that thread.
A failed public shutdown retains exact owned or close-only obligations for
retry. If terminal destruction or an unexpected owner-loop exit cannot release
a mutex, owner-thread termination abandons it. Only after joining that thread
may the joining thread make a final close-only attempt; it never calls
`ReleaseMutex` on the dead owner's behalf. A persistently failing close remains
an explicit cleanup limitation, not a successful release acknowledgment.
An unexpected owner-thread failure
makes all subsequent editing and publication fail closed; reservations are not
silently recreated. `WAIT_ABANDONED` records an abandoned-owner observation
alongside acquisition and requires archive/role/hash/identity validation before
writes. It does not permit cleanup or stealing based on a PID.

Tests must cover same-process duplicate workspaces, idempotent ensure,
thread-affine release, partial acquisition rollback, coexistence with the
scoped save mutex, real cross-process alias conflicts, abandonment, replacement
transfer ordering, post-publication transfer failure, and shutdown while I/O
is active. A local Windows mutex probe confirmed recursive same-thread waits
and rejection of release from another thread; it does not substitute for those
service and integration tests. The existing scoped `DestinationSaveMutex`
keeps its current synchronous role.

Validated archive entry points carry an `ArchiveRole` of `ordinary` or
`recovery_copy`. The role is checked against typed records; changing a file
extension or renaming a recovery file cannot turn it into an ordinary project.
Ordinary Open routes a recovery archive to recovery or independent-copy
handling. An unknown or role/record mismatch is read-only and cannot be made
editable by changing an extension or normalizing metadata. A recovery copy
never adopts or overwrites its recorded source path automatically; a source
hash mismatch requires an explicit recovery choice or independent copy.
Promotion is the only role-changing transaction.

Supported archives may be made into an editable independent copy with a fresh
document ID, explicit origin lineage, explicitly rebased draft source
bindings, and fresh session IDs where required. An opaque archive or unknown
version is never deserialized and resaved as an editable project. Its exact
original bytes may be copied only through a hash-checked exact-byte API; the
copy retains its embedded identity and read-only status. Failed publication
or promotion preserves both the source and the existing recovery archive.

## Desktop contract

Both workspace tabs use the same authoring session and drawing context.
Switching tabs preserves unfinished work; changing drawing method does not
change workspace identity. The canvases receive copied analytical draft
geometry, labels, anchor/pen markers, and phase state from the session. They
do not own a second list of authoritative points. A desktop click uses the
same effective model-space point as the cursor and pending dimension; snap
changes recompute that point without requiring pointer motion.

The desktop routes every edit, document undo/redo, save acknowledgement, and
project replacement through `ProjectWorkspace`. Local draft undo is separate
from document/workspace undo. Replacing the document or drawing context
invalidates a prepared commit preview and resolves unfinished work explicitly
through preservation, completion, or confirmed discard. Restoring a
checkpoint never completes a chain implicitly. Transient drafts remain out of
print and export.

Recovery UI presents validated candidates before adoption after restart. It
does not hide stale or unknown entries, silently discard them, or adopt a
recovery copy's source path without an explicit choice. Save/open, Save As,
recovery promotion, and close use the barrier and ownership rules above.

## Implementation sequence

The implementation must proceed in dependency order. Each package is an
internal checkpoint and requires its focused tests before the next package
depends on it.

1. **A1: session codec and source digest.** Add the typed checkpoint value and
   strict codec; normalize and record semantic actions; implement candidate
   replay, counters, local history, pointer restoration, reset, cancel, and
   exception safety; add the source digest and exact context binding. Cover
   both modes, all constructors and phases, no-ops, open chains, pending
   dimensions, redo tails, abandoned counter gaps, malformed input, and
   fault-injection invariants.
2. **A2: aggregate, store, and coordinator.** Add the immutable aggregate
   snapshot and workspace owner; implement v4 table/marker/digest validation,
   the three known record kinds, inline lifecycle history and baseline fence,
   `Document` fork/restore validation, prepare/commit CAS, global ordering,
   archive roles, opaque read-only handling, exact-byte copying, and Windows
   path plus `FILE_ID` ownership above existing CAS publication. Demonstrate
   unchanged v1-v3 behavior and actual retained-v3-reader rejection of v4.
3. **B: desktop recovery.** Route all mutation through the workspace; restore
   one shared session in both tabs; add named-source-preserving autosave,
   generation/epoch acknowledgements, restart recovery, Save As, promotion,
   cleanup barriers, owner conflicts, recovery UI, and clean shutdown. Verify
   stale worker completions, queued Save As/autosave, cleanup failures, and
   process interruption at publication barriers.
4. **End-to-end and production qualification.** Run the full Debug and
   Release suites, actual-reader fixtures, controlled interruption tests,
   large-history measurements, alias/ownership tests, and desktop acceptance
   fixtures. Reconcile every requirement and gate in the production plan;
   passing an internal package does not certify the product.

## Verification matrix

| Area | Required evidence | Current status at this checkpoint |
| --- | --- | --- |
| Session state and replay | Exact encode/decode/re-encode; both modes and every phase; all receipt constructors and point construction; semantic no-op, open-chain, pending-dimension, classification-only, fully-undone, reset, cancel, pointer-only, redo-tail, branch, and abandoned-counter cases. Faults leave state unchanged. | Core checkpoint/replay and aggregate ledger codecs pass focused Debug/Release tests; desktop durable recovery and full parity certification remain open. |
| Counter and identity safety | Four next-ID values reject zero, overflow, decrease, collision, namespace mismatch, and inconsistent allocation; valid forward gaps survive; high-water values dominate the entire retained timeline. | Required; not a production-certified result. |
| Source binding | Save-marker invariance; exact document ID/revision/digest/context; forged same-ID history rejection; undo/redo staleness; explicit fresh-ID rebind and provenance. | The source-digest helper and focused Debug/Release save/reopen checks pass; A1 remains underway for session codec and recovery integration. |
| V4 archive and compatibility | Exact STRICT table and metadata markers; nonempty-ledger rule; canonical digest; duplicate/missing/empty/malformed/oversize records; unknown kind/version preservation; document-only v4 refusal; current-reader round trip; actual retained-v3-reader rejection; v1-v3 fixtures unchanged. | Internal v4 save/load and regression tests implemented; frozen pre-v4 reader rejects both roles. Exact-byte opaque copying and ownership/desktop integration remain open, so A2 is not complete. |
| Workspace history | Baseline with both Document undo and redo stacks; fence crossings; event IDs/order/links; finish and discard inline events; mixed edit/discard/undo/redo; nonempty redo on open; branch retention; no orphan payloads. | Core history codec, aggregate validation and `ProjectWorkspace::restore_archive` pass focused Debug/Release tests; desktop routing remains open. |
| Mutation and commit authority | All mutation enters through workspace commands; const/snapshot access only; stale identity/epoch/full-digest candidate rejection; one generation per successful operation; opaque/read-only archives reject every edit path. | Core workspace CAS and read-only/opaque boundaries are implemented; desktop migration is open. |
| Anchors and asynchronous saves | Nullable explicit-save anchor; named-source preservation; untitled never-saved recovery; separate epoch/edit/checkpoint generations; stale completion remains a valid old file but cannot acknowledge current state; queued barrier, cleanup failure, shutdown, and retry cases. | Core save acknowledgement, FIFO worker queue, owner-thread autosave policy, stale-result handling, and ownership-verified Save As cleanup/rebasing pass focused tests; restart integration remains open. |
| Ownership and roles | Case/path/hardlink/DOS/extended aliases; volume plus `FILE_ID`; replacement identity; missing Save As; restart/crash lock behavior; recovery aliases; ordinary versus recovery role; promotion and exact-byte opaque copies. | v4 role validation, CAS publication and archive identity checks are implemented; long-lived Windows ownership, promotion and exact-byte opaque copies remain open. |
| Desktop recovery | Both tabs share one session; source/context replacement invalidates candidates; autosave/reopen/finish/discard; recovery UI; pointer-only flush; process interruption; no transient output leakage. | Guarded v4 open/save, entity/lifecycle command routing, and failed-open preservation pass focused tests; constraint/boundary mutation migration, recovery UI, autosave and restart recovery remain open. |
| Real qualification gates | Physical power interruption, filesystem/filter-driver and disk-full behavior, multiprocess alias qualification, large-history replay/save/input latency, backup growth, clean-machine packaging, and full requirement audit. | Open production gates; model agreement and focused tests are insufficient. |

## Remaining production gates

Durable recovery is only one part of the full production goal. Exact Apex
keyboard and dimension-placement acceptance fixtures, receipt-preserving move,
dimension-edit, reversal, split, and merge derivations, source-edge
provenance, and complete desktop workflow parity remain required. The current
v24q internal construction and storage evidence does not establish those
requirements.

The recovery implementation also needs measured large-history copy/replay and
save latency, input responsiveness, archive/backup growth, resource-bound
behavior, and the proposed debounce/maximum-flush schedule. Physical power
interruption, disk-full and filesystem-filter behavior, and multiprocess alias
qualification remain separate real-machine gates. Reproducible or
clean-machine packaging, complete notices and licensing, and the complete
requirement audit remain outside the current internal checkpoint.

No implementation package, current v3 archive, focused test, actual older
reader, or independent review can be described as production certification.
Recovery remains incomplete until A1, A2, and B are implemented and the matrix
and real gates above have current evidence. The full application additionally
requires every other capability and gate in the accepted production plan.

The surrounding current contracts are `docs/boundary-authoring.md` for shared
authoring and receipt behavior, `docs/project-format.md` for storage v1-v3
and the existing CAS publication protocol, and `docs/implementation-status.md`
for the current evidence boundary.
