# Project-local Qt 6 runtime

The repository has a project-local Qt SDK for the Windows native desktop build. It is deliberately kept under `.deps/`, which is ignored by Git, so a normal build never modifies a system Qt installation.

| Item | Selection |
| --- | --- |
| Qt | 6.11.2 OSS, Windows Desktop |
| Compiler ABI | `win64_msvc2022_64` (MSVC 2022 x64) |
| Linkage | Dynamic Qt DLLs with MSVC import libraries |
| Build prefix | `.deps/qt/6.11.2/msvc2022_64` |
| Installer | Public upstream aqt revision `076e1659807d0b362a3ed684d54c2e9c775eb9c7` in `.deps/tooling/qt-6.11.2-venv` |
| Download cache | `.deps/downloads` |

## Provision or verify

From the repository root, run:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\scripts\bootstrap-qt.ps1
```

If installation or repair is needed, the script uses `C:\Program Files\Python312\python.exe` by default, creates only the project-local virtual environment, and keeps the pinned Qt archives in `.deps/downloads`. Use `-PythonPath <path>` when Python 3.12 is installed elsewhere. It does not use the Qt online installer or require an account or subscription. To check an already-installed prefix without network access, run:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\scripts\bootstrap-qt.ps1 -VerifyOnly
```

The normal command checks for a complete prepared SDK before requiring Python or setting up the aqt tooling environment. If that SDK is complete, it skips tooling setup and installation. If it is missing or incomplete, the normal command requires Python and installs or repairs the pinned SDK. To explicitly require an existing complete SDK and prohibit installation or network fallback, run:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\scripts\bootstrap-qt.ps1 -Offline
```

`-Offline` verifies only the prepared binary SDK and fails if any required Qt files are missing. It does not require Python and never provisions a missing or incomplete SDK.

The authoritative selection is [qt-sdk.json](../../third_party/qt-sdk.json).
Build, runtime, inspection and desktop checks read that same selection. CMake
requires its exact version and imported prefix; a global or stale SDK cannot
satisfy it. Use the documented Windows build script, or pass
`-DCMAKE_PREFIX_PATH=<repository>/.deps/qt/6.11.2/msvc2022_64;<repository>/.deps/native/x64-windows`.
When changing an existing Qt SDK selection, configure once with `cmake --fresh`
in the same persistent build directory to clear component cache entries.

The official 6.11 repository separates Windows architecture metadata. Released
aqt 3.3.0 requests the older folder layout and fails to select this SDK. Bootstrap
uses the exact public [upstream installer revision](https://github.com/miurahr/aqtinstall/commit/076e1659807d0b362a3ed684d54c2e9c775eb9c7),
with source ZIP SHA-256
`8c42874261a8e1769728da9305626b9f227b59d08b291b3bf79115b22f73d6ad`.
This is an unreleased installer-tool revision; Qt itself is the unmodified
public SDK. Original SPDX metadata hashes and all eight required component
DLLs, import libraries and CMake configs are checked after installation.

## Selected 6.11.2 inputs

Only `qtbase`, `qtsvg` and the separate `qtpdf` extension are selected. They
provide Core, Gui, Widgets, OpenGL, OpenGLWidgets, PrintSupport, Pdf and Svg.
The exact SDK build is `6.11.2-0-202608131017`, Windows 11 24H2, MSVC 2022 x64.
Archive URLs, publisher SHA-1, measured SHA-256, sizes and original SPDX hashes
are recorded in the selection file. The corresponding source archive pins are:

| Source module | SHA-256 |
| --- | --- |
| QtBase | `5b2e00eccaf5a4d8c14134ffa0ea8dfd0a35ae1ffc7f8d87fa4305a1ed23cf22` |
| QtSVG | `d594337feca84c26fb67fe87b85e6a5c12fda404b611d905f9d138210c311876` |
| QtWebEngine, containing the PDF sources | `6101c1aa00ff933d1b65ee5d167f76e8d71b9ac5b378b0111277723ebda7c163` |

Source tags match the selected SDK revision locators: QtBase
`ef55f427f2c8b410d34f8a7681020a3000cf6866`, QtSVG
`17ca512f903f935282ebeca496aac5d11ba4199a`, and QtWebEngine/Pdf
`a33fa2a897e5ee58e385b3f88dc247d99fca56db`. SDK and source archives are retained
independently. QtWebEngine source inclusion does not select its browser runtime.

The selected Qt library route is LGPL-3.0-only; Vertex remains
GPL-3.0-or-later. The actual Core, Pdf and Svg SPDX module records offer that
route. Qt documents its [licensing alternatives](https://doc.qt.io/qt-6.11/licensing.html)
and the [PDFium and other PDF notices](https://doc.qt.io/qt-6.11/qtpdf-licensing.html).
The materialized source notice index and retained preferred sources accompany
distribution preparation; they do not certify binary source derivation, rights,
clean offline rebuilding or application behavior. See [qt-notices.md](qt-notices.md).

## Historical 6.8.3 provisioning record

The following evidence describes the preserved earlier SDK. Its hashes,
licensing metadata and observed verification are not relabelled as 6.11.2.

### Package scope

The installation was selected from aqt's Qt 6.8.3 Windows Desktop metadata. The pinned [aqtinstall 3.3.0 release](https://github.com/miurahr/aqtinstall/releases/tag/v3.3.0) and its [CLI documentation](https://aqtinstall.readthedocs.io/en/latest/cli.html) are the installer provenance:

```text
qtbase qtsvg qtpdf
```

These provide the requested CMake components: `Core`, `Gui`, `Widgets`, `OpenGL`, `OpenGLWidgets`, `PrintSupport`, `Pdf`, and `Svg`. `qtpdf` is the only optional Qt extension installed. No GPL-only optional module such as Charts, Data Visualization, Graphs, MQTT, or WebEngine was requested. The base archive already provides the qmake, moc, rcc, and uic build tools needed for this project.

The aqt package metadata was queried with:

```powershell
\.deps\tooling\qt-venv\Scripts\aqt.exe list-qt windows desktop --spec 6.8.3
\.deps\tooling\qt-venv\Scripts\aqt.exe list-qt windows desktop --arch 6.8.3
\.deps\tooling\qt-venv\Scripts\aqt.exe list-qt windows desktop --modules 6.8.3 win64_msvc2022_64
\.deps\tooling\qt-venv\Scripts\aqt.exe list-qt windows desktop --archives 6.8.3 win64_msvc2022_64
```

The observed results included version `6.8.3`, architecture `win64_msvc2022_64`, extension `qtpdf`, and the selected base archives `qtbase` and `qtsvg`. aqt's official command documentation explains `--archives` as the way to limit base-package scope and `--modules` as the way to add an extension.

### Provenance and source kit

The binary archives are fetched from Qt's public SDK repository by aqt, with the official mirror hash verification left enabled. The retained files and their SHA-256 values from the provisioned cache are:

| Archive | SHA-256 |
| --- | --- |
| `qtbase-Windows-Windows_11_23H2-MSVC2022-Windows-Windows_11_23H2-X86_64.7z` | `41688269fac0565db956c66d9eecae777d16197e0c02cd81b88640c1f5d73d3f` |
| `qtsvg-Windows-Windows_11_23H2-MSVC2022-Windows-Windows_11_23H2-X86_64.7z` | `48d1d798894cbf8696a0010fcfdeafea652b35159c56ea2060ee85ce3d0048f7` |
| `qtpdf-Windows-Windows_11_23H2-MSVC2022-Windows-Windows_11_23H2-X86_64.7z` | `db722373b94a46019f10812de94c418f8bcff3e333883e3c43351f96e18579f6` |

For an offline source kit, use the official [Qt 6.8.3 source directory](https://download.qt.io/official_releases/qt/6.8/6.8.3/single/) and download [qt-everywhere-src-6.8.3.zip](https://download.qt.io/official_releases/qt/6.8/6.8.3/single/qt-everywhere-src-6.8.3.zip). Qt's published [SHA-256 file](https://download.qt.io/official_releases/qt/6.8/6.8.3/single/qt-everywhere-src-6.8.3.zip.sha256) records:

```text
51acbdb32aa5e74cd8f7a2b30acef90739369ad10b665a2d4dd7ba446c1069b0
```

The source archive is a provenance and rebuild input; it is not required by the prebuilt SDK bootstrap. `-Offline` verifies this prepared binary SDK only. It does not verify that the source archive or transitive dependency sources are complete, and it does not prove a full offline source rebuild. The source archive and the installed modules must still be audited as a complete dependency set before production distribution.

### Licensing

Qt's [6.8 licensing page](https://doc.qt.io/qt-6.8/licensing.html) identifies the open-source licensing choices and lists modules that are GPL-only. The requested base modules are available under LGPLv3 or GPLv2; [Qt Widgets](https://doc.qt.io/qt-6.8/qtwidgets-index.html) documents that choice explicitly. [Qt PDF licensing](https://doc.qt.io/qt-6.8/qtpdf-licensing.html) covers the `qtpdf` extension and its PDFium and other third-party notices. Keep the applicable Qt license texts, notices, and corresponding source access with any distributed application; this provisioning record is not a completed legal or third-party license audit.

### Verification evidence

The installed prefix was checked on 2026-09-09:

```text
qmake: Qt 6.8.3
qtpaths --query QT_VERSION: 6.8.3
qtpaths --query QT_INSTALL_PREFIX: <repository>/.deps/qt/6.8.3/msvc2022_64
```

For each requested component, the release `bin/Qt6<Component>.dll`, MSVC `lib/Qt6<Component>.lib`, and `lib/cmake/Qt6<Component>/Qt6<Component>Config.cmake` were present. This verifies the local SDK layout and dynamic linkage artifacts; it does not replace a full application build, runtime deployment test, or dependency-license audit.
