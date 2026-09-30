# Candidate IFC kernel: static libraries, matching CPython's dynamic MSVC CRT.
# Release-only preparation does not create or qualify a production runtime.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
