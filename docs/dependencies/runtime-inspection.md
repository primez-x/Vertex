# Runtime inspection

Run `scripts/inspect-runtime.ps1` after the Release build. It recursively reads
PE dependencies with the installed MSVC `dumpbin`, records every inspected
binary's SHA256, and fails on unresolved imports. Results are local build
artifacts at `artifacts/runtime/release-imports.json`.

The entry points are the desktop executable, CLI, replaceable PlaneGCS DLL,
Windows platform/style plugins and GIF/ICO/JPEG image plugins. A component's
presence in this inventory alone does not prove the desktop calls it. The v15
desktop import table now directly includes `property_planegcs.dll` through the
constraint-authoring service, so that DLL is a required local runtime component.
Explicit entry points include separately built components and dynamically
loaded plugins that an executable import table alone cannot discover.

The current inspected closure contains 34 application/component binaries and
has no unresolved static imports on this development machine. In addition to
the directly linked OCCT toolkits, recursion finds TKShHealing, TKHLR, TKMesh,
FreeType, libpng, zlib, Brotli and bzip2. Installed Windows/MSVC runtimes are
recorded as system dependencies, not copied from System32 for distribution.

## Qt plugin boundary

A plain Qt 6.8.3 `windeployqt --dry-run --json` expands this executable's four
Qt libraries into additional PDF, SVG and network libraries/plugins. Deployment
must therefore use an explicit allowlist. This checked dry-run recipe selects
the libraries/plugins currently used by the desktop checkpoint:

```powershell
& .\.deps\qt\6.8.3\msvc2022_64\bin\windeployqt.exe `
  --dry-run --json --no-translations --no-compiler-runtime `
  --no-network --no-pdf --no-svg `
  --exclude-plugins 'qtuiotouchplugin,qpdf,qsvg,qsvgicon' `
  --skip-plugin-types 'tls,networkinformation' `
  build\windows-release\property-studio.exe
```

This is inspection only. PDF tracing and other planned adapters still require
their own qualified components; excluding unused plugins from this checkpoint
does not remove those features from the production plan. QPdfWriter output
uses QtGui and does not itself require the QtPdf input/rendering module.

The installed SDK lacks translation catalogs, and deployment inspection reports
missing DXC compiler files. The actual graphics fallback/runtime policy,
compiler redistributable, complete notices and corresponding-source kit must
be resolved and tested in the offline installer work.

Static import resolution is not proof of a complete dynamic-load inventory,
clean-machine installation, offline operation, license clearance or commercial
redistributability. The production dependency audit remains incomplete.

## Distribution inventory work

The current PE closure covers the application, four Qt libraries and five Qt
plugins, sixteen OCCT toolkits, and six FreeType/compression libraries. It does
not identify static/header contributions from SQLite, nlohmann-json, Eigen or
Boost, or bind their source identities to the compiled application. Those inputs
must be added to the packaging inventory alongside the PlaneGCS source snapshot.

Pinned vcpkg packages provide local `share/<package>/vcpkg.spdx.json` and
`copyright` evidence. A source/notice kit must retain the selected packages and
their transitives, including the selected FreeType license alternative. Qt's
installed SDK is broader than the runtime allowlist; its exact notices, bundled
third-party sources and future PDF-input components need separate composition
checks. No blanket Qt or IfcOpenShell ecosystem license assumption is permitted.

The next packaging artifact must map each shipped binary hash and static/header
input to its exact package, version, source/provenance and notice files. This
inventory must remain distinct from proof that distribution obligations and
offline installation have been satisfied.

After the runtime evidence and ownership report are current, use the
[Windows offline bundle](offline-installer.md) staging command to carry the
hash-checked runtime set, source-kit files, and dependency/license metadata to
another machine. The bundle verifier checks declared bytes; it does not turn
static import evidence into clean-machine or network-denied runtime evidence.
