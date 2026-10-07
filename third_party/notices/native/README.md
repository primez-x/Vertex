# Native notice provenance

These notice inputs supplement the exact installed vcpkg copyright summaries.
They record source evidence; they do not establish a final license conclusion,
distribution-rights clearance, actual build applicability or corresponding-source
qualification. Original installed `LicenseRef-vcpkg-null` declarations remain
unchanged for Leptonica 1.87.0, libarchive 3.8.8 and liblzma 5.8.3.

The selected source archives already contain the original full source notices.
This directory exposes a bounded set separately to binary notice packaging.
The generated selected inventory/source kit must be refreshed after integration.

| Component | Exact archive SHA-256 | Installed copyright SHA-256 |
|---|---|---|
| leptonica 1.87.0 | `fa2b40c5caea96d1bb93a97486262aed8731b69ce25a84a6bf5d25323e33f631` | `87829abb5bbb00b55a107365da89e9a33f86c4250169e5a1e5588505be7d5806` |
| libarchive 3.8.8 | `528f9c91e11238cbb5ce6d79b20fa3bb48a5cd124008036af1913d84fc5ba420` | `30e556b3959e3985d66efefec5eaac51d4995053caa1d3cffe6eb916f146f229` |
| liblzma 5.8.3 | `8ec1767fa517642ecb4cf08b891ce667ba6f143551e382b07c7ef437bda335e2` | `616a3ad264ce29b8f1cb97e53037b139d406899ca8d1f799651e17bfa09830b8` |

XZ 5.8.3's [pinned COPYING summary](https://raw.githubusercontent.com/tukaani-project/xz/v5.8.3/COPYING)
lists four separate license files. The files here are unchanged archive-member
bytes. Their presence does not assign the summary's command-line, getopt or
build-system terms to liblzma.dll. The summary identifies liblzma as 0BSD.

