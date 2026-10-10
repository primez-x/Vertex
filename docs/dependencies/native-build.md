# Native library build

The source dependencies for Open CASCADE, Eigen, Boost, and PROJ are pinned by
`vcpkg.json` and the matching registry commit in
`third_party/dependencies.json`. Prepare them explicitly:

```powershell
.\scripts\bootstrap-native.ps1
```

The default compilation cache is `C:\Build\Vertex\vcpkg-build`.
OCCT's long generated filenames exceeded Windows compiler path limits inside
this deeply nested checkout. The short compilation path addresses that build
failure without altering OCCT geometry code. Override it with `-BuildRoot` if
needed. Installed libraries remain in `.deps/native/x64-windows`; archives and
vcpkg source remain in `.deps/vcpkg`.

The script checks the vcpkg revision, disables its metrics, and limits native
compilation to 16 jobs. `-Offline` forbids new source downloads and requires
the manager, tools and source archives to be cached already. A clean-machine
offline source-kit build still needs separate qualification.

This manifest selects OCCT 8.0.1, Eigen 5.0.1, Boost 1.92.0, and PROJ 9.8.1 from the pinned
registry. The extracted PlaneGCS solver compiles and its guarded adapter tests
pass with this Eigen version; see [solver qualification](planegcs.md).
Optional OCCT FreeImage, VTK, TBB and RapidJSON features
are disabled. FreeType and its transitive dependencies are retained for native
visualization. PROJ is built with `default-features: false`; its local `proj_9.dll`,
`proj.db`, and `proj.ini` are the only georeferencing runtime inputs currently
declared for packaging. The complete shipped license closure remains under audit.

Compile the shared headless engine and CLI after provisioning:

```powershell
.\scripts\build.ps1 -Architecture
```

`-Architecture` is accepted for compatibility; the same headless engine is now
the default when `-Desktop` is omitted. It includes exact area/deduction checks
used while restoring saved workspace histories, so native dependencies remain
required even for the CLI. This configuration uses OCCT, PROJ and the
Eigen/PlaneGCS dependencies while excluding Qt and the desktop visualization
host. `SKETCH_BUILD_ARCHITECTURE=OFF` together with
`SKETCH_BUILD_DESKTOP=OFF` is an invalid configuration and reports the dependency
before compilation.

Headless Debug/Release presets have separate directories under
`build/windows-headless-*`. Desktop builds continue using `build/windows-*`.
Use `scripts/run-cli.ps1 -Configuration Release -ApplicationArguments
@('inspect', 'example.bldproj')` for a source-built headless CLI; the runner
temporarily supplies the pinned native DLL directory and restores the caller's
PATH after it exits. Packaged executables retain their bundled runtime layout.

Normal CMake builds use the prepared local libraries and do not invoke vcpkg
installation or fetch sources.

The precision engine explicitly depends on Boost Multiprecision 1.92.0 through
the pinned `boost-multiprecision` port and `Boost::multiprecision` CMake target.
Its header-only rational backend supports bounded curve-contact certification;
it adds no runtime DLL or online service. The distribution component manifest
names the retained port SPDX record and BSL-1.0 notice separately from Boost
Graph. Changed-source compilation and offline source-kit qualification remain
required; previous package evidence does not qualify this dependency change.
