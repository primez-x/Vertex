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

The current inspected closure contains 42 application/component binaries and
has no unresolved static imports on this development machine. In addition to
the directly linked OCCT toolkits, recursion finds TKShHealing, TKHLR, TKMesh,
FreeType, libpng, zlib, Brotli and bzip2. Windows operating-system libraries are
recorded as system dependencies, not copied from System32 for distribution.
The five required MSVC runtime DLLs are instead selected from an explicitly
prepared, hash-pinned x64 SDK redistributable payload. `inspect-runtime.ps1`
searches that payload before System32; system alternatives are recorded
separately from local packaging candidates.

## Qt plugin boundary

Deployment uses the versioned component inventory and explicit plugin list,
not an unrestricted SDK scan. The current runtime includes QtCore, QtGui,
QtWidgets, QtPrintSupport, QtPdf, QtSvg, and QtNetwork as resolved by PE imports.
The plugin set is Windows platform/style plus GIF, ICO and JPEG image plugins.
`packaging/qt.conf` installs beside the executable and points to `../plugins`;
the installed application must not depend on the developer's `QT_PLUGIN_PATH`.
Library presence does not establish network activity or complete dynamic-load
coverage. Earlier recipes excluding PDF/SVG modules no longer describe this
application's dependency closure.

The installed SDK lacks translation catalogs, and deployment inspection reports
missing DXC compiler files. The actual graphics fallback/runtime policy,
compiler redistributable, complete notices and corresponding-source kit must
be resolved and tested in the offline installer work.

Static import resolution is not proof of a complete dynamic-load inventory,
clean-machine installation, offline operation, license clearance or commercial
redistributability. The production dependency audit remains incomplete.

## Distribution inventory work

The current PE closure covers the application/CLI/PlaneGCS, seven Qt libraries
and five Qt plugins, sixteen OCCT toolkits, six FreeType/compression libraries,
and five MSVC DLLs. PE imports do not identify static/header contributions from
SQLite, nlohmann-json, Eigen or Boost. The distribution inventory records those
inputs separately, alongside the font, assistance asset, Qt configuration and
PlaneGCS provenance. This is source ownership evidence, not proof of a
reproducible binary-to-source build.

Pinned vcpkg packages provide local `share/<package>/vcpkg.spdx.json` and
`copyright` evidence. A source/notice kit must retain the selected packages and
their transitives, including the selected FreeType license alternative. Qt's
installed SDK is broader than the runtime allowlist; its exact notices, bundled
third-party sources and future PDF-input components need separate composition
checks. No blanket Qt or IfcOpenShell ecosystem license assumption is permitted.

The distribution inventory maps shipped binary hashes and static/header inputs
to package versions, source/provenance and notice files. This inventory remains
distinct from proof that distribution obligations and offline installation have
been satisfied.

After the runtime evidence and ownership report are current, use the
[Windows offline bundle](offline-installer.md) staging command to carry the
hash-checked runtime set, source-kit files, and dependency/license metadata to
another machine. The bundle verifier checks declared bytes; it does not turn
static import evidence into clean-machine or network-denied runtime evidence.