Libarchive 3.8.8's [pinned COPYING summary](https://raw.githubusercontent.com/libarchive/libarchive/v3.8.8/COPYING)
says the per-file statements control. The two compress-filter notice files
contain their complete initial copyright, conditions and disclaimer comments,
including both the source authors and UC Regents. `BLAKE2-notice.txt` is the
identical complete initial comment in all four `archive_blake2*` files named
in COPYING. It offers CC0, OpenSSL or Apache-2.0 at the recipient's option.
`Apache-2.0.txt` is the unmodified [primary Apache text](https://www.apache.org/licenses/LICENSE-2.0.txt)
linked by that header. Vertex selects the Apache-2.0 alternative for these four
BLAKE2 files. The [GNU GPLv3 guide](https://www.gnu.org/licenses/quick-guide-gplv3.en.html)
and [Apache's compatibility statement](https://www.apache.org/licenses/GPL-compatibility.html)
confirm Apache-2.0 code may be included in GPLv3 projects. This selection fits
Vertex's GPL-3.0-or-later project route and retains the original BLAKE2 grant,
copyright and full Apache text. It does not replace libarchive's original
`LicenseRef-vcpkg-null` declaration or certify broader distribution rights.

Preserved Release Ninja compile/link records include `archive_blake2s_ref.c`,
`archive_blake2sp_ref.c`, both compress-filter files and `archive_parse_date.c`
in the shared `archive.dll` target. The actual build, installed and vcpkg package
DLLs are byte-identical: 783,872 bytes, SHA-256
`1463391ebf117034a247f239ac618871f71b120088668ef72b59a828174795f9`.
The inspected original sources match their pinned archive members. The Release
cache has `ENABLE_LIBB2=OFF`, so the BLAKE2 reference sources supply that code.
The two compress-filter notices and selected BLAKE2 terms therefore apply to
this selected library's compiled source inputs.

`mtree.5` is documentation, not a compiled source in the shared-library link
rule. CMake's install record places it under `share/man/man5`; the exact recipe
removes `share/man`, and the final installed file list and package omit it.
Its complete UC Regents notice stays in the delivered original source archive;
there is no additional mtree runtime notice gap for this selected DLL.
`archive_parse_date.c` is compiled and explicitly public domain in its source
header. Its original statement stays in the source archive. Build-script terms
remain with their unchanged source files and are not relabeled as DLL terms.

The Release link rule also names native OpenSSL 3.6.4's `libcrypto.lib`, but
the exact library is import-only: 5,895 short import objects plus three COFF
descriptor objects containing `.idata` and `.debug` sections, without `.text`.
Its hash matches installed OpenSSL SPDX. The selected DLL has neither a
libcrypto/OpenSSL import nor a delay-import directory. The generated config
selects Windows digest backends, and the pinned cryptor source selects the
Windows CNG branch before its OpenSSL fallback. These combined build and byte
facts establish that this libcrypto link input does not embed OpenSSL
implementation code in the selected archive.dll. No additional OpenSSL runtime
owner or notice is inferred for it. Other separately delivered OpenSSL-bearing
components retain their own version/source/notice evidence.

These bounded facts do not qualify every transitive component, a final package,
an offline rebuild or the overall distribution. Existing false licensing and
corresponding-source qualification states are preserved.

Leptonica 1.87.0's [pinned leptonica-license.txt](https://raw.githubusercontent.com/DanBloomberg/leptonica/1.87.0/leptonica-license.txt)
is byte-identical to the installed copyright input already declared for the
component; no duplicate copy is added.

| Added notice | Bytes | SHA-256 | Archive member / primary source |
|---|---:|---|---|
| [liblzma/5.8.3/COPYING.0BSD](liblzma/5.8.3/COPYING.0BSD) | 607 | `0b01625d853911cd0e2e088dcfb743261034a091bb379246cb25a14cc4c74bf1` | `xz-5.8.3/COPYING.0BSD` |
| [liblzma/5.8.3/COPYING.LGPLv2.1](liblzma/5.8.3/COPYING.LGPLv2.1) | 26419 | `20e50fe7aae3e56378ebf0417d9de904f55a0e61e4df315333e632a4d3555d95` | `xz-5.8.3/COPYING.LGPLv2.1` |
| [liblzma/5.8.3/COPYING.GPLv2](liblzma/5.8.3/COPYING.GPLv2) | 17984 | `edaef632cbb643e4e7a221717a6c441a4c1a7c918e6e4d56debc3d8739b233f6` | `xz-5.8.3/COPYING.GPLv2` |
| [liblzma/5.8.3/COPYING.GPLv3](liblzma/5.8.3/COPYING.GPLv3) | 35149 | `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986` | `xz-5.8.3/COPYING.GPLv3` |
| [libarchive/3.8.8/archive_read_support_filter_compress-notice.txt](libarchive/3.8.8/archive_read_support_filter_compress-notice.txt) | 3280 | `c719c519be76313715a04d05cda3b5c4a8780f314c4450efeb91e0082db6c7c8` | `libarchive-3.8.8/libarchive/archive_read_support_filter_compress.c` |
| [libarchive/3.8.8/archive_write_add_filter_compress-notice.txt](libarchive/3.8.8/archive_write_add_filter_compress-notice.txt) | 3114 | `af142a8794509bf428fe86930b110ab1ec618cd5a6f971c5c0aa86ff91d5f985` | `libarchive-3.8.8/libarchive/archive_write_add_filter_compress.c` |
| [libarchive/3.8.8/BLAKE2-notice.txt](libarchive/3.8.8/BLAKE2-notice.txt) | 602 | `73a0d7fdb4e59a58e29244e0322fd50c7d388b77754458a2be78a2a8c47760d8` | `libarchive-3.8.8/libarchive/archive_blake2.h` |
| [libarchive/3.8.8/Apache-2.0.txt](libarchive/3.8.8/Apache-2.0.txt) | 11358 | `cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30` | `https://www.apache.org/licenses/LICENSE-2.0.txt` |

Header copies retain exact bytes from byte zero through the last initial
comment and following blank lines, before the first C preprocessor directive.
No code, notice wording, comment marks or line endings were rewritten.
