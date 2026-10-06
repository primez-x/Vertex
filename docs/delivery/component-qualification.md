# D02 component and distribution qualification

This audit records remaining distribution work for the public,
GPL-3.0-or-later, forever-free Vertex application. It is an evidence crosswalk,
not licensing clearance or production acceptance. Existing authorization to
include the supplied artwork remains recorded; this audit introduces no new
permission gate or publication action.

The inspected inputs are the current tracked dependency, source and packaging
manifests. The local `artifacts/runtime/distribution-inventory.json` was generated
at `2026-10-06T00:51:44.194894Z`, covers 113 binaries and 16 static inputs, and
sets `distribution_qualified`, `installer_qualified` and `offline_qualified` to
false. Its component-manifest receipt names `artifacts/runtime/cad-components.json`.
Those historical bytes have not been rebound to the later relocated source
build or installed candidate. New candidate evidence must be generated from
the actual finished Release payload; neither this audit nor an earlier inventory
certifies the installed runtime.

## Existing evidence

- [dependencies.json](../../third_party/dependencies.json) pins SQLite,
  nlohmann-json, Inter and the vcpkg manager. Its production license audit is
  explicitly incomplete.
- [distribution-components.json](../../third_party/distribution-components.json)
  declares 25 application/native/Qt/resource components. Every declared
  `notice_paths` file exists in the inspected workspace. Existence is not a
  completeness or rights finding.
- The generated CAD component manifest adds 13 entries: CPython, eleven Python
  distributions and the application adapter. Archive hashes, original package
  metadata and notice paths are retained; non-reviewed license prose remains
  `NOASSERTION` in the SBOM rather than a fabricated SPDX conclusion.
- [PlaneGCS provenance](../../third_party/planegcs/SOURCE.md) identifies the
  FreeCAD commit, extracted files, local compatibility shims and the export-only
  change, with hashes and the original LGPL notice.
- [SVG provenance](../../assets/symbols/architectural_v2/SOURCE.md) identifies
  the supplied archive by SHA-256, documents public GPL authorization and
  deterministic restyling, and distinguishes independently authored additions.
  The inspected catalog contains 345 SVGs, including 320 imported identities.
- [source-kit-allowlist.json](../../packaging/source-kit-allowlist.json) selects
  1,498 application/build/docs/license/fixture files, including all 350 current
  architectural-library files. The source-kit and distribution tools explicitly
  leave corresponding-source and redistribution qualification incomplete.

## Specific remaining facts and smallest fixes

