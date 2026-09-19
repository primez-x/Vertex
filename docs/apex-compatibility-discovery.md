# Apex compatibility discovery

Discovery checkpoint: 2026-09-19. This is source evidence for the compatibility
work, not a tested importer, caller protocol, or certification. The mandatory
requirements remain `APX-INT-001`, `APX-INT-002`, `APX-NATIVE-AX5-001`,
`APX-NATIVE-AX7-001`, `APX-NATIVE-LEGACY-001`, and `APX-COMPAT-001/002` in the
[production ledger](requirements/apex-parity.json).

## Local evidence

The three ordinary Windows uninstall registry locations (machine 64-bit,
machine 32-bit, and current user) contained no display-name match for
`ApexSketch`, `Apex Sketch`, or `Apex Software`. This does not exclude a portable
installation, a differently named product, or an installation on another
computer. No representative native Apex project has been identified in the
current workspace. The retained `.bldproj` compatibility fixtures are generated
by Vertex and cannot serve as evidence of Apex compatibility.

The exact installed edition, executable version/hash, enabled modules, settings,
native source files, and actual caller applications remain to be identified.

## Primary-source findings

| Source | Observed information | Engineering consequence |
| --- | --- | --- |
| [Apex downloads](https://apexappraisalsolutions.com/downloads/) | The page lists v7 Pro build 39305 and Standard build 45588 separately. | Record the actual binary identity for each fixture; the two displayed build labels do not identify the user's installation or establish format equivalence. |
| [Apex online store](https://www.apexwin.com/store) | The v7 product description names keyboard, mouse, digital-pen, finger/touch, and DISTO input as supported workflows. The same page identifies v5 integrations with a la mode, ACI, Bradford Technologies, SFREP, and WCA. | Keep keyboard, pen, touch, and DISTO as separate input fixtures. Treat the listed forms products as compatibility candidates only; capture exact host versions, bitness, exchange files/fields, and return behavior before implementing or advertising an adapter. |
| [Official v7 user-interface reference](https://apexwin.com/support/ApexSketchv7/ApexSketchv7-User-Interface.pdf) | Save As and Export support an older v5.x format; the documented v7 window capture is titled `Sketch1.AX7`, and Import retains the current sketch's subject information and filename. Integrated exit returns to the calling application. Subject information may arrive from that caller, and per-area attributes may be parsed by it. The guide documents generic Bluetooth auto-detection for Leica DISTO meters without naming a model, firmware, transport profile, message protocol, or field-assignment rule. Touch exposes a dedicated panel; enabling Touch disables active-pen inking. | Treat `.AX7` as an observed v7 fixture extension, not as a format specification. Capture the actual bytes/signature and test native open, import-into-current, Save As, and legacy export as separate behaviors. A caller fixture must cover launch, subject/area data, cancellation, close/return, and persistence. DISTO hardware identity and raw exchanges must be observed before selecting a protocol. Pen and touch need separate fixtures plus a transition test because the documented modes interact. |
| [TOTAL's Apex v7 listing](https://totalstore.alamode.com/product/apex-sketch-v7-standard) | Integrated use requires a separate TOTAL integration step in addition to installing Apex. | Standalone Apex execution cannot prove the TOTAL caller/return path. The integration component and host version belong in the fixture identity. |
| [ACI v7 installation instructions](https://www.aciweb.com/kb/apex-v7-install/) | ACI documents an `APEX7` updater step after installing Apex. | Capture the ACI-side integration version and observed exchanges; Apex's version alone is insufficient. The linked historical installer URL redirected to the Apex homepage when followed at this checkpoint. |
| [WhisperReporter Ascent's Apex documentation](https://whispersolutions.com/Help/WhisperReporterV2/apexsketch.htm) | Its 64-bit host supports Apex v7 or later; its 32-bit host also supports older versions. It describes launching an embedded sketch and updating the report database when Apex closes. | Record host bitness and test launch, edit, cancellation, close/return, and report persistence separately. This description does not establish a COM interface, command-line protocol, or wire format. |

The current [official v7 guide index](https://apexappraisalsolutions.com/av7-help/)
links the workflow, shortcut, curve, text, and photometrics references. The
pages inspected here did not provide a native-file schema, a caller SDK, or a
downloadable native sample. This bounded result does not establish that none
exists elsewhere. Native extensions and signatures must be verified from
actual files or authoritative format evidence; similarly named products and
file-extension directories are not sufficient evidence.

## Required fixture capture

Each source fixture needs its original hash, producing application/edition,
executable version and hash, enabled modules, units, relevant rounding and
classification settings, plus expected geometry, calculations, and output.
Preserve originals and perform all conversion and editing on copies. Record
permission to retain and distribute each fixture independently of permission
to inspect it; private customer project data does not belong in distributable
test packages by default.

Caller fixtures additionally need host version/bitness, integration component
identity, launch arguments or observed interface calls, exchanged files/fields,
return behavior, errors, timeout/cancellation behavior, and an offline run.
Observe these values from the actual supported application before defining an
adapter. Public integration listings identify discovery candidates; they do
not automatically add a tested application to the supported compatibility list.

No compatibility requirement changes status as a result of this discovery.
