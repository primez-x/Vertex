# Property Studio

Working name for an independent, offline Windows property-measurement and
residential/light-commercial architectural application.

**Development checkpoint — not a production release or a certified Apex
replacement.** The accepted scope requires Apex parity, compatibility, both
workspaces, full architectural authoring, modern enhancements, and assisted
workflows to pass one production acceptance gate together.

## Build

The application uses C++20, Visual Studio 2022 Build Tools with the x64 C++
workload, Windows SDK, CMake/Ninja, and Python 3.12. From PowerShell in this
directory, prepare pinned dependencies explicitly, then build:

```powershell
python scripts/bootstrap.py
.\scripts\bootstrap-qt.ps1
.\scripts\bootstrap-native.ps1
.\scripts\build.ps1 -Desktop
# Optimized configuration:
.\scripts\build.ps1 -Desktop -Configuration Release
```

The bootstrap steps download source/packages explicitly. Once prepared,
configuration, compilation, tests, and application use run locally. The core
bootstrap accepts `--offline` with a populated cache. A complete redistributable
offline build kit/installer remains part of the active production work.

`build.ps1` discovers the installed compiler and runs CTest. Without `-Desktop`,
it builds only the precision/document/storage core and CLI. CAD dependencies
use a short dedicated build cache because some upstream generated paths exceed
Windows path limits; see [native build notes](docs/dependencies/native-build.md)
and [Qt provisioning](docs/dependencies/qt-provisioning.md).

For a focused build, pass multiple target names as a PowerShell array and run
the matching tests explicitly. Space-separated trailing arguments are rejected
so a build cannot silently omit requested targets:

```powershell
.\scripts\build.ps1 -Desktop -Configuration Release -SkipTests -Targets @('boundary_entity_tests', 'boundary_integrity_tests')
```

Launch the current development application with `.\scripts\run.ps1` (Debug) or
`.\scripts\run.ps1 -Configuration Release`. This sets process-local DLL/plugin
paths; it does not install runtimes globally. Source builds produce
`build/windows-debug/property-studio.exe` and `property-cli.exe`.

Explicit native checks use `scripts/test-native.ps1` and
`scripts/test-desktop.ps1`; their windows stay hidden and failures have timeouts.
The visual harnesses run under both Windows PowerShell 5.1 and PowerShell 7.
The latter saves visual captures for inspection in `artifacts/desktop-smoke`.
See [runtime inspection](docs/dependencies/runtime-inspection.md) for the
current binary/plugin inventory and its packaging limitations.

The reviewed runtime inventory, source-kit manifest, and portable allowlist
can be composed into a deterministic Windows offline bundle with
`scripts/stage_offline_bundle.py`; see [offline bundle instructions](docs/dependencies/offline-installer.md).

The [project format](docs/project-format.md), [calculation contract](docs/calculations.md),
[workspace UI](docs/workspace-ui.md),
[accepted production plan](docs/production-plan.md), and
[implementation status](docs/implementation-status.md) distinguish implemented
behavior from the remaining production acceptance requirements.

The current navigator uses the project's actual buildings, floors and layers.
Use Commands to add or rename these containers, then choose a Drawing layer
for new geometry. Both workspaces share that context. Floor/layer checkboxes
filter both plans and 3D without changing totals; Show all clears the filters.
Floor association currently leaves world elevations unchanged.
See [project organization](docs/project-organization.md) for the tested boundary.

The source is private. Third-party components retain their licenses; see
`LICENSE` and the component notices. No source publication or product release
is implied by this repository.