| ID | Observed evidence or gap | Required completion evidence / smallest fix | Owner |
|---|---|---|---|
| D02-Q01 | The supplied SVG archive had no separate license file. `SOURCE.md` records the project owner's direction to include it as first-party GPL assets, but expressly makes no representation about unidentified third-party rights. Archive identity and permission to perform the requested import are established; independent creator/title or an upstream grant is not recorded. | Preserve the existing authorization and original archive receipt. Add a concise creator/provenance record supporting the right to publish the imported 320 identities under GPL, or the applicable original grant/notice if they originated elsewhere. Identify any exceptions rather than relabeling unknown third-party work. The independently authored additions retain their separate provenance. | D02, project provenance facts |
| D02-Q02 | Qt 6.8.3 base/PDF/SVG entries declare alternative licenses and stage SPDX records as notices. No `LICENSE*` files were found in the inspected Qt prefix. The qtpdf SBOM has two extracted license records and seven packages; it does not by itself prove a complete PDFium third-party notice payload. | Record the chosen open-source route for each shipped module and retain its exact version/source receipt. Stage and inspect the actual GNU license texts and all required Qt/PDFium/static-third-party notices from the pinned source payload; bind them to the shipped module build/options. Verify the final augmented CAD package as well as the base allowlist so duplicated GNU texts are not mistaken for complete third-party attribution. | D02 |
| D02-Q03 | The application source allowlist contains project source, PlaneGCS and dependency metadata, but no `.deps/` source payload. The inspected generated CAD source-kit manifest contains 1,076 selected project files and omits the later IFC source lock, candidate/staging helpers and derivation patch. A metadata-only dependency prefix can satisfy an artifact-presence check without carrying the corresponding dependency sources. | Refresh the project allowlist for the final source batch, including delivery docs/tools. Compose exact Qt/native/CAD dependency source archives, historical recipes, modifications, notices and build controls into the handoff. Verify the payload against the actual runtime and rebuild it offline; do not infer source completeness from the presence of `source-kit/third_party/`. | D02/D10 |
| D02-Q04 | The shipped IfcOpenShell wheel is locked, but its exact publisher-to-source binding and bundled native source closure remain unresolved in `cad-runtime-lock.json`. The differently hashed official catalog candidate is not evidence for that wheel. A separately built/staged source candidate and SDK-source payload exist as historical evidence, not as a qualified replacement runtime. | Select the actual release runtime identity. Either obtain exact wheel/source/native-closure evidence or qualify the controlled replacement, including original/derived source, patch, eight-schema build settings, generated wrapper, static SDK sources and original notices. Bind the replacement to the actual independent worker and distribution inventory before substituting it. | D02/D08 |
| D02-Q05 | CAD wheels contain bundled native code beyond their top-level Python licenses: Shapely retains GEOS and Windows notices; NumPy retains OpenBLAS and uniquely named native DLLs; CPython retains OpenSSL/libffi and other components. The generated entries preserve publisher prose, including `See package notices`, `Dual License` and other non-SPDX declarations. | Review the exact embedded notice/source sets, select any alternative license branches, and record reviewed expressions without overwriting original prose. Provide corresponding source/build inputs where the actual embedded license requires them. Include transitive code embedded in a DLL or extension, not just PE-imported DLL owners. | D02 |
| D02-Q06 | The MSVC runtime manifest explicitly has `licensing_clearance: false`, operator-declared versions and local-evidence-only provenance. Its `Redist.txt` is a 187-byte pointer to the current list; the stored notices do not establish distributor eligibility or the exact licensed redistributable terms. | Record the applicable Visual Studio/Build Tools license basis, the actual applicable redistributable list/terms, and that the five selected non-debug CRT files are covered. Retain the existing byte hashes and third-party notices. Treat compiler/Windows SDK handoff rights separately from runtime rights; a reproducible kit need not silently redistribute an entire licensed SDK. | D02 |
| D02-Q07 | The OCCT copyright file already contains LGPL-2.1 and the Open CASCADE exception, including a prominent supporting-documentation attribution condition. The manifest records only `LGPL-2.1-only`. Existing hashes/notices, Eigen MPL, Inter OFL, and permissive native notices are present but not a reviewed final obligation matrix. | Preserve the exact OCCT exception and add the required release attribution; record the chosen exception-aware expression without discarding the original text. Confirm exact modified-source/relinking obligations for the selected linking forms, Eigen covered files, and font redistribution/renaming facts. Keep all existing permissive copyright/disclaimer texts in the final package. | D02 |
| D02-Q08 | The audit found private-source wording in `third_party/README.md` and a stale 342-file SVG README count. Root corrected these to public GPL source and 345 SVGs in this batch; these prose corrections do not establish a redistribution right. | Preserve actual hashes, authorization and dated proofs. Refresh the source-kit/inventory after the final input batch is frozen; the original count/prose finding is resolved. | Root integration |

## Obligation sources and qualification boundary

The repository's [GPL text](../../LICENSE) defines Corresponding Source to include
the source and controls needed to generate, install and run the work, with its
stated System Library/tool exclusions. Binary delivery needs an applicable
source-delivery route; public repository presence alone does not demonstrate
that a particular runtime's complete corresponding source is available. The
source kit should name that route and the exact candidate inputs.

For Qt PDF, the [official Qt 6.8 licensing page](https://doc.qt.io/qt-6.8/qtpdf-licensing.html)
identifies an LGPLv3 open-source option and requires the included PDFium and
third-party licenses to be respected. The local 6.8.3 module SBOM and actual
source licenses govern the pinned binaries; current documentation cannot
replace their exact notice/source review. Qt's broader
[licensing documentation](https://doc.qt.io/qt-6/licensing.html) likewise identifies
module-specific alternatives and third-party code.

[Microsoft's redistribution documentation](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files?view=msvc-170)
limits Visual C++ redistribution to licensed Visual Studio users under the
applicable terms and directs distributors to the relevant redistributable list.
The five selected DLL hashes and the SDK-directory role are useful technical
provenance, while the eligibility/terms fact remains distinct.

Public GPL source and the project's forever-free pricing commitment are
compatible product commitments. GPL redistribution freedoms, including the
ability of recipients to redistribute commercially, remain intact; do not add
a downstream no-charge restriction to the GPL grant. Third-party components
retain their own licenses and notices.

No component is marked accepted by this audit. Close each row with concrete
candidate-bound source, notice, rights or rebuild evidence. D02 qualification
can continue alongside compatibility acquisition and document authority work;
missing release evidence does not require repeating unrelated implementation
or adding an automatic goal loop.

Checks performed: read-only manifest/notice existence and bounded provenance
inspection; exact artwork count; comparison of project and generated source-kit
selections; official Qt and Microsoft licensing documentation. No dependency
downloads, installations, native builds, external mutations, or production
clearance were performed.
