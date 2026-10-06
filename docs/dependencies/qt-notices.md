# Qt 6.8.3 notice materialization

`scripts/qualification/materialize_qt_notices.py` creates a fresh, deterministic
notice payload from the three checksum-pinned official release archives already
cached in `.deps/downloads/source-closure`. It does not download anything, execute
archive code, extract the complete source tree, or qualify distribution rights.
The original archives are preserved.

The selected route for Qt library code offering that alternative is
`LGPL-3.0-only`; Vertex remains `GPL-3.0-or-later`. This uses the original LGPL
version 3 text and accompanying GPL version 3 text. It does not relabel Qt as
LGPL "or later", discard third-party alternatives, apply LGPL to tools or other
code that does not offer it, or establish that the selected build satisfies the
route's obligations. The original module alternatives and third-party notices
remain available in the archive and notice payload.

The component manifest retains the existing package license declaration
alternatives. The selected route is recorded in the notice index and `NOTICE.txt`;
notice materialization alone does not establish a candidate-specific license
choice for every package or third-party dependency.

[Qt PDF licensing](https://doc.qt.io/qt-6.8/qtpdf-licensing.html) identifies an
LGPLv3 option and separate PDFium/third-party obligations. The published 6.8
documentation currently describes a later patch release; the pinned 6.8.3 source
bytes and selected binary evidence must govern this candidate's qualification.
[Qt licensing](https://doc.qt.io/qt-6.8/licensing.html) describes module-specific
alternatives and third-party code. These references provide context, not a
candidate-specific clearance finding.

## Exact inputs and observed selection

The script pins each official URL, archive length and SHA-256 in code, matching
the retained source-acquisition receipt. There is no CLI checksum override.

| Archive | SHA-256 | Selected files | Selected bytes |
| --- | --- | ---: | ---: |
| qtbase-everywhere-src-6.8.3.tar.xz | `56001b905601bb9023d399f3ba780d7fa940f3e4861e496a7c490331f49e0b80` | 158 | 3,799,550 |
| qtsvg-everywhere-src-6.8.3.tar.xz | `35eb516460f00f264eb504baa253432384351cf23fb9980a5857190e8deef438` | 12 | 175,425 |
| qtwebengine-everywhere-src-6.8.3.tar.xz | `df4e19ba2b3a540551b6f998d62597377ffa688c1cff564589b7da2e2bf87337` | 2,210 | 6,665,417 |

Read-only in-memory inspection on 2026-10-06 checked the exact pinned archives,
including 25,704 QtBase, 641 QtSVG and 270,769 QtWebEngine entries. No final notice
directory was materialized by that inspection. The 2,380 selected files total
10,640,392 bytes; the 3.37 GB expanded QtWebEngine source tree is never copied.

Root subsequently materialized `.deps/qt-notices/6.8.3` successfully. The actual
command and exit 0 are recorded in
`artifacts/reset-delivery/qt-notice-materialization-20261006-01.json` (with its
companion `.log`). Root verification and manifest integration checked all 2,380
original files (10,640,392 bytes) and the generated 674-byte `NOTICE.txt` against
the index's sizes and SHA-256 hashes. The actual `notice-index.json` SHA-256 is
`4d2e4a6f9eb6c190a938dc26a996c8edcf0f60d69ba0be4c877672ef09d1af9c`.
This establishes materialization and byte identity; qualification remains open.

The selection retains original bytes for:

- Every top-level `LICENSES/` entry and every filename identifying a license,
  copying, copyright, notice or author record across each archive.
- Every `qt_attribution.json`, its `LicenseFile(s)` and `CopyrightFile(s)`
  references, and explicit `Files` source references when no separate license
  file is declared. Those few source files retain embedded notices as inert
  originals. Qt's literal newlines in attribution strings are supported.
- Every `README.chromium` and `README.pdfium`, plus resolvable `License File`
  references, including nonstandard names such as `FTL.TXT`.
- Original `.tag` values and `CHROMIUM_VERSION` as source identity evidence.
  The GNU LGPLv3/GPLv3 texts, QtSVG `src/svg/XSVG_LICENSE.txt`, Chromium licenses
  and `src/3rdparty/chromium/third_party/pdfium/LICENSE` must be present/nonempty.

This intentionally includes notices for unshipped platforms, examples, tests and
build tools. It is a conservative source-notice superset. It does not assert that
every selected component is linked into the shipped binaries, or that naming and
attribution references discover every embedded source notice.

## Fresh output and receipts

The output parent must already exist and the output itself must not exist, even
as an empty directory. Root integration created the parent and ran the
materialization command below. The completed output is preserved and must not be
reused as the target of another materialization run:

```text
python -B scripts/qualification/materialize_qt_notices.py --cache-dir .deps/downloads/source-closure --output-root .deps/qt-notices/6.8.3
python -B -m unittest discover -s tests -p test_materialize_qt_notices.py -v
```

Original bytes are stored under short `texts/<module>/<number>.txt` paths to avoid
Windows filename/path constraints. `notice-index.json` maps each output to its
full original archive member, exact archive URL/hash, content size/SHA-256 and
selection reasons. The index contains no absolute workspace paths or timestamps.
`NOTICE.txt` explains the chosen library route and the qualification boundary;
its own content hash is in the index. The index is written last.

Every archive entry is checked, including unselected entries. Traversal,
absolute/drive/stream paths, Windows reserved names, duplicate/case collisions,
file/directory conflicts, symlinks, hardlinks, reparse points, sparse/special files
and configured size/count limits are rejected. Filesystem archive hardlinks and
input changes during inspection are also rejected. Inputs are validated before
the output is exclusively created; unknown or concurrently created output is
preserved. Output files use exclusive creation. A write failure can leave a
partial fresh output for diagnosis; it is never deleted or reused automatically.
A missing index indicates an incomplete run. Root should verify every delivered
output hash before integrating it into the candidate; the completed root and
manifest-integration verification establishes the current payload's hashes.

`third_party/distribution-components.json` now preserves the existing Qt SPDX
notice paths and adds every indexed original text to its owning component:
158 QtBase texts under `qtbase`, 12 QtSVG texts under `qtsvg`, and the 2,210
QtWebEngine/PDFium notice-superset texts under `qtpdf`. The shared `NOTICE.txt`
and `notice-index.json` are declared once under the always-shipped `qtbase`
dependency, with no duplicate paths within a component. The portable allowlist
keeps all indexed texts together under `licenses/qt-notices/6.8.3/`, so the
shared index's relative paths remain usable. The next distribution inventory
records these file paths/hashes; dependency source composition can copy their
original bytes with that evidence.
This manifest inclusion does not establish delivery in the current frozen
candidate. Root must refresh the affected inventory/source composition and
verify the eventual delivered payload at the next appropriate candidate boundary.

## Remaining D02-Q02 evidence

The installed `qtpdf-6.8.3.spdx.json` `Pdf` package explicitly warns that consumed
third-party dependencies are omitted. It contains seven packages and no PDFium
dependency package. QtSVG's SBOM identifies XSVG (`HPND-sell-variant`) and an
external QtBase Zlib dependency; its notice is retained from the pinned source.
QtBase's SBOM describes bundled/static components and relationships beyond the
top-level module license. The original SPDX records remain useful evidence and
should accompany the added actual texts.

The completed index records 100 unresolved QtWebEngine references; the earlier
read-only inspection recorded 105 unresolved `README` references. Current gaps
include external/ambiguous pointers and license paths pruned from the release
archive, such as an ANGLE OpenCL loader notice. The index preserves each metadata
member and exact reference; it never substitutes a URL for text or fetches an
unpinned license. Some refer to platforms/tools absent from this Windows/PDF
build, but that applicability requires evidence from the actual shipped closure.
Root must resolve applicable gaps or document candidate-bound exclusions.

A bounded follow-up separates 97 non-PDFium references from three PDFium
references. PDFium's CPU-features `src/LICENSE` and Ninja's remote `COPYING`
pointers both have `Shipped: no` in their original metadata. The Fuchsia SDK
`sdk/LICENSE` pointer has `Shipped: yes`, although its description concerns
building and testing on Fuchsia. The selected release archive lacks the exact
CPU-features and Fuchsia SDK license paths. Another CPU-features copy's Apache
text and an openscreen Ninja `COPYING` are retained, but their different paths
do not establish the missing copies' identities. These findings guide the
Windows build-closure review; they do not yet remove any unresolved reference
or substitute an unrelated notice.

The Windows closure review adds bounded technical evidence for three possible
candidate exclusions. Chromium `DEPS` sets `checkout_fuchsia = False` (lines
74–80), and the SDK checkout hook is conditional (lines 5101–5111); this supports
a Windows-specific Fuchsia SDK exclusion, subject to the actual build closure.
The PDFium CPU-features metadata says `Shipped: no`, while its `BUILD.gn` still
contains `impl_x86_windows.c` (lines 18–35) and the aggregate
`third_party/BUILD.gn` has no `cpu_features` edge. This supports only a
provisional exclusion pending actual build-closure evidence. Ninja's README
marks it `Shipped: no` as a build tool; Chromium `DEPS` pins its version (lines
507–510), and no Ninja binary appears in the selected Qt PDF runtime inventory.
These facts support a runtime-exclusion recommendation while leaving build-source
obligations separate. The pinned QtWebEngine archive is SHA-256
`df4e19ba2b3a540551b6f998d62597377ffa688c1cff564589b7da2e2bf87337` and
566,553,436 bytes. The selected qtpdf SBOM records Windows AMD64;
the selected-source receipt binds `Qt6Pdf.dll` by SHA-256
`b1e59033fbfe86080d914050c7674ff312e57b0f39aaa1adeed46a5b3e9b564d` and
5,337,736 bytes. These receipts identify the reviewed source and binary; they do
not establish derivation or complete build closure. The evidence does not resolve
the other 97 references, establish complete notice coverage, or alter the qtpdf
SBOM warning that consumed third-party dependencies are omitted.

The retained archive `.tag` files exactly match the source revision strings in
the selected prebuilt module SBOMs: QtBase
`c07c2d5a527a644d36e7853d55132ae38921682f`, QtSVG
`c75099a75e81df35e7347a65e301bbf1011831a5`, and QtWebEngine/PDF
`b586c4eb65d8e46ab2c255e1a141676043a650da`. The qtpdf SBOM spells its repository
locator `qtpdf.git`; the release source archive is QtWebEngine. Qt's
[PDF build instructions](https://wiki.qt.io/QtPDF_Build_Instructions) place the
PDF source in QtWebEngine. The matching tags and that source location support
using the selected release archive as the vendor-declared versioned source
payload; this is an inference from those original records, not an independently
verified binary rebuild. Keep the original SBOM locator unchanged. Its configure
comments record stated Windows build options; actual expanded static/transitive
closure and rebuild evidence remain separate requirements.

Actual materialization, payload byte verification and component-manifest notice
inclusion are complete. Root still owns candidate/source-kit delivery, review of
applicable static/transitive notice and alternative-license choices, final
runtime/options/revision binding, corresponding-source delivery and offline
rebuild verification. All licensing, shipped-notice-closure, binary/source,
corresponding-source, rebuild and distribution qualification flags stay false.
