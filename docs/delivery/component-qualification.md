# D02 component and distribution qualification

This audit records remaining distribution work for the public,
GPL-3.0-or-later, forever-free Vertex application. It is an evidence crosswalk,
not licensing clearance or production acceptance. Existing authorization to
include the supplied artwork remains recorded; this audit introduces no new
permission gate or publication action.

## Tracked source-kit inventory correction (2026-10-10)

The explicit source-kit allowlist now includes all 1,889 current tracked paths.
The correction adds 169 omitted implementation and artwork inputs, including
architectural editing/removal/phase modules, desktop dialogs, the architectural
DXF codec and seven door/opening SVG assets. Each addition is an existing tracked
path, classified by the current source-kit policy; untracked and generated files
are not enrolled. JSON inspection found no missing paths, extras or duplicates.

This corrects the manifest input only. No generator, source-kit composition,
build, package, installation or runtime qualification ran. The historical source
kit receipts below remain bound to their original payloads. Complete dependency
sources, notices/SBOM, exact final candidate binding and an offline rebuild remain
required before distribution qualification.

## Precision dependency source overlay (2026-10-09)

The precision target explicitly selects Boost Multiprecision 1.92.0 for the
bounded rational curve-contact fallback. The retained port's SPDX record names
recipe tree `5c3b7779f77b2e9c7997e4b36f6cc19505e4720d`, the upstream
`boost-1.92.0` resource and BSL-1.0. Its original notice and all installed Boost
headers are declared in a separate static-source component. The pinned vcpkg
manifest and exact-version CMake target now name this dependency directly,
instead of relying on Boost Graph's transitive include availability.

No dependency provisioning, compilation, inventory/SBOM generation or source-kit
composition ran for this source change. Existing candidate-bound counts and
receipts below remain historical; the new component and its transitive inputs
require final source-kit/runtime binding. This declaration does not establish
corresponding-source completeness or distribution qualification.

## Selected Qt source and build overlay (2026-10-07)

The current source selects the coherent Qt 6.11.2 Windows MSVC SDK through
`third_party/qt-sdk.json`. Its eight CMake components and the independently
verified SDK/source archive identities replace the earlier 6.8.3 selection.
The actual payload's PE imports determine which modules are distributed;
the CMake component list does not itself assert that every SDK module ships.
The earlier installed candidate and inventory counts below remain historical
and must not be described as this new selected runtime.

The selected Release build and affected Site/editor checks pass. All 345 SVG
assets pass the actual native palette, transparent-selection and output checks;
bounded SVG admission also passes persisted artwork, optional-guide painting
and explicit invalid-output refusal. These observations verify the exercised
paths, not complete parser security or production acceptance.

Current corresponding-source inputs are pinned for QtBase, QtSvg and the
QtWebEngine source archive that supplies QtPdf/PDFium. The QtPdf snapshot at
`third_party/notices/qt-pdf/6.11.2` contains 45 distinct original grant slices
correlated to 77 current source members. The preserved older snapshot is
historical evidence. Source notices and upstream build descriptions still
require correlation with the exact final binary configuration and complete
transitive input set. Open-source module routes and original alternative
license expressions remain recorded separately; this overlay supplies no
licensing clearance.

The pre-integration source allowlist check covered 1,705 of 1,705 tracked inputs.
Further source changes require an updated allowlist and a source-kit/candidate
binding. Rebuild the runtime inventory, SBOM, corresponding-source kit and
installed checks from the final committed payload before cutover. The selected
source/build observations do not rebind or qualify the installed candidate.
All distribution, offline and production qualification flags remain false.

## Historical evidence overlay (2026-10-06)

The current selected runtime inventory contains 127 binaries, 18 static inputs
and 52 components. Its selected-source audit records 49 exact local sources, two
recorded assets, zero missing sources and one outstanding MSVC rights review.
The inventory and audit counts below that describe earlier candidates remain
historical observations.

The source composer successfully produced `.deps/source-closure/dependency-kit-selected`:
19,056 physical files, 1,703,723,555 bytes and 52 components. Its manifest
SHA-256 is
`f31024e0bef574ea6243a41320eba5325969211232be8782a5bf207b6535f542`; it is
bound to project commit `86888988a3e164f3ed4c0941770c35d3e07f8133`
and source-kit SHA-256
`3cba0a5c4a9a5519c740919bf380fa8519563cd1e408023dbf433d9252b42f48`,
covering 1,565 of 1,565 files.
The source-v2 tree and budget corrections were reviewed and approved. This
composition is historical after later desktop and handoff edits; candidate
rebinding, staging, installation, offline rebuild, transitive-native review,
license review and artwork-rights evidence remain open. All qualification flags
remain false.

