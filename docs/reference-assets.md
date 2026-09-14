# Offline reference asset catalog

`ReferenceAssetCatalog` supplies a storage-independent core for APX-TRACE-001,
APX-TRACE-002, APX-TRACE-003 and IO-PDF-001. Callers supply local bytes, a stable
unique ID, portable project-relative provenance path and MIME type. The catalog
does not open paths or perform network requests. It preserves bytes and SHA-256
across all calibration and display commands. Paths reject traversal, absolute
paths, Windows devices and nonportable reserved characters.

PNG, JPEG, BMP, TIFF and PDF container signatures are recognized by the
storage catalog. PDFs require an explicit one-based page request. This is
deliberately a catalog import, not a renderer: signatures do not prove
decoding success or PDF page existence. The Windows desktop decoder ships
PNG, JPEG, and BMP codecs through Qt and decodes TIFF through the Windows
Imaging Component (WIC), so TIFF underlays work on Windows without requiring a
Qt TIFF plugin. TIFF multi-frame files currently use the first frame; a
non-zero page request remains invalid for raster assets. The decoder reports
failure before an asset is published when the WIC frame cannot be rendered.
The snapshot reports this fidelity limitation and identifies imported content
as tracing references with no editable extraction. The Windows `reference_asset`
entity persists the same declaration with `content_mode`, `editable_extraction`,
`source_preserved`, `fidelity_mode`, `source_format`, and zero-based page
selection/count fields, so a reopened project never has to infer whether an
underlay is editable. A decoder must validate page bounds and renderability
before displaying an underlay.

Calibration records the original known-distance expression, default input unit,
two source-coordinate points and derived metres per source unit. It uses the
existing exact quantity parser. Measurements use source coordinates, so display
scale, rotation, flips, intensity and visibility cannot change measured geometry.
`set_transform` and `calibrate` each have one independent undo step. Source bytes
are exposed only through const catalog access. Import itself has no undo command.

Versioned JSON snapshots sort records by stable ID, contain source bytes, and
preserve all calibration and transform metadata. Restore validates signatures,
paths, IDs, byte ranges, source hash and recalculated calibration. Undo history
is session-local. A caller can store the snapshot in its own persistence layer;
this module does not yet wire the catalog to project storage, Qt controls,
rendering, editable PDF extraction, or traced geometry creation.

`reference_asset_tests` checks the 100 mm fixture before and after transformed
JSON save/reopen, immutable source bytes, independent undo, atomic failed
commands, format signatures, unsafe paths, duplicate IDs and corrupted snapshots.
These are core-level tests; they do not establish end-user workflow completion.
