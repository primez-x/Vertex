# Qt PDF 6.8.3 embedded original notices

These texts preserve the contiguous leading original source comments from the
AGG 2.3, Little CMS 2.15 and OpenJPEG 2.5.0 production sources named in the pinned
PDFium `third_party/BUILD.gn`. They supplement the original README metadata, which
names licenses without a separate License File reference. Original copyright,
grant and disclaimer wording is retained; no publisher SPDX declaration is
replaced. The AGG source grant is retained as written even though its README
labels it MIT.

The exact QtWebEngine 6.8.3 archive is 566,553,436 bytes, SHA-256
`df4e19ba2b3a540551b6f998d62597377ffa688c1cff564589b7da2e2bf87337`.
Its original source remains the complete archive. `notice-index.json` records
the official URL, original member hash/length, zero-based source byte offset,
one-based inclusive line bounds, and notice slice hash/length. Forty-five
distinct comment slices cover 77 named source/header members (27 AGG, 28 Little
CMS, 22 OpenJPEG). One additional GN-listed generated table header contains no
copyright/grant phrase and is recorded without an invented notice.

The source recipe connects QtPdf to PDFium; its production graph contains
`fx_agg`, `fx_lcms2`, and `fx_libopenjpeg` targets. The exact installed QtPdf
private config disables V8, XFA and XFA image codecs. This bounds source-recipe
applicability, while the selected publisher binary's complete expanded compile
and link inputs remain unobserved. These original source notices are a
conservative supplement, not independent proof of binary derivation, complete
shipped-notice coverage, license clearance, corresponding-source qualification,
or offline rebuild. All qualification fields remain false.

This OpenJPEG is PDFium's modified embedded 2.5.0 copy. It is distinct from the
separately inventoried native OpenJPEG runtime and its notice/source records.
