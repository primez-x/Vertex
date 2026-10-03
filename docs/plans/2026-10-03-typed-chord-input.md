# Typed chord length and heading

The accepted precision workflow needs curve entry from a measured chord, without
requiring the user to calculate world X/Y coordinates. Each chord-based arc form
will offer Length / heading and Endpoint X/Y definitions. Length / heading is the
default for new dialogs; an accepted choice is remembered during a drawing
session and resets with the input-unit basis. Existing coordinate construction
remains available explicitly. Invalid quantities, impossible arcs and cancelled
or obsolete dialogs cannot commit.

Typed length and heading must be durable analytical construction inputs, not
discarded UI values or a second geometry authority. A chord receipt has exactly
one definition: the legacy endpoint or a normalized exact quantity/angle pair.
Replay derives the endpoint from the captured local start. Ordered edits and
transforms retain those original expressions and units.

The core worker owns receipts, session/recovery APIs, linework promotion,
necessary entity/storage/exchange adapters and core regressions. Root owns the
shared native controls, live admission, UI regressions, CMake, scripts, docs,
generators, integration, build/test execution, Git and installed delivery.
Native writers freeze whenever a build or executable check is running.

Receipt version 2 explicitly identifies typed chord input. Boundary schema 4,
measured-linework schema/replay 4 and recovery schema 2 opt in only when necessary;
legacy encodings remain unchanged. Native format 31 and extraction 29 prevent
older readers from silently losing these inputs, including inputs retained only
in history or nested provenance. Unknown versions remain opaque and known
malformed data fails atomically.

Verify missing behavior first, then each chord construction in both unit systems,
signed sweeps/heights, clockwise choice, actual D before the first canvas click,
both definition choices, expressions, correction/cancel/context guards,
Undo/Redo, transformations/edits, recovery, save/reopen, retained-history format
floors and analytical area/output. Inspect native rendering and installed
runtime behavior. Update the practical checklist without marking user tests
passed. Full parity, compatibility and unified production acceptance remain open.
