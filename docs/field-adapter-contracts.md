# Field and appraisal adapter contracts

`field_adapter_contract.hpp` provides local, offline exchange DTOs for
APX-INPUT-002, APX-INT-001 and APX-INT-002. These contracts do not establish DISTO
hardware support or compatibility with an appraisal application. No Bluetooth,
network, process launch or third-party application calls occur. Production
adapters, their caller protocols and representative compatibility fixtures remain
to be discovered and implemented.

## Desktop local reading path

The Windows desktop exposes **Import DISTO reading…** from the overflow menu,
command palette, and quick access. It accepts a version 1.0 JSON envelope from a
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
external mutation. Memory allocation failures propagate. The declared timeout
is 1–120000 milliseconds; it is metadata, not an implemented scheduler. A future
caller must enforce it, report timeout/offline/unsupported-version failures
explicitly, avoid automatic writes after an uncertain response and retain the
original input for diagnosis. Transport cancellation, retry policy, process
isolation and vendor-specific error translation are not implemented here.

The synthetic tests establish DTO validation, deterministic round trips,
selected-field safety and mapping failure behavior only. Exact DISTO hardware,
firmware and transport evidence, appraisal application exchanges, fidelity
reports and real caller offline/timeout tests remain production acceptance gates.
