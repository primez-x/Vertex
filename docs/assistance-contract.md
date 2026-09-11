# Offline assistance proposal contract

This module is a bounded data and acceptance contract for ASSIST-001 through
ASSIST-005. It contains no model, OCR, tracing algorithm, natural-language
interpreter, network client, document mutation, or desktop integration. The
production assisted-workflow gate remains open.

All four proposal kinds (tracing, dimension extraction, label placement and
natural language) serialize as `unverified` and require explicit acceptance.
Source reference, normalized source rectangle and confidence are mandatory.
Extraction and language proposals retain original source text. Confidence is
metadata, never evidence that a measurement is correct. Command previews name
the command and affected existing or proposed entity IDs; their bounded JSON
arguments are an envelope, not proof of command-specific type validity.

Assistance starts disabled. Disabling prevents new acceptance requests and has
no document effects. Unaccepted proposals have no route to document mutation.
An explicit acceptance creates only an `AssistanceCommandRequest`, retaining
the complete proposal and its unverified provenance. A future adapter must
resolve its command type into the existing typed command API, validate all
parameters, check entity identity and permissions against current document
state, and perform the ordinary undoable transaction. Acceptance does not
authorize skipping those checks or mark inferred measurements verified.
Requests already handed to a caller are plain values, not revocable tokens;
the future dispatcher must recheck current enablement and document revision.

Every producer declares processing libraries and assets with provenance,
license records, and portable package-relative resource paths, or an explicit
omission. Omitted resources remain unavailable even if a caller lists their
IDs. Missing resources are returned in declaration order and block acceptance.
The package loader must establish actual file presence, hashes, containment
after symlink resolution, and license audit results before supplying available
IDs. This module validates declarations; it does not verify files or licenses.

Version 1 JSON uses fixed object fields and stable enum strings, rejects unknown
fields, unknown versions, false acceptance requirements and claimed verified
status, and round-trips deterministically. Validation rejects non-finite
numbers, invalid source rectangles, duplicate resource/entity IDs, remote or
traversing resource paths, and oversized/deep command payloads.

The standalone tests cover all proposal kinds, preserved provenance, default
off and explicit acceptance gates, re-disabling, missing/omitted assets, strict
decoding and malformed inputs. They do not demonstrate offline suggestion
generation, visible previews, actual command dispatch/undo, package audits, or
the full assistance-off draw/edit/calculate/save/print/export workflow. Those
remain required integration and packaged runtime evidence for ASSIST-001..005.
