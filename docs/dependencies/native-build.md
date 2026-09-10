# Native library build

The source dependencies for Open CASCADE, Eigen and Boost are pinned by
`vcpkg.json` and the matching registry commit in
`third_party/dependencies.json`. Prepare them explicitly:

```powershell
.\scripts\bootstrap-native.ps1
```

The default compilation cache is `C:\Build\PropertyStudio\vcpkg-build`.
OCCT's long generated filenames exceeded Windows compiler path limits inside
this deeply nested checkout. The short compilation path addresses that build
failure without altering OCCT geometry code. Override it with `-BuildRoot` if
needed. Installed libraries remain in `.deps/native/x64-windows`; archives and
vcpkg source remain in `.deps/vcpkg`.

The script checks the vcpkg revision, disables its metrics, and limits native
compilation to 16 jobs. `-Offline` forbids new source downloads and requires
the manager, tools and source archives to be cached already. A clean-machine
offline source-kit build still needs separate qualification.

This manifest selects OCCT 8.0.1, Eigen 5.0.1 and Boost 1.92.0 from the pinned
registry. The extracted PlaneGCS solver compiles and its guarded adapter tests
pass with this Eigen version; see [solver qualification](planegcs.md).
Optional OCCT FreeImage, VTK, TBB and RapidJSON features
are disabled. FreeType and its transitive dependencies are retained for native
visualization. The complete shipped license closure remains under audit.

Compile the application-specific architecture engine after provisioning:

```powershell
.\scripts\build.ps1 -Architecture
```

Normal CMake builds use the prepared local libraries and do not invoke vcpkg
installation or fetch sources.