The OCCT release attribution is now present in the README and third-party
attribution. The upstream SPDX literal remains `LGPL-2.1-only`; the
exception-aware documentation expression is
`LGPL-2.1-only WITH OCCT-exception-1.0`. Recording the exception does not resolve
modified-source, relinking or rights questions.

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
  1,498 application/build/docs/license/fixture files at initial audit, including all 350 current
  architectural-library files. The source-kit and distribution tools explicitly
  leave corresponding-source and redistribution qualification incomplete.

Root's subsequent candidate integration refreshed and verified all 1,510 tracked
application/build/docs/license/fixture files. The identified candidate's source
kit uses commit 387ab41b271a23e0dedb8015a175c77343be4eb9. This closes the stale
application allowlist portion of D02-Q03/Q08; exact dependency source, build,
notice and redistribution obligations below remain open.

The later source batch now has a selected controlled IfcOpenShell runtime and
4,744 materialized original/derived/control source files; their exact identities
and checks are in [ifc-source-build.md](../dependencies/ifc-source-build.md).
The full Qt notice extraction also completed: 2,380 original files totaling
10,640,392 bytes, plus its index and route notice. Those original text paths are
now declared in the Qt component entries and all output hashes were verified.
[qt-notices.md](../dependencies/qt-notices.md) records the pinned inputs and the
100 remaining metadata references. This advances D02-Q02/Q04's actual local
inputs; it does not rebind the historical inventory or certify the installed
candidate. The next inventory/source payload must include these new declarations
and preserve unresolved applicable obligations.

## Specific remaining facts and smallest fixes

| ID | Observed evidence or gap | Required completion evidence / smallest fix | Owner |
|---|---|---|---|
| D02-Q01 | The supplied SVG archive had no separate license file. `SOURCE.md` records the project owner's direction to include it as first-party GPL assets, but expressly makes no representation about unidentified third-party rights. Archive identity and permission to perform the requested import are established; independent creator/title or an upstream grant is not recorded. | Preserve the existing authorization and original archive receipt. Add a concise creator/provenance record supporting the right to publish the imported 320 identities under GPL, or the applicable original grant/notice if they originated elsewhere. Identify any exceptions rather than relabeling unknown third-party work. The independently authored additions retain their separate provenance. | D02, project provenance facts |
| D02-Q02 | Qt 6.8.3 base/PDF/SVG entries declare alternative licenses and stage SPDX records as notices. No `LICENSE*` files were found in the inspected Qt prefix. The qtpdf SBOM has two extracted license records and seven packages; it does not by itself prove a complete PDFium third-party notice payload. | Record the chosen open-source route for each shipped module and retain its exact version/source receipt. Stage and inspect the actual GNU license texts and all required Qt/PDFium/static-third-party notices from the pinned source payload; bind them to the shipped module build/options. Verify the final augmented CAD package as well as the base allowlist so duplicated GNU texts are not mistaken for complete third-party attribution. | D02 |
| D02-Q03 | The application source allowlist contains project source, PlaneGCS and dependency metadata, but no `.deps/` source payload. The inspected generated CAD source-kit manifest contains 1,076 selected project files and omits the later IFC source lock, candidate/staging helpers and derivation patch. A metadata-only dependency prefix can satisfy an artifact-presence check without carrying the corresponding dependency sources. | Refresh the project allowlist for the final source batch, including delivery docs/tools. Compose exact Qt/native/CAD dependency source archives, historical recipes, modifications, notices and build controls into the handoff. Verify the payload against the actual runtime and rebuild it offline; do not infer source completeness from the presence of `source-kit/third_party/`. | D02/D10 |
| D02-Q04 | The shipped IfcOpenShell wheel is locked, but its exact publisher-to-source binding and bundled native source closure remain unresolved in `cad-runtime-lock.json`. The differently hashed official catalog candidate is not evidence for that wheel. A separately built/staged source candidate and SDK-source payload exist as historical evidence, not as a qualified replacement runtime. | Select the actual release runtime identity. Either obtain exact wheel/source/native-closure evidence or qualify the controlled replacement, including original/derived source, patch, eight-schema build settings, generated wrapper, static SDK sources and original notices. Bind the replacement to the actual independent worker and distribution inventory before substituting it. | D02/D08 |
| D02-Q05 | CAD wheels contain bundled native code beyond their top-level Python licenses: Shapely retains GEOS and Windows notices; NumPy retains OpenBLAS and uniquely named native DLLs; CPython retains OpenSSL/libffi and other components. The generated entries preserve publisher prose, including `See package notices`, `Dual License` and other non-SPDX declarations. | Review the exact embedded notice/source sets, select any alternative license branches, and record reviewed expressions without overwriting original prose. Provide corresponding source/build inputs where the actual embedded license requires them. Include transitive code embedded in a DLL or extension, not just PE-imported DLL owners. | D02 |
| D02-Q06 | The MSVC runtime manifest explicitly has `licensing_clearance: false`, operator-declared versions and local-evidence-only provenance. Its `Redist.txt` is a 187-byte pointer to the current list; the stored notices do not establish distributor eligibility or the exact licensed redistributable terms. | Record the applicable Visual Studio/Build Tools license basis, the actual applicable redistributable list/terms, and that the five selected non-debug CRT files are covered. Retain the existing byte hashes and third-party notices. Treat compiler/Windows SDK handoff rights separately from runtime rights; a reproducible kit need not silently redistribute an entire licensed SDK. | D02 |
| D02-Q07 | The OCCT copyright file already contains LGPL-2.1 and the Open CASCADE exception, including a prominent supporting-documentation attribution condition. The README and third-party attribution now include the release attribution. The manifest retains its upstream literal `LGPL-2.1-only`; the exception-aware documentation expression is `LGPL-2.1-only WITH OCCT-exception-1.0`. Existing hashes/notices, Eigen MPL, Inter OFL, and permissive native notices are present but not a reviewed final obligation matrix. | Preserve the exact upstream expression and exception, and confirm exact modified-source/relinking obligations for the selected linking forms, Eigen covered files, and font redistribution/renaming facts. Keep all existing permissive copyright/disclaimer texts in the final package. | D02 |
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

