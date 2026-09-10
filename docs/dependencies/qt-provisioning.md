# Project-local Qt 6 runtime

The repository has a project-local Qt SDK for the Windows native desktop build. It is deliberately kept under `.deps/`, which is ignored by Git, so a normal build never modifies a system Qt installation.

| Item | Selection |
| --- | --- |
| Qt | 6.8.3 OSS, Windows Desktop |
| Compiler ABI | `win64_msvc2022_64` (MSVC 2022 x64) |
| Linkage | Dynamic Qt DLLs with MSVC import libraries |
| Build prefix | `.deps/qt/6.8.3/msvc2022_64` |
| Installer | `aqtinstall==3.3.0` in `.deps/tooling/qt-venv` |
| Download cache | `.deps/downloads` |

## Provision or verify

From the repository root, run:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\scripts\bootstrap-qt.ps1
```

The script uses `C:\Program Files\Python312\python.exe` by default, creates only the project-local virtual environment, downloads the pinned Qt archives, and keeps them in `.deps/downloads`. Use `-PythonPath <path>` when Python 3.12 is installed elsewhere. It does not use the Qt online installer or require an account or subscription. To check an already-installed prefix without network access, run:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\scripts\bootstrap-qt.ps1 -VerifyOnly
```

Pass the prefix to CMake with `-DCMAKE_PREFIX_PATH=<repository>/.deps/qt/6.8.3/msvc2022_64`. The existing Build Tools installation provides CMake 3.31.6, Ninja 1.12.1, and MSVC 19.44 x64 under `C:\BuildTools`.

## Package scope

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

## Provenance and source kit

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

The source archive is a provenance and rebuild input; it is not required by the prebuilt SDK bootstrap. The source archive and the installed modules must still be audited as a complete dependency set before production distribution.

## Licensing

Qt's [6.8 licensing page](https://doc.qt.io/qt-6.8/licensing.html) identifies the open-source licensing choices and lists modules that are GPL-only. The requested base modules are available under LGPLv3 or GPLv2; [Qt Widgets](https://doc.qt.io/qt-6.8/qtwidgets-index.html) documents that choice explicitly. [Qt PDF licensing](https://doc.qt.io/qt-6.8/qtpdf-licensing.html) covers the `qtpdf` extension and its PDFium and other third-party notices. Keep the applicable Qt license texts, notices, and corresponding source access with any distributed application; this provisioning record is not a completed legal or third-party license audit.

## Verification evidence

The installed prefix was checked on 2026-09-09:

```text
qmake: Qt 6.8.3
qtpaths --query QT_VERSION: 6.8.3
qtpaths --query QT_INSTALL_PREFIX: <repository>/.deps/qt/6.8.3/msvc2022_64
```

For each requested component, the release `bin/Qt6<Component>.dll`, MSVC `lib/Qt6<Component>.lib`, and `lib/cmake/Qt6<Component>/Qt6<Component>Config.cmake` were present. This verifies the local SDK layout and dynamic linkage artifacts; it does not replace a full application build, runtime deployment test, or dependency-license audit.
