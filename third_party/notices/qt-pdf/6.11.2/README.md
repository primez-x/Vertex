# QtPdf source-inline notices for QtWebEngine 6.11.2

This is a separate snapshot for the QtWebEngine 6.11.2 source
archive. It preserves the historical 6.8.3 notice files unchanged and does not
replace or relabel them.

The snapshot contains 45 distinct leading source-comment slices used by 77
notice-bearing source members, plus the retained observation of one source
member without a found notice. Each of the 77 current source members was read
from the hash-verified 6.11.2 archive. Its full byte count and SHA-256 are in
`notice-index.json`; every indexed offset and inclusive line range was checked
against the staged header bytes. All 45 header files match the historical
tracked copies byte-for-byte and still equal the corresponding 6.11.2 source
slices. No notice was invented for the separate no-notice member.

The index also records the SHA-256 and byte count of PDFium's production
`third_party/BUILD.gn` file and the source README/license-document hashes for
each component. That GN file declares `fx_agg` at line 217 (source README: AGG 2.3, license field MIT), `fx_lcms2` at line 269 (source README: Little CMS 2.15, license field MIT), and
`fx_libopenjpeg` at line 397 (source README: OpenJPEG 2.5.3, license field BSD-2-Clause). The bundled source READMEs identify AGG 2.3, Little CMS
2.15, and OpenJPEG 2.5.3. The OpenJPEG source README identifies BSD-2-Clause;
the 2.5.0 label from the older snapshot must not be carried forward.

These are source-only observations. GN contains conditional system-library
paths, so these declarations and upstream README fields do not establish which
libraries are present in an installed binary. Licensing clearance,
binary/source derivation, shipped-notice closure, and distribution
qualification all remain false in the index.

The source archive is identified by SHA-256
`6101c1aa00ff933d1b65ee5d167f76e8d71b9ac5b378b0111277723ebda7c163`. The component license files and component README files are
recorded as source evidence by path, byte count, and SHA-256; their text is not
rewritten into this notice set.
