# Field and appraisal adapter contracts

`field_adapter_contract.hpp` provides local, offline exchange DTOs for
APX-INPUT-002, APX-INT-001 and APX-INT-002. These contracts do not establish DISTO
hardware support or compatibility with an appraisal application. No Bluetooth,
network, process launch or third-party application calls occur. Production
adapters, their caller protocols and representative compatibility fixtures remain
to be discovered and implemented.

## Desktop local reading path

The Windows desktop exposes **Import DISTO reading…** from the overflow menu and
command palette. It accepts a version 1.0 JSON envelope from a
local file or clipboard paste and requires an explicitly selected compatible
field. The current supported targets are wall length/height/thickness/elevation,
opening width/height, slab thickness/elevation, and room height/elevation. The
existing typed editor validates the proposed dimension before the reading is
accepted; a second reading cannot replace an occupied field silently.

Accepted readings are retained under the selected entity's opaque extension as
`disto_measurements.version = 1` with a `fields` object keyed by the declared
target field. Each value is the original validated envelope, including reading
ID, value, unit, capture time, model, firmware, transport, and provenance. The
geometry edit and provenance retention are separate ordinary history entries so
either step can be undone and the project remains inspectable offline. This is a
local adapter boundary, not a claim of Bluetooth or vendor compatibility; exact
DISTO hardware/firmware and any future transport session still require fixture
qualification.

## DISTO reading envelope, protocol 1.0

Each record requires an explicit target field, unique reading identifier supplied
by the producer, positive finite value, original unit (`m`, `mm`, `cm`, `ft`, `in`),
UTC capture timestamp, model, firmware, transport and provenance. Units are
retained without conversion. Timestamps use `YYYY-MM-DDTHH:MM:SSZ` with calendar
validation; leap seconds and fractional seconds are outside this version.

`assign_disto_measurement` accepts only an empty destination whose explicitly
selected field matches the reading. It rejects occupied destinations, including
retries, rather than overwriting. It does not identify fields in a drawing or
deduplicate reading IDs across destinations: the future device session owns
selection, replay detection and deliberate replacement workflows.

## Appraisal descriptor and one-way payload, protocol 1.0

A descriptor identifies adapter, application, exact application version,
exchange format, provenance, timeout and one-to-one field mappings. Every mapping
explicitly marks its source as required or optional. Duplicate source or target
fields are errors. Payload creation requires all required fields, omits absent
optional fields, and rejects unknown or empty supplied values. Values are text;
numeric interpretation, units and fidelity are the responsibility of the
application-specific adapter. A payload embeds its complete descriptor.

`appraisal_contract_compatible` performs exact matching of protocol and target
identity. Version 1.0 supports only one-way text mapping; a future capability or
version must receive an explicit implementation instead of being assumed
compatible. Mapping-array order does not affect serialized output.

## Bounds and failure semantics

Parsers reject malformed JSON, duplicate or unknown object members, incorrect
types and unsupported protocol versions. Input is limited to 1 MiB and nesting
depth 16. Identifiers are 1–128 ASCII letters/digits/dot/underscore/hyphen;
descriptive strings are bounded and reject control characters. Mappings are
limited to 256 fields and supplied values to 2048 bytes each. Serialization uses
sorted object keys and mappings for deterministic compact JSON.

Validation failures throw `std::invalid_argument` and produce no payload or
external mutation. Memory allocation failures during local payload validation
propagate. The declared timeout is 1–120000 milliseconds.

## Appraisal caller dispatcher

`appraisal_dispatcher.hpp` adds a drawing-engine-independent, single-owner
polling runtime around the mapping contract. It accepts the descriptor, original
source values, a unique request ID, a cancellation token and an owned
`AppraisalCallBoundary`. It retains the original input for diagnosis. There is
no drawing/document dependency, destination handle, write callback or retry loop.

The worker first advertises capabilities. Exactly one must match adapter ID,
protocol major/minor, application and exact application version, exchange format,
and every source/target/required field mapping. Mapping order is immaterial;
provenance and timeout are local policy metadata, not negotiated capabilities.
An empty/no-match advertisement is unsupported; duplicate exact matches are
ambiguous. Malformed advertisements and more than 64 capabilities are uncertain.
Only after successful negotiation does the dispatcher send the validated payload
for **preparation**. A success acknowledgement must echo the request ID and exact
payload, with no capability data. Wrong-stage, stale or inconsistent responses
fail closed. This verifies correlation and byte agreement, not appraisal fidelity.

One steady-clock deadline covers local payload preparation, negotiation and
worker preparation. `poll()` checks cancellation and the deadline before and
after transport calls and before accepting success. Equality with the deadline
is timeout; cancellation wins if both signals are observed at a checkpoint.
The budget is never reset after negotiation. Cancellation is sampled at these
checkpoints, not asynchronously after a terminal success. Terminal results are
immutable, the boundary is abandoned exactly once, and late replies cannot
resurrect a timed-out or cancelled request. Destruction also abandons a pending
session. Caller servicing of `poll()` is required; there is no background timer.

Typed outcomes are success, offline, unavailable, unsupported, ambiguous,
timeout, cancelled, worker failure and uncertain response. Transport exceptions
map to worker failure unless cancellation/timeout already applies. Only success
contains prepared data. No outcome authorizes a destination write. In particular,
**timeout, cancellation and uncertain responses must never trigger a destination
commit, automatic retry or delayed write**. All workers behind this boundary
must be preparation-only and have no destination-write authority. Any eventual
commit requires a separate, explicitly designed transaction/reconciliation path.

The boundary is injectable and all its operations, including abandonment and
destruction, must be nonblocking. The dispatcher enforces acceptance deadlines;
it cannot interrupt a boundary implementation that blocks or prevent a dishonest
worker from writing externally. No native subprocess launcher, IPC transport,
OS sandbox, worker kill/reap implementation or vendor adapter is supplied here.
The synthetic boundary is **not proof of process isolation**. A production
transport must provide that isolation and bounded lifecycle independently, encode
and validate wire replies, enforce preparation-only authority, and translate
vendor-specific failures before it can be qualified.

Synthetic tests establish DTO validation, deterministic round trips,
selected-field safety, mapping failure behavior and the dispatcher state machine.
Dispatcher fixtures cover exact negotiation/version mismatch, duplicate selection,
success, offline/unavailable, deadline and cancellation before/during transport
calls, launch/preparation/poll exceptions, malformed/stale/uncertain replies and
late-result rejection. Exact DISTO hardware, firmware and transport evidence,
appraisal application exchanges, fidelity reports and real isolated caller
offline/timeout/crash tests remain production acceptance gates.