## Executable dependency source-closure audit

`scripts/qualification/dependency_source_closure.py` now verifies a selected
distribution inventory, its runtime/component receipts, every listed binary and
notice, exact source archive checksums, historical vcpkg recipe files, and an
optional frozen offline package. With `--bundle-root`, it also verifies every
package payload hash and uses the package's application source kit rather than
the changing working tree. Explicit build receipts are hashed without copying
their machine-specific contents. Report publication uses a unique sibling file
and atomic replacement; malformed inputs or hash drift fail without replacing
the previous report. The tool performs no download, extraction or build.

The stable `vertex-candidate` audit observed 38 components, 113 inventory
binaries and 4,162 verified package files. Its packaged runtime manifest hash is
`f7abb108576eae9c089b8b469cc1730d8397a02b9a635269b9715d16950c60ce`,
and `bin/vertex.exe` is
`6221781b06c2a4be8d0c9f4842543fbb9b3fb7a36df98ccc31b32057021aa556`.
These identify this observation; subsequent candidates require a fresh audit.
The generated JSON and official metadata receipt remain ignored under
`artifacts/reset-delivery/dependency-source-closure/`.

| Resolved technical fact | Remaining qualification |
|---|---|
| Exact sources are locally present for 20 component entries. This includes the 12 vcpkg entries, whose installed SPDX resource SHA-512 checksums match cached archives and whose historical recipe/patch SHA-256 checksums match local files. OCCT, Eigen, PROJ and native SQLite retain exact upstream revisions/URLs and recipe option expressions in the report. PlaneGCS retains its FreeCAD commit and modified local source receipt. Bootstrap SQLite, the single-header JSON source and CPython 3.13.15 source archive are also byte-bound. | Local availability does not establish inclusion in the distributable source kit, an actual binary/source derivation, complete embedded-source closure, or an offline rebuild. Recipe expressions and installed status receipts need actual expanded configure/build controls. |
| Official Qt 6.8.3 source archive URLs and SHA-256 checksums are resolved for qtbase, qtsvg and Qt PDF's QtWebEngine source archive. The prebuilt module SBOM revision locators and observed qconfig variables remain separate evidence. | None of those three source archives is in the inspected local caches. The qtpdf SBOM locator alone must not be substituted for the source payload. Exact prebuilt revision/options, PDFium/Chromium source and complete notices still require qualification. |
| Exact official PyPI metadata was observed for all 11 locked wheel project/version pairs. Ten publish source distributions with authoritative archive URLs and SHA-256 checksums; none of these ten source archives is locally cached. IfcOpenShell 0.8.3.post2 publishes no source distribution in that metadata response. | Source distributions do not establish exact wheel/native-library derivation. Resolve the IfcOpenShell wheel binding or qualify the controlled replacement. Retain embedded GEOS, OpenBLAS, OpenSSL, libffi and other source/build/notice obligations. |
| All frozen package files match, and the import-worker's individually listed source files match its source kit. The application and CLI `src` directory aggregate receipts differ from the frozen kit. The current source tree includes generated Python cache content that the allowlist omits. | Root should make source-tree receipts exclude generated/cache files consistently with the source kit, then regenerate the next candidate. Keep this historical mismatch explicit; the audit does not relabel an unmatched aggregate receipt as complete. |
| The remaining 17 source-status entries are the three Qt modules, 11 wheel projects, Inter's unrecorded editable/source payload, and the two unmatched application/CLI aggregate receipts. One additional entry is the MSVC redistributable rights review. | Preserve the existing font/artwork authorization evidence and resolve any required provenance facts. MSVC distributor eligibility and applicable terms remain external facts. URLs and pointer notices never count as delivered source or complete notice text. |

The initial cache observations in the preceding table are historical. The
2026-10-05 acquisition receipt now verifies all three Qt archives and all ten
available PyPI source distributions under `.deps/downloads/source-closure`.
The subsequent audit records 33 locally present sources, four missing source
entries, and one redistributable-rights review for that earlier candidate.
Its detailed receipt is retained at
`artifacts/reset-delivery/dependency-source-closure/source-acquisition.json`.
Local availability has advanced; final runtime/source binding, delivery,
embedded-source obligations and offline rebuilding remain unqualified. Pass
`--cache-dir .deps/downloads/source-closure` when refreshing the audit, because
the default cache scan does not recursively search nested directories.

The [official Qt archive index](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/)
and its [qtbase checksum metadata](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtbase-everywhere-src-6.8.3.tar.xz.mirrorlist),
[qtsvg checksum metadata](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtsvg-everywhere-src-6.8.3.tar.xz.mirrorlist),
and [QtWebEngine checksum metadata](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtwebengine-everywhere-src-6.8.3.tar.xz.mirrorlist)
support the pinned archive receipts. Qt's [PDF build instructions](https://wiki.qt.io/QtPDF_Build_Instructions)
identify QtWebEngine as the PDF source repository and describe disabling the
WebEngine build when building PDF alone. This is upstream guidance, not evidence
that the selected prebuilt 6.8.3 binaries used those exact build options. The
[exact IfcOpenShell PyPI metadata](https://pypi.org/pypi/ifcopenshell/0.8.3.post2/json)
supports the observed absence of an sdist for that release.

Run the audit after selecting and freezing a candidate:

```text
python -B scripts/qualification/dependency_source_closure.py --workspace . --inventory artifacts/runtime/distribution-inventory.json --bundle-root artifacts/packages/vertex-candidate --upstream-metadata artifacts/reset-delivery/dependency-source-closure/upstream-metadata.json --output artifacts/reset-delivery/dependency-source-closure/current-candidate.json
python -B -m unittest discover -s tests -p test_dependency_source_closure.py -v
```

The output directory must already exist. The optional upstream metadata file is
a locally reviewed receipt, not a live network operation; its hash and declared
official response digests are retained. Without it, wheel sources remain
unresolved. Optional `--build-receipt` arguments bind additional exact files;
they do not certify that a receipt produced the selected binary. Exit zero
means the audit completed consistently, including recorded gaps. Both
`licensing_clearance` and `corresponding_source_qualified` remain false for every
component and for the report. The ten focused tests verify receipt integrity,
frozen-source selection, conservative wheel/pointer handling, input boundaries
and atomic failure preservation; they do not certify distribution rights.

## Current OCR source batch (2026-10-06)

The current component manifest declares 12 additional runtime owners for the
14 DLLs reached by the real offline OCR runtime: Tesseract 5.5.2, libarchive
3.8.8, Leptonica 1.87.0, curl 8.22.0, liblzma 5.8.3, LZ4 1.10.0, zstd 1.5.7,
giflib 6.1.3, libjpeg-turbo 3.2.0, libwebp 1.6.0#3 (three DLLs), OpenJPEG 2.5.4
and TIFF 4.7.2. Existing bzip2, zlib and PNG owners cover the shared imports.
OpenSSL is installed but is absent from this OCR runtime closure. Static code
embedded in these libraries still requires its own source/notice review.

All 14 installed DLLs match their installed SPDX file checksums and ownership
relationships. All twelve installed SPDX resource SHA-512 values match existing
archives under `.deps/vcpkg/downloads/`; all 67 declared recipe/patch files match
their installed SPDX SHA-256 values under `.deps/vcpkg/ports/`. The audit records
these exact archives, recipes, upstream locators and recipe option expressions;
it does not download replacements or infer expanded native build options.

Three installed ports conclude `LicenseRef-vcpkg-null`. The manifest retains
that exact expression, and the inventory explicitly exposes `license_concluded`.
Their copyright and SPDX files are pinned as manifest artifacts. The following
observations concern those exact installed notices and are not substituted SPDX
conclusions or redistribution clearance:

| Port | Installed notice observation | SHA-256 of pinned copyright |
|---|---|---|
| Leptonica 1.87.0 | Two-clause BSD redistribution conditions and disclaimer are present. | `87829abb5bbb00b55a107365da89e9a33f86c4250169e5a1e5588505be7d5806` |
| libarchive 3.8.8 | Default two-clause BSD terms plus identified UC Regents terms, public-domain code, triple-licensed files and varying build-script terms. Per-file terms are controlling. | `30e556b3959e3985d66efefec5eaac51d4995053caa1d3cffe6eb916f146f229` |
| liblzma 5.8.3 | The XZ licensing summary identifies the liblzma library as 0BSD and distinguishes command-line/build-system terms. Its four referenced full license texts are exposed separately in the notice inputs; applicability and actual build binding remain unqualified. | `616a3ad264ce29b8f1cb97e53037b139d406899ca8d1f799651e17bfa09830b8` |

The installed copyright files for all three ports are byte-identical to their
members in the exact source archives already included in the composed dependency
source kit. [Native notice provenance](../../third_party/notices/native/README.md)
records their archive identities. This closes a standalone notice-exposure gap,
not a missing-source-archive gap. The selected runtime notice inputs now include
XZ's `COPYING.0BSD`, `COPYING.LGPLv2.1`, `COPYING.GPLv2`, and `COPYING.GPLv3`,
without claiming that the command-line/build-system terms apply to liblzma.dll.

Libarchive's two compress-filter source headers contain complete author and UC
Regents terms absent from its generic installed summary. Those exact header
bytes and the shared BLAKE2 header are separate notice inputs. The BLAKE2 header
offers CC0, OpenSSL or Apache-2.0. Vertex selects Apache-2.0 for the four named
BLAKE2 files, retaining their original grant and full Apache text. The
[GNU GPLv3 guide](https://www.gnu.org/licenses/quick-guide-gplv3.en.html) and
[Apache compatibility statement](https://www.apache.org/licenses/GPL-compatibility.html)
support inclusion of Apache-2.0 code in this GPL-3.0-or-later project route.
This does not replace the installed license-conclusion strings or establish
broader distribution clearance.

Preserved Release compile/link logs include both BLAKE2 reference sources,
both compress filters and the public-domain date parser in the shared library.
Its actual build, installed and package DLL copies have the same SHA-256:
`1463391ebf117034a247f239ac618871f71b120088668ef72b59a828174795f9`.
The named original sources match the pinned libarchive archive. `mtree.5` is
a manpage removed by the exact recipe after installation, absent from the
shared-library link and installed package; its source notice remains in the
delivered archive. No additional applicable standalone notice was found in
these specifically named per-file sources.

The libcrypto.lib named by the same Release link is the installed native
OpenSSL 3.6.4 import library, not static OpenSSL implementation code. Its
import-object/descriptor contents, the DLL's actual normal and absent delayed
imports, generated Windows digest config and pinned CNG cryptor selection
jointly resolve this library's OpenSSL runtime applicability. No OpenSSL runtime
owner is added for archive.dll on that link input. Separate OpenSSL-bearing
components retain their independent version/source/notice records.
Leptonica's complete installed notice needed no duplicate addition in this
bounded review. The original archive retains individual build-script terms.
All three original `LicenseRef-vcpkg-null` expressions and false qualification
states are preserved. Root must regenerate the selected inventory and source kit
to bind these additions to a later packaged candidate.

The fixed GPL engine descriptor is a separate workspace asset. The worker's
source closure now includes `assistance_ocr.cpp/.hpp` and
`assistance_ocr_recognizer.cpp/.hpp`. The English model is a separate Apache-2.0
asset pinned to tessdata_fast 4.1.0 commit
`65727574dfcd264acbb0c3e07860e4e9e9b22185` and SHA-256
`7d4322bd2a7749724879683fc3912cb542f19906c83bcc1a52132556427170b2`.
Both the tracked model and original `.deps/downloads/ocr/eng.traineddata` cache
copy are byte-bound; its LICENSE and upstream README are notice inputs.
The source audit records the exact distributed model as `upstream_asset`,
including frozen-package mapping and a separate `distributed_asset_recorded`
status. Training/generation evidence is not assessed; no training-data delivery
obligation or new application requirement is inferred from the model receipt.

These checks apply to current installed dependency inputs, not the old frozen
candidate. Its 38-component/113-binary evidence above remains historical and
unchanged. Root must generate a new candidate inventory after the final native
payload is selected, stage the complete exact source/notice inputs, resolve the
three unknown conclusions and other embedded/alternative terms, and qualify
the offline rebuild. Distribution, licensing and corresponding-source
qualification remain false; this source batch does not accept D02 or D07.

## Selected MSVC redistributable evidence (2026-10-06)

The five selected release CRT DLLs are byte-identical to the installed Visual
Studio Build Tools 2022 redistributable files. The installation reports version
17.14.37 (build 17.14.37516.0); the selected directory is
`VC/Redist/MSVC/14.44.35112/x64/Microsoft.VC143.CRT`. Each selected DLL reports
file version 14.44.35211.0. This is a comparison with the actual selected files,
not a claim that directory and file version labels must be identical.

| Selected file | SHA-256 |
|---|---|
| msvcp140.dll | `0f885b509a685d2bbfa652fed26b5fb31d88fbdab0a978c641d1c7b8aa460aa9` |
| msvcp140_1.dll | `bfad5aef4c63a669e3c140655cdfdf395b6c979b400a447bd5dcb65ed8826c3d` |
| msvcp140_2.dll | `3ea06f0ee098b4823cb79599df3780e7f23cce52c19aac31d2a0d47efe33a5e9` |
| vcruntime140.dll | `d5e4d9a3e835fa679450145d6a7d94e36573a509317111904d9b3712c30d9066` |
| vcruntime140_1.dll | `1f2d41c4aa5db0bc33ebf7b66d72943a817d7ce6cbe880502a9403823633093f` |

Microsoft's [Visual Studio 2022 redistributable list](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution)
includes unmodified files under the VC redistributable directory and its
subdirectories, excluding debug redistributables. That directory-wide rule
covers the five selected non-debug DLLs. The retained 187-byte `Redist.txt`
is a pointer to that list; its SHA-256 is
`da53b097e02b08e0fc69706102a60bc384fe756426ae4dc4a855e96f95cb2b9c`.
The original 7,043,986-byte third-party notice is also retained, with SHA-256
`782815bd1256f9ad798211eee4b0e574ddd113bd07700c6921ab25c591fbcda7`.
The retained runtime license document is 39,644 bytes, with SHA-256
`f1e3d56ceb2ad68aae0711b910375009e651ac5530fa0760f0dea6e81e54fae1`.

The [app-local redistribution guidance](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files?view=msvc-170)
and [runtime license terms](https://visualstudio.microsoft.com/license-terms/vs2022-cruntime/)
remain the terms sources. Installation metadata alone does not establish the
distributor's licensed-user eligibility or acceptance of applicable terms.
That fact remains unresolved in D02-Q06, and `licensing_clearance` remains
false. This bounded CRT evidence makes no compiler or Windows SDK handoff
rights claim.
